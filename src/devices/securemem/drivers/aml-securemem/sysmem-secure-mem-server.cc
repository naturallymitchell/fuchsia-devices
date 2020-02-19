// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sysmem-secure-mem-server.h"

#include <lib/async-loop/default.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/fit/defer.h>

#include <cinttypes>

#include <safemath/safe_math.h>

#include "log.h"

SysmemSecureMemServer::SysmemSecureMemServer(thrd_t ddk_dispatcher_thread,
                                             zx::channel tee_client_channel)
    : ddk_dispatcher_thread_(ddk_dispatcher_thread),
      tee_client_channel_(std::move(tee_client_channel)),
      loop_(&kAsyncLoopConfigNoAttachToCurrentThread) {
  ZX_DEBUG_ASSERT(tee_client_channel_);
}

SysmemSecureMemServer::~SysmemSecureMemServer() {
  ZX_DEBUG_ASSERT(thrd_current() == ddk_dispatcher_thread_);
  ZX_DEBUG_ASSERT(!is_protect_memory_range_active_);
  ZX_DEBUG_ASSERT(is_loop_done_ || !was_thread_started_);
  if (was_thread_started_) {
    // Call StopAsync() first, and only do ~SysmemSecureMemServer() when secure_mem_server_done has
    // been called.
    ZX_DEBUG_ASSERT(loop_.GetState() == ASYNC_LOOP_QUIT);
    ZX_DEBUG_ASSERT(is_loop_done_);
    // EnsureLoopDone() was already previously called.
    ZX_DEBUG_ASSERT(!secure_mem_server_done_);
    loop_.JoinThreads();
    // This will cancel the wait, which will run EnsureLoopDone(), but since is_loop_done_ is
    // already true, the EnsureLoopDone() that runs here won't do anything.  This call to Shutdown()
    // is necessary to complete the llcpp unbind.
    loop_.Shutdown();
  }
}

zx_status_t SysmemSecureMemServer::BindAsync(zx::channel sysmem_secure_mem_server,
                                             thrd_t* sysmem_secure_mem_server_thread,
                                             SecureMemServerDone secure_mem_server_done) {
  ZX_DEBUG_ASSERT(sysmem_secure_mem_server);
  ZX_DEBUG_ASSERT(sysmem_secure_mem_server_thread);
  ZX_DEBUG_ASSERT(secure_mem_server_done);
  ZX_DEBUG_ASSERT(thrd_current() == ddk_dispatcher_thread_);
  zx_status_t status = loop_.StartThread("sysmem_secure_mem_server_loop", &loop_thread_);
  if (status != ZX_OK) {
    LOG(ERROR, "loop_.StartThread() failed - status: %d", status);
    return status;
  }
  was_thread_started_ = true;
  // This probably goes without saying, but it's worth pointing out that the loop_thread_ must be
  // separate from the ddk_dispatcher_thread_ so that TEEC_* calls made on the loop_thread_ can be
  // served by the ddk_dispatcher_thread_ without deadlock.
  ZX_DEBUG_ASSERT(ddk_dispatcher_thread_ != loop_thread_);
  closure_queue_.SetDispatcher(loop_.dispatcher(), loop_thread_);
  *sysmem_secure_mem_server_thread = loop_thread_;
  secure_mem_server_done_ = std::move(secure_mem_server_done);
  PostToLoop([this, sysmem_secure_mem_server = std::move(sysmem_secure_mem_server)]() mutable {
    ZX_DEBUG_ASSERT(thrd_current() == loop_thread_);
    zx_status_t status = fidl::Bind<SysmemSecureMemServer>(
        loop_.dispatcher(), std::move(sysmem_secure_mem_server), this,
        [this](SysmemSecureMemServer* sysmem_secure_mem_server) {
          // This can get called from the ddk_dispatcher_thread_ if
          // we're doing loop_.Shutdown() to unbind an llcpp server (so
          // far the only way to unbind an llcpp server).  However, in
          // this case the call to EnsureLoopDone() will idempotently do
          // nothing because is_loop_done_ is already true.
          ZX_DEBUG_ASSERT(thrd_current() == loop_thread_ || is_loop_done_);
          // If secure_mem_server_done_ still set by this point, !is_success.
          EnsureLoopDone(false);
        });
    if (status != ZX_OK) {
      LOG(ERROR, "fidl::Bind() failed - status: %d", status);
      ZX_DEBUG_ASSERT(secure_mem_server_done_);
      EnsureLoopDone(false);
    }
  });
  return ZX_OK;
}

void SysmemSecureMemServer::StopAsync() {
  // The only way to unbind an llcpp server is to Shutdown() the loop, but before we can do that we
  // have to Quit() the loop.
  ZX_DEBUG_ASSERT(thrd_current() == ddk_dispatcher_thread_);
  ZX_DEBUG_ASSERT(was_thread_started_);
  PostToLoop([this] {
    ZX_DEBUG_ASSERT(thrd_current() == loop_thread_);
    // Stopping the loop intentionally is considered is_success, if that happens before channel
    // failure.  EnsureLoopDone() is idempotent so it'll early out if already called previously.
    EnsureLoopDone(true);
  });
}

void SysmemSecureMemServer::GetPhysicalSecureHeaps(
    llcpp::fuchsia::sysmem::SecureMem::Interface::GetPhysicalSecureHeapsCompleter::Sync completer) {
  ZX_DEBUG_ASSERT(thrd_current() == loop_thread_);
  llcpp::fuchsia::sysmem::PhysicalSecureHeaps heaps;
  zx_status_t status = GetPhysicalSecureHeapsInternal(&heaps);
  if (status != ZX_OK) {
    LOG(ERROR, "GetPhysicalSecureHeapsInternal() failed - status: %d", status);
    completer.ReplyError(status);
    return;
  }
  completer.ReplySuccess(std::move(heaps));
}

void SysmemSecureMemServer::SetPhysicalSecureHeaps(
    llcpp::fuchsia::sysmem::PhysicalSecureHeaps heaps,
    llcpp::fuchsia::sysmem::SecureMem::Interface::SetPhysicalSecureHeapsCompleter::Sync completer) {
  ZX_DEBUG_ASSERT(thrd_current() == loop_thread_);
  llcpp::fuchsia::sysmem::SecureMem_SetPhysicalSecureHeaps_Result result;
  auto complete = fit::defer([&completer, &result] {
    ZX_DEBUG_ASSERT(!result.has_invalid_tag());
    completer.Reply(std::move(result));
  });
  zx_status_t status = SetPhysicalSecureHeapsInternal(heaps);
  if (status != ZX_OK) {
    LOG(ERROR, "SetPhysicalSecureHeapsInternal() failed - status: %d", status);
    result.set_err(&status);
    return;
  }
  llcpp::fuchsia::sysmem::SecureMem_SetPhysicalSecureHeaps_Response response;
  result.set_response(&response);
}

void SysmemSecureMemServer::PostToLoop(fit::closure to_run) {
  // For now this is only expected to be called from ddk_dispatcher_thread_.
  ZX_DEBUG_ASSERT(thrd_current() == ddk_dispatcher_thread_);
  closure_queue_.Enqueue(std::move(to_run));
}

zx_status_t SysmemSecureMemServer::TrySetupSecmemClientSession() {
  ZX_DEBUG_ASSERT(thrd_current() == loop_thread_);
  // We only try this once; if it doesn't work the first time, it's very
  // unlikely to work on retry anyway, and this avoids some retry complexity.
  if (!is_setup_secmem_client_session_attempted_) {
    is_setup_secmem_client_session_attempted_ = true;
    // This is skipping over TEEC_InitializeContext(), which doesn't presently
    // have a way to specify a pre-existing FIDL channel, or any way to connect
    // that's suitable to establish a loopback connection.  It's fine that we're
    // skipping is_global_platform_compliant(); we already know that's true.
    //
    // While we could do this on the thread that calls the constructor,
    // TEEC_InitializeContext() would want to be called from the loop_thread_,
    // so do this here in case we switch to TEEC_InitializeContext().
    //
    // TODO(dustingreen): Regardless of whether we split tee-client-api into an
    // outer and inner lib, it'd be good for the tee-client-api (inner or only)
    // to have an entry point to init a context from a channel.  Unfortunately
    // that entry point is outside the spec the other TEEC_ calls are following
    // but that's probably ok.  Or, we could use tee.Device directly without
    // using any of the tee-client-api.
    ZX_DEBUG_ASSERT(tee_client_channel_);
    context_.imp.tee_channel = tee_client_channel_.release();
    ZX_DEBUG_ASSERT(!secmem_client_session_);
    secmem_client_session_.emplace(&context_);
    setup_secmem_client_session_status_ = secmem_client_session_->Init();
  }
  return setup_secmem_client_session_status_;
}

void SysmemSecureMemServer::EnsureLoopDone(bool is_success) {
  if (is_loop_done_) {
    return;
  }
  // We can't assert this any sooner, because when unbinding llcpp server using loop_.Shutdown()
  // we'll be on ddk_dispatcher_thread_.  But in that case, the first run of EnsureLoopDone()
  // happened on the loop_thread_.
  ZX_DEBUG_ASSERT(thrd_current() == loop_thread_);
  is_loop_done_ = true;
  closure_queue_.StopAndClear();
  loop_.Quit();
  if (is_setup_secmem_client_session_attempted_) {
    ZX_DEBUG_ASSERT(thrd_current() == loop_thread_);
    ZX_DEBUG_ASSERT(secmem_client_session_);
    if (is_protect_memory_range_active_) {
      TEEC_Result tee_status =
          secmem_client_session_->ProtectMemoryRange(protect_start_, protect_length_, false);
      if (tee_status != TEEC_SUCCESS) {
        LOG(ERROR, "secmem_sesson_.ProtectMemoryRange(false) failed - tee_status: %d", tee_status);
        ZX_PANIC("secmem_sesson_.ProtectMemoryRange(false) failed - tee_status: %d", tee_status);
      }
      is_protect_memory_range_active_ = false;
    }
    secmem_client_session_.reset();
    TEEC_FinalizeContext(&context_);
  } else {
    // We could be running on the loop_thread_ or the ddk_dispatcher_thread_ in this case.
    ZX_DEBUG_ASSERT(!secmem_client_session_);
    ZX_DEBUG_ASSERT(context_.imp.tee_channel == ZX_HANDLE_INVALID);
  }
  if (secure_mem_server_done_) {
    secure_mem_server_done_(is_success);
  }
}

zx_status_t SysmemSecureMemServer::GetPhysicalSecureHeapsInternal(
    llcpp::fuchsia::sysmem::PhysicalSecureHeaps* heaps) {
  ZX_DEBUG_ASSERT(thrd_current() == loop_thread_);

  if (is_get_physical_secure_heaps_called_) {
    LOG(ERROR, "GetPhysicalSecureHeaps may only be called at most once - reply status: %d",
        ZX_ERR_BAD_STATE);
    return ZX_ERR_BAD_STATE;
  }
  is_get_physical_secure_heaps_called_ = true;

  zx_status_t status = TrySetupSecmemClientSession();
  if (status != ZX_OK) {
    LOG(ERROR, "TrySetupSecmemClientSession() failed - status: %d", status);
    return status;
  }

  uint64_t vdec_phys_base;
  size_t vdec_size;
  status = SetupVdec(&vdec_phys_base, &vdec_size);
  if (status != ZX_OK) {
    LOG(ERROR, "SetupVdec failed - status: %d", status);
    return status;
  }

  heaps->heaps_count = 1;
  heaps->heaps[0].heap = llcpp::fuchsia::sysmem::HeapType::AMLOGIC_SECURE_VDEC;
  heaps->heaps[0].physical_address = vdec_phys_base;
  heaps->heaps[0].size_bytes = static_cast<uint64_t>(vdec_size);
  return ZX_OK;
}

zx_status_t SysmemSecureMemServer::SetPhysicalSecureHeapsInternal(
    llcpp::fuchsia::sysmem::PhysicalSecureHeaps heaps) {
  ZX_DEBUG_ASSERT(thrd_current() == loop_thread_);
  if (is_set_physical_secure_heaps_called_) {
    LOG(ERROR, "SetPhysicalSecureHeaps may only be called at most once - reply status: %d",
        ZX_ERR_BAD_STATE);
    return ZX_ERR_BAD_STATE;
  }
  is_set_physical_secure_heaps_called_ = true;

  zx_status_t status = TrySetupSecmemClientSession();
  if (status != ZX_OK) {
    LOG(ERROR, "TrySetupSecmemClientSession() failed - status: %d", status);
    return status;
  }

  // This implementation is amlogic-specific; we expect exactly 1 heap which is
  // AMLOGIC_SECURE, only.
  if (heaps.heaps_count != 1) {
    LOG(ERROR, "heaps.heaps_count != 1");
    return ZX_ERR_INVALID_ARGS;
  }
  const llcpp::fuchsia::sysmem::PhysicalSecureHeap& heap = heaps.heaps[0];
  if (heap.heap != llcpp::fuchsia::sysmem::HeapType::AMLOGIC_SECURE) {
    LOG(ERROR, "heap != AMLOGIC_SECURE");
    return ZX_ERR_INVALID_ARGS;
  }

  status = ProtectMemoryRange(heap.physical_address, heap.size_bytes);
  if (status != ZX_OK) {
    LOG(ERROR, "ProtectMemoryRange() failed - status: %d", status);
    return status;
  }

  LOG(INFO, "Succeeded protecting memory range 0x%lx 0x%lx", heap.physical_address,
      heap.size_bytes);

  return ZX_OK;
}

zx_status_t SysmemSecureMemServer::SetupVdec(uint64_t* physical_address, size_t* size_bytes) {
  ZX_DEBUG_ASSERT(thrd_current() == loop_thread_);
  ZX_DEBUG_ASSERT(is_setup_secmem_client_session_attempted_);
  ZX_DEBUG_ASSERT(setup_secmem_client_session_status_ == ZX_OK);
  uint32_t start;
  uint32_t length;
  TEEC_Result tee_status = secmem_client_session_->AllocateSecureMemory(&start, &length);
  if (tee_status != TEEC_SUCCESS) {
    LOG(ERROR, "secmem_client_session_.AllocateSecureMemory() failed - tee_status: %" PRIu32,
        tee_status);
    return ZX_ERR_INTERNAL;
  }
  *physical_address = start;
  *size_bytes = length;
  return ZX_OK;
}

zx_status_t SysmemSecureMemServer::ProtectMemoryRange(uint64_t physical_address,
                                                      size_t size_bytes) {
  ZX_DEBUG_ASSERT(thrd_current() == loop_thread_);
  ZX_DEBUG_ASSERT(is_setup_secmem_client_session_attempted_);
  ZX_DEBUG_ASSERT(setup_secmem_client_session_status_ == ZX_OK);
  if (!safemath::IsValueInRangeForNumericType<uint32_t>(physical_address)) {
    LOG(ERROR, "heap.physical_address > 0xFFFFFFFF");
    return ZX_ERR_INVALID_ARGS;
  }
  if (!safemath::IsValueInRangeForNumericType<uint32_t>(size_bytes)) {
    LOG(ERROR, "heap.size_bytes > 0xFFFFFFFF");
    return ZX_ERR_INVALID_ARGS;
  }
  if (!safemath::CheckAdd(physical_address, size_bytes).IsValid<uint32_t>()) {
    LOG(ERROR, "start + size overflow");
    return ZX_ERR_INVALID_ARGS;
  }
  uint32_t start = static_cast<uint32_t>(physical_address);
  uint32_t length = static_cast<uint32_t>(size_bytes);
  TEEC_Result tee_status = secmem_client_session_->ProtectMemoryRange(start, length, true);
  if (tee_status != TEEC_SUCCESS) {
    LOG(ERROR, "secmem_sesson_.ProtectMemoryRange() failed - tee_status: %d returning status: %d",
        tee_status, ZX_ERR_INTERNAL);
    return ZX_ERR_INTERNAL;
  }

  // Stash these so we can clean up later during DdkSuspend(), so mexec can work.
  protect_start_ = start;
  protect_length_ = length;
  is_protect_memory_range_active_ = true;

  return ZX_OK;
}
