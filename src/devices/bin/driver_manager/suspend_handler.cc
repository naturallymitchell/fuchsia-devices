// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "suspend_handler.h"

#include <lib/fdio/directory.h>
#include <zircon/syscalls/system.h>

#include <inspector/inspector.h>

#include "coordinator.h"
#include "driver_host.h"
#include "src/devices/lib/log/log.h"

namespace {

constexpr char kFshostAdminPath[] = "/svc/fuchsia.fshost.Admin";

fidl::Client<llcpp::fuchsia::fshost::Admin> ConnectToFshostAdminServer(
    async_dispatcher_t* dispatcher) {
  zx::channel local, remote;
  zx_status_t status = zx::channel::create(0, &local, &remote);
  if (status != ZX_OK) {
    return fidl::Client<llcpp::fuchsia::fshost::Admin>();
  }
  status = fdio_service_connect(kFshostAdminPath, remote.release());
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to connect to fuchsia.fshost.Admin: %s", zx_status_get_string(status));
    return fidl::Client<llcpp::fuchsia::fshost::Admin>();
  }
  return fidl::Client<llcpp::fuchsia::fshost::Admin>(std::move(local), dispatcher);
}

void SuspendFallback(const zx::resource& root_resource, uint32_t flags) {
  LOGF(INFO, "Suspend fallback with flags %#08x", flags);
  if (flags == DEVICE_SUSPEND_FLAG_REBOOT) {
    zx_system_powerctl(root_resource.get(), ZX_SYSTEM_POWERCTL_REBOOT, nullptr);
  } else if (flags == DEVICE_SUSPEND_FLAG_REBOOT_BOOTLOADER) {
    zx_system_powerctl(root_resource.get(), ZX_SYSTEM_POWERCTL_REBOOT_BOOTLOADER, nullptr);
  } else if (flags == DEVICE_SUSPEND_FLAG_REBOOT_RECOVERY) {
    zx_system_powerctl(root_resource.get(), ZX_SYSTEM_POWERCTL_REBOOT_RECOVERY, nullptr);
  } else if (flags == DEVICE_SUSPEND_FLAG_POWEROFF) {
    zx_system_powerctl(root_resource.get(), ZX_SYSTEM_POWERCTL_SHUTDOWN, nullptr);
  }
}

void DumpSuspendTaskDependencies(const SuspendTask* task, int depth = 0) {
  ZX_ASSERT(task != nullptr);

  const char* task_status = "";
  if (task->is_completed()) {
    task_status = zx_status_get_string(task->status());
  } else {
    bool dependence = false;
    for (const auto* dependency : task->Dependencies()) {
      if (!dependency->is_completed()) {
        dependence = true;
        break;
      }
    }
    task_status = dependence ? "<dependence>" : "Stuck <suspending>";
    if (!dependence) {
      zx_koid_t pid = task->device().host()->koid();
      if (!pid) {
        return;
      }
      zx::unowned_process process = task->device().host()->proc();
      char process_name[ZX_MAX_NAME_LEN];
      zx_status_t status = process->get_property(ZX_PROP_NAME, process_name, sizeof(process_name));
      if (status != ZX_OK) {
        strlcpy(process_name, "unknown", sizeof(process_name));
      }
      printf("Backtrace of threads of process %lu:%s\n", pid, process_name);
      inspector_print_debug_info_for_all_threads(stdout, process->get());
      fflush(stdout);
    }
  }
  LOGF(INFO, "%*cSuspend %s: %s", 2 * depth, ' ', task->device().name().data(), task_status);
  for (const auto* dependency : task->Dependencies()) {
    DumpSuspendTaskDependencies(reinterpret_cast<const SuspendTask*>(dependency), depth + 1);
  }
}

}  // namespace

SuspendHandler::SuspendHandler(Coordinator* coordinator, bool suspend_fallback,
                               zx::duration suspend_timeout)
    : coordinator_(coordinator),
      suspend_fallback_(suspend_fallback),
      suspend_timeout_(suspend_timeout) {
  fshost_admin_client_ = ConnectToFshostAdminServer(coordinator_->dispatcher());
}

void SuspendHandler::Suspend(uint32_t flags, SuspendCallback callback) {
  // The sys device should have a proxy. If not, the system hasn't fully initialized yet and
  // cannot go to suspend.
  if (!coordinator_->sys_device()->proxy()) {
    LOGF(ERROR, "Aborting system-suspend, system is not fully initialized yet");
    if (callback) {
      callback(ZX_ERR_UNAVAILABLE);
    }
    return;
  }

  // A suspend is already in progress.
  if (InSuspend()) {
    LOGF(ERROR, "Aborting system-suspend, a system suspend is already in progress");
    if (callback) {
      callback(ZX_ERR_ALREADY_EXISTS);
    }
    return;
  }

  flags_ = Flags::kSuspend;
  sflags_ = flags;
  suspend_callback_ = std::move(callback);

  if ((sflags() & DEVICE_SUSPEND_REASON_MASK) != DEVICE_SUSPEND_FLAG_SUSPEND_RAM) {
    log_to_debuglog();
    LOGF(INFO, "Shutting down filesystems to prepare for system-suspend");
    ShutdownFilesystems([this](zx_status_t status) { SuspendAfterFilesystemShutdown(); });
    return;
  }
  // If we don't have to shutdown the filesystems we can just call this directly.
  SuspendAfterFilesystemShutdown();
}

void SuspendHandler::SuspendAfterFilesystemShutdown() {
  LOGF(INFO, "Filesystem shutdown complete, creating a suspend timeout-watchdog\n");
  auto watchdog_task = std::make_unique<async::TaskClosure>([this] {
    if (!InSuspend()) {
      return;  // Suspend failed to complete.
    }
    LOGF(ERROR, "Device suspend timed out, suspend flags: %#08x", sflags());
    if (task() != nullptr) {
      DumpSuspendTaskDependencies(task());
    }
    if (suspend_fallback_) {
      SuspendFallback(coordinator_->root_resource(), sflags());
      // Unless in test env, we should not reach here.
      if (suspend_callback_) {
        suspend_callback_(ZX_ERR_TIMED_OUT);
      }
    }
  });
  suspend_watchdog_task_ = std::move(watchdog_task);
  zx_status_t status =
      suspend_watchdog_task_->PostDelayed(coordinator_->dispatcher(), suspend_timeout_);
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to create timeout watchdog for suspend: %s\n",
         zx_status_get_string(status));
  }
  auto completion = [this](zx_status_t status) {
    suspend_watchdog_task_->Cancel();
    if (status != ZX_OK) {
      // TODO: unroll suspend
      // do not continue to suspend as this indicates a driver suspend
      // problem and should show as a bug
      LOGF(ERROR, "Failed to suspend: %s", zx_status_get_string(status));
      flags_ = SuspendHandler::Flags::kRunning;
      if (suspend_callback_) {
        suspend_callback_(status);
      }
      return;
    }
    if (sflags() != DEVICE_SUSPEND_FLAG_MEXEC) {
      // should never get here on x86
      // on arm, if the platform driver does not implement
      // suspend go to the kernel fallback
      SuspendFallback(coordinator_->root_resource(), sflags());
      // if we get here the system did not suspend successfully
      flags_ = SuspendHandler::Flags::kRunning;
    }

    if (suspend_callback_) {
      suspend_callback_(ZX_OK);
    }
  };
  // We don't need to suspend anything except sys_device and it's children,
  // since we do not run suspend hooks for children of test or misc

  task_ = SuspendTask::Create(coordinator_->sys_device(), sflags(), std::move(completion));
  LOGF(INFO, "Successfully created suspend task on device 'sys'");
}

void SuspendHandler::ShutdownFilesystems(fit::callback<void(zx_status_t)> callback) {
  auto callback_ptr = std::make_shared<fit::callback<void(zx_status_t)>>(std::move(callback));

  auto result = fshost_admin_client_->Shutdown(
      [callback_ptr](llcpp::fuchsia::fshost::Admin::ShutdownResponse* response) {
        LOGF(INFO, "Successfully waited for VFS exit completion\n");
        if (*callback_ptr) {
          (*callback_ptr)(ZX_OK);
        }
      });
  if (result.status() != ZX_OK) {
    LOGF(WARNING,
         "Failed to cause VFS exit ourselves, this is expected during orderly shutdown: %s",
         zx_status_get_string(result.status()));
    if (*callback_ptr) {
      (*callback_ptr)(ZX_OK);
    }
  }
}
