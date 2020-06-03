// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device.h"

#include <fuchsia/device/manager/c/fidl.h>
#include <fuchsia/device/manager/llcpp/fidl.h>
#include <fuchsia/driver/test/c/fidl.h>
#include <fuchsia/io/c/fidl.h>
#include <lib/fidl/coding.h>
#include <lib/zx/clock.h>
#include <zircon/status.h>

#include <memory>

#include <ddk/driver.h>
#include <fbl/auto_call.h>

#include "coordinator.h"
#include "devfs.h"
#include "fidl.h"
#include "fidl_txn.h"
#include "init_task.h"
#include "resume_task.h"
#include "src/devices/lib/log/log.h"
#include "suspend_task.h"

// TODO(fxb/43370): remove this once init tasks can be enabled for all devices.
static constexpr bool kEnableAlwaysInit = false;

Device::Device(Coordinator* coord, fbl::String name, fbl::String libname, fbl::String args,
               fbl::RefPtr<Device> parent, uint32_t protocol_id, zx::channel client_remote,
               bool wait_make_visible)
    : coordinator(coord),
      name_(std::move(name)),
      libname_(std::move(libname)),
      args_(std::move(args)),
      parent_(std::move(parent)),
      protocol_id_(protocol_id),
      publish_task_([this] { coordinator->HandleNewDevice(fbl::RefPtr(this)); }),
      client_remote_(std::move(client_remote)),
      wait_make_visible_(wait_make_visible) {
  test_reporter = std::make_unique<DriverTestReporter>(name_);
  inspect_.emplace(coord->inspect_manager().devices(), coord->inspect_manager().device_count(),
                   name_.c_str());
  set_state(Device::State::kActive);  // set default state
}

Device::~Device() {
  // Ideally we'd assert here that immortal devices are never destroyed, but
  // they're destroyed when the Coordinator object is cleaned up in tests.
  // We can probably get rid of the IMMORTAL flag, since if the Coordinator is
  // holding a reference we shouldn't be able to hit that check, in which case
  // the flag is only used to modify the proxy library loading behavior.

  devfs_unpublish(this);

  // Drop our reference to our driver_host if we still have it
  set_host(nullptr);

  std::unique_ptr<Metadata> md;
  while ((md = metadata_.pop_front()) != nullptr) {
    if (md->has_path) {
      // return to published_metadata list
      coordinator->AppendPublishedMetadata(std::move(md));
    } else {
      // metadata was attached directly to this device, so we release it now
    }
  }

  // TODO: cancel any pending rpc responses
  // TODO: Have dtor assert that DEV_CTX_IMMORTAL set on flags
  VLOGF(1, "Destroyed device %p '%s'", this, name_.data());
}

zx_status_t Device::Create(Coordinator* coordinator, const fbl::RefPtr<Device>& parent,
                           fbl::String name, fbl::String driver_path, fbl::String args,
                           uint32_t protocol_id, fbl::Array<zx_device_prop_t> props,
                           zx::channel coordinator_rpc, zx::channel device_controller_rpc,
                           bool wait_make_visible, bool want_init_task, zx::channel client_remote,
                           fbl::RefPtr<Device>* device) {
  fbl::RefPtr<Device> real_parent;
  // If our parent is a proxy, for the purpose of devfs, we need to work with
  // *its* parent which is the device that it is proxying.
  if (parent->flags & DEV_CTX_PROXY) {
    real_parent = parent->parent();
  } else {
    real_parent = parent;
  }

  auto dev = fbl::MakeRefCounted<Device>(coordinator, std::move(name), std::move(driver_path),
                                         std::move(args), real_parent, protocol_id,
                                         std::move(client_remote), wait_make_visible);
  if (!dev) {
    return ZX_ERR_NO_MEMORY;
  }
  zx_status_t status = dev->SetProps(std::move(props));
  if (status != ZX_OK) {
    return status;
  }

  dev->device_controller_.Bind(std::move(device_controller_rpc), coordinator->dispatcher());
  dev->set_channel(std::move(coordinator_rpc));

  // If we have bus device args we are, by definition, a bus device.
  if (dev->args_.size() > 0) {
    dev->flags |= DEV_CTX_MUST_ISOLATE;
  }

  // We exist within our parent's device host
  dev->set_host(parent->host());

  // We must mark the device as invisible before publishing so
  // that we don't send "device added" notifications.
  // The init task must complete before marking the device visible.
  // If |wait_make_visible| is true, we also wait for a device_make_visible call.
  // TODO(fxb/43370): this check should be removed once init tasks apply to all devices.
  if (wait_make_visible || want_init_task) {
    dev->flags |= DEV_CTX_INVISIBLE;
  }

  if ((status = devfs_publish(real_parent, dev)) < 0) {
    return status;
  }

  if ((status = Device::BeginWait(dev, coordinator->dispatcher())) != ZX_OK) {
    return status;
  }

  real_parent->children_.push_back(dev.get());
  VLOGF(1, "Created device %p '%s' (child)", real_parent.get(), real_parent->name().data());

  if (want_init_task) {
    dev->CreateInitTask();
  }

  dev->InitializeInspectValues();

  *device = std::move(dev);
  return ZX_OK;
}

zx_status_t Device::CreateComposite(Coordinator* coordinator, fbl::RefPtr<DriverHost> driver_host,
                                    const CompositeDevice& composite, zx::channel coordinator_rpc,
                                    zx::channel device_controller_rpc,
                                    fbl::RefPtr<Device>* device) {
  const auto& composite_props = composite.properties();
  fbl::Array<zx_device_prop_t> props(new zx_device_prop_t[composite_props.size()],
                                     composite_props.size());
  memcpy(props.data(), composite_props.data(), props.size() * sizeof(props[0]));

  auto dev =
      fbl::MakeRefCounted<Device>(coordinator, composite.name(), fbl::String(), fbl::String(),
                                  nullptr, ZX_PROTOCOL_COMPOSITE, zx::channel());
  if (!dev) {
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t status = dev->SetProps(std::move(props));
  if (status != ZX_OK) {
    return status;
  }

  dev->device_controller_.Bind(std::move(device_controller_rpc), coordinator->dispatcher());
  dev->set_channel(std::move(coordinator_rpc));
  // We exist within our parent's device host
  dev->set_host(std::move(driver_host));

  // TODO: Record composite membership

  // TODO(teisenbe): Figure out how to manifest in devfs?  For now just hang it off of
  // the root device?
  if ((status = devfs_publish(coordinator->root_device(), dev)) < 0) {
    return status;
  }

  if ((status = Device::BeginWait(dev, coordinator->dispatcher())) != ZX_OK) {
    return status;
  }

  VLOGF(1, "Created composite device %p '%s'", dev.get(), dev->name().data());

  dev->InitializeInspectValues();
  *device = std::move(dev);
  return ZX_OK;
}

zx_status_t Device::CreateProxy() {
  ZX_ASSERT(proxy_ == nullptr);

  fbl::String driver_path = libname_;
  // non-immortal devices, use foo.proxy.so for
  // their proxy devices instead of foo.so
  if (!(this->flags & DEV_CTX_IMMORTAL)) {
    const char* begin = driver_path.data();
    const char* end = strstr(begin, ".so");
    fbl::StringPiece prefix(begin, end == nullptr ? driver_path.size() : end - begin);
    fbl::AllocChecker ac;
    driver_path = fbl::String::Concat({prefix, ".proxy.so"}, &ac);
    if (!ac.check()) {
      return ZX_ERR_NO_MEMORY;
    }
  }

  auto dev =
      fbl::MakeRefCounted<Device>(this->coordinator, name_, std::move(driver_path), fbl::String(),
                                  fbl::RefPtr(this), protocol_id_, zx::channel());
  if (dev == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }

  dev->flags = DEV_CTX_PROXY;

  dev->InitializeInspectValues();

  proxy_ = std::move(dev);
  VLOGF(1, "Created proxy device %p '%s'", this, name_.data());
  return ZX_OK;
}

void Device::InitializeInspectValues() {
  inspect().set_driver(libname().c_str());
  inspect().set_protocol_id(protocol_id_);
  inspect().set_flags(flags);
  inspect().set_properties(props());

  char path[fuchsia_device_manager_DEVICE_PATH_MAX] = {};
  coordinator->GetTopologicalPath(fbl::RefPtr(this), path, fuchsia_device_manager_DEVICE_PATH_MAX);
  inspect().set_topological_path(path);

  std::string type("Device");
  if (flags & DEV_CTX_PROXY) {
    type = std::string("Proxy device");
  } else if (protocol_id_ == ZX_PROTOCOL_COMPOSITE) {
    type = std::string("Composite device");
  }
  inspect().set_type(type);
}

void Device::DetachFromParent() {
  if (this->flags & DEV_CTX_PROXY) {
    parent_->proxy_ = nullptr;
  } else {
    parent_->children_.erase(*this);
  }
  parent_ = nullptr;
}

zx_status_t Device::SignalReadyForBind(zx::duration delay) {
  return publish_task_.PostDelayed(this->coordinator->dispatcher(), delay);
}

void Device::CreateInitTask() {
  // We only ever create an init task when a device is initially added.
  ZX_ASSERT(!active_init_);
  set_state(Device::State::kInitializing);
  active_init_ = InitTask::Create(fbl::RefPtr(this));
}

fbl::RefPtr<SuspendTask> Device::RequestSuspendTask(uint32_t suspend_flags) {
  if (active_suspend_) {
    // We don't support different types of suspends concurrently, and
    // shouldn't be able to reach this state.
    ZX_ASSERT(suspend_flags == active_suspend_->suspend_flags());
  } else {
    active_suspend_ = SuspendTask::Create(fbl::RefPtr(this), suspend_flags);
  }
  return active_suspend_;
}

zx_status_t Device::SendInit(InitCompletion completion) {
  ZX_ASSERT(!init_completion_);

  VLOGF(1, "Initializing device %p '%s'", this, name_.data());
  zx_status_t status = dh_send_init(this);
  if (status != ZX_OK) {
    return status;
  }
  init_completion_ = std::move(completion);
  return ZX_OK;
}

zx_status_t Device::CompleteInit(zx_status_t status) {
  if (!init_completion_ && status == ZX_OK) {
    LOGF(ERROR, "Unexpected reply when initializing device %p '%s'", this, name_.data());
    return ZX_ERR_IO;
  }
  if (init_completion_) {
    init_completion_(status);
  }
  active_init_ = nullptr;
  return ZX_OK;
}

fbl::RefPtr<ResumeTask> Device::RequestResumeTask(uint32_t target_system_state) {
  if (active_resume_) {
    // We don't support different types of resumes concurrently, and
    // shouldn't be able to reach this state.
    ZX_ASSERT(target_system_state == active_resume_->target_system_state());
  } else {
    active_resume_ = ResumeTask::Create(fbl::RefPtr(this), target_system_state);
  }
  return active_resume_;
}

zx_status_t Device::SendSuspend(uint32_t flags, SuspendCompletion completion) {
  if (suspend_completion_) {
    // We already have a pending suspend
    return ZX_ERR_UNAVAILABLE;
  }
  VLOGF(1, "Suspending device %p '%s'", this, name_.data());
  zx_status_t status = dh_send_suspend(this, flags);
  if (status != ZX_OK) {
    return status;
  }
  set_state(Device::State::kSuspending);
  suspend_completion_ = std::move(completion);
  return ZX_OK;
}

zx_status_t Device::SendResume(uint32_t target_system_state, ResumeCompletion completion) {
  if (resume_completion_) {
    // We already have a pending resume
    return ZX_ERR_UNAVAILABLE;
  }
  VLOGF(1, "Resuming device %p '%s'", this, name_.data());
  zx_status_t status = dh_send_resume(this, target_system_state);
  if (status != ZX_OK) {
    return status;
  }
  set_state(Device::State::kResuming);
  resume_completion_ = std::move(completion);
  return ZX_OK;
}

void Device::CompleteSuspend(zx_status_t status) {
  if (status == ZX_OK) {
    // If a device is being removed, any existing suspend task will be forcibly completed,
    // in which case we should not update the state.
    if (state_ != Device::State::kDead) {
      set_state(Device::State::kSuspended);
    }
  } else {
    set_state(Device::State::kActive);
  }

  active_suspend_ = nullptr;
  if (suspend_completion_) {
    suspend_completion_(status);
  }
}

void Device::CompleteResume(zx_status_t status) {
  if (status != ZX_OK) {
    set_state(Device::State::kSuspended);
  } else {
    set_state(Device::State::kResumed);
  }
  if (resume_completion_) {
    resume_completion_(status);
  }
}

void Device::CreateUnbindRemoveTasks(UnbindTaskOpts opts) {
  if (state_ == Device::State::kDead) {
    return;
  }
  // Create the tasks if they do not exist yet. We always create both.
  if (active_unbind_ == nullptr && active_remove_ == nullptr) {
    // Make sure the remove task exists before the unbind task,
    // as the unbind task adds the remove task as a dependent.
    active_remove_ = RemoveTask::Create(fbl::RefPtr(this));
    active_unbind_ = UnbindTask::Create(fbl::RefPtr(this), opts);
    return;
  }
  if (!active_unbind_) {
    // The unbind task has already completed and the device is now being removed.
    return;
  }
  // User requested removals take priority over coordinator generated unbind tasks.
  bool override_existing = opts.driver_host_requested && !active_unbind_->driver_host_requested();
  if (!override_existing) {
    return;
  }
  // There is a potential race condition where a driver calls device_remove() on themselves
  // but the device's unbind hook is about to be called due to a parent being removed.
  // Since it is illegal to call device_remove() twice under the old API,
  // drivers handle this by checking whether their device has already been removed in
  // their unbind hook and hence will never reply to their unbind hook.
  if (state_ == Device::State::kUnbinding) {
    if (unbind_completion_) {
      zx_status_t status = CompleteUnbind(ZX_OK);
      if (status != ZX_OK) {
        LOGF(ERROR, "Cannot complete unbind: %s", zx_status_get_string(status));
      }
    }
  } else {
    // |do_unbind| may not match the stored field in the existing unbind task due to
    // the current device_remove / unbind model.
    // For closest compatibility with the current model, we should prioritize
    // driver_host calls to |ScheduleRemove| over our own scheduled unbind tasks for the children.
    active_unbind_->set_do_unbind(opts.do_unbind);
  }
}

zx_status_t Device::SendUnbind(UnbindCompletion completion) {
  if (unbind_completion_) {
    // We already have a pending unbind
    return ZX_ERR_UNAVAILABLE;
  }
  VLOGF(1, "Unbinding device %p '%s'", this, name_.data());
  set_state(Device::State::kUnbinding);
  unbind_completion_ = std::move(completion);
  zx_status_t status = dh_send_unbind(this);
  if (status != ZX_OK) {
    return status;
  }
  return ZX_OK;
}

zx_status_t Device::SendCompleteRemoval(UnbindCompletion completion) {
  if (remove_completion_) {
    // We already have a pending remove.
    return ZX_ERR_UNAVAILABLE;
  }
  VLOGF(1, "Completing removal of device %p '%s'", this, name_.data());
  set_state(Device::State::kUnbinding);
  remove_completion_ = std::move(completion);
  zx_status_t status = dh_send_complete_removal(
      this, [dev = fbl::RefPtr(this)]() mutable { dev->CompleteRemove(); });
  if (status != ZX_OK) {
    return status;
  }
  return ZX_OK;
}

zx_status_t Device::CompleteUnbind(zx_status_t status) {
  if (!unbind_completion_ && status == ZX_OK) {
    LOGF(ERROR, "Unexpected reply when unbinding device %p '%s'", this, name_.data());
    return ZX_ERR_IO;
  }
  if (unbind_completion_) {
    unbind_completion_(status);
  }
  active_unbind_ = nullptr;
  return ZX_OK;
}

zx_status_t Device::CompleteRemove(zx_status_t status) {
  if (!remove_completion_ && status == ZX_OK) {
    LOGF(ERROR, "Unexpected reply when removing device %p '%s'", this, name_.data());
    return ZX_ERR_IO;
  }
  // If we received an error, it is because we are currently force removing the device.
  if (status == ZX_OK) {
    coordinator->RemoveDevice(fbl::RefPtr(this), false);
  }
  if (remove_completion_) {
    // If we received an error, it is because we are currently force removing the device.
    // In that case, all other devices in the driver_host will be force removed too,
    // and they will call CompleteRemove() before the remove task is scheduled to run.
    // For ancestor dependents in other driver_hosts, we want them to proceed removal as usual.
    remove_completion_(ZX_OK);
  }
  active_remove_ = nullptr;
  return ZX_OK;
}

// Handle inbound messages from driver_host to devices
void Device::HandleRpc(fbl::RefPtr<Device>&& dev, async_dispatcher_t* dispatcher,
                       async::WaitBase* wait, zx_status_t status,
                       const zx_packet_signal_t* signal) {
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to wait on RPC for device %p '%s': %s", dev.get(), dev->name().data(),
         zx_status_get_string(status));
    return;
  }

  if (signal->observed & ZX_CHANNEL_READABLE) {
    zx_status_t r = dev->HandleRead();
    if (r != ZX_OK) {
      if (r != ZX_ERR_STOP) {
        LOGF(ERROR, "Failed to handle RPC for device %p '%s': %s", dev.get(), dev->name().data(),
             zx_status_get_string(r));
      }
      // If this device isn't already dead (removed), remove it. RemoveDevice() may
      // have been called by the RPC handler, in particular for the RemoveDevice RPC.
      if (dev->state() != Device::State::kDead) {
        dev->coordinator->RemoveDevice(dev, true);
      }
      // Do not start waiting again on this device's channel again
      return;
    }
    Device::BeginWait(std::move(dev), dispatcher);
    return;
  }
  if (signal->observed & ZX_CHANNEL_PEER_CLOSED) {
    // If the device is already dead, we are detecting an expected disconnect from the driver_host.
    if (dev->state() != Device::State::kDead) {
      LOGF(ERROR, "Disconnected device %p '%s'", dev.get(), dev->name().data());
      dev->coordinator->RemoveDevice(dev, true);
    }
    // Do not start waiting again on this device's channel again
    return;
  }
  LOGF(WARNING, "Unexpected signal state %#08x for device %p '%s'", signal->observed, dev.get(),
       dev->name().data());
  Device::BeginWait(std::move(dev), dispatcher);
}

static zx_status_t fidl_LogMessage(void* ctx, const char* msg, size_t size) {
  auto dev = static_cast<Device*>(ctx);
  dev->test_reporter->LogMessage(msg, size);
  return ZX_OK;
}

static zx_status_t fidl_LogTestCase(void* ctx, const char* name, size_t name_size,
                                    const fuchsia_driver_test_TestCaseResult* result) {
  auto dev = static_cast<Device*>(ctx);
  dev->test_reporter->LogTestCase(name, name_size, result);
  return ZX_OK;
}

static const fuchsia_driver_test_Logger_ops_t kTestOps = {
    .LogMessage = fidl_LogMessage,
    .LogTestCase = fidl_LogTestCase,
};

void Device::HandleTestOutput(async_dispatcher_t* dispatcher, async::WaitBase* wait,
                              zx_status_t status, const zx_packet_signal_t* signal) {
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to wait on test output for device %p '%s': %s", this, name_.data(),
         zx_status_get_string(status));
    return;
  }
  if (!(signal->observed & ZX_CHANNEL_PEER_CLOSED)) {
    LOGF(WARNING, "Unexpected signal state %#08x for device %p '%s'", signal->observed, this,
         name_.data());
    return;
  }

  test_reporter->TestStart();

  // Now that the driver has closed the channel, read all of the messages.
  // TODO(ZX-4374): Handle the case where the channel fills up before we begin reading.
  while (true) {
    uint8_t msg_bytes[ZX_CHANNEL_MAX_MSG_BYTES];
    zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
    uint32_t msize = sizeof(msg_bytes);
    uint32_t hcount = std::size(handles);

    zx_status_t r = test_output_.read(0, &msg_bytes, handles, msize, hcount, &msize, &hcount);
    if (r == ZX_ERR_PEER_CLOSED) {
      test_reporter->TestFinished();
      break;
    } else if (r != ZX_OK) {
      LOGF(ERROR, "Failed to read test output for device %p '%s': %s", this, name_.data(),
           zx_status_get_string(r));
      break;
    }

    fidl_msg_t fidl_msg = {
        .bytes = msg_bytes,
        .handles = handles,
        .num_bytes = msize,
        .num_handles = hcount,
    };

    if (fidl_msg.num_bytes < sizeof(fidl_message_header_t)) {
      zx_handle_close_many(fidl_msg.handles, fidl_msg.num_handles);
      LOGF(ERROR, "Invalid FIDL message header for device %p '%s'", this, name_.data());
      break;
    }

    auto header = static_cast<fidl_message_header_t*>(fidl_msg.bytes);
    FidlTxn txn(test_output_, header->txid);
    r = fuchsia_driver_test_Logger_dispatch(this, txn.fidl_txn(), &fidl_msg, &kTestOps);
    if (r != ZX_OK) {
      LOGF(ERROR, "Failed to handle RPC for device %p '%s': %s", this, name_.data(),
           zx_status_get_string(r));
      break;
    }
  }
}

zx_status_t Device::HandleRead() {
  uint8_t msg[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t hin[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t msize = sizeof(msg);
  uint32_t hcount = std::size(hin);

  if (state_ == Device::State::kDead) {
    LOGF(ERROR, "Attempted to RPC dead device %p '%s'", this, name_.data());
    return ZX_ERR_INTERNAL;
  }

  zx_status_t r;
  if ((r = channel()->read(0, &msg, hin, msize, hcount, &msize, &hcount)) != ZX_OK) {
    return r;
  }

  fidl_msg_t fidl_msg = {
      .bytes = msg,
      .handles = hin,
      .num_bytes = msize,
      .num_handles = hcount,
  };

  if (fidl_msg.num_bytes < sizeof(fidl_message_header_t)) {
    zx_handle_close_many(fidl_msg.handles, fidl_msg.num_handles);
    return ZX_ERR_IO;
  }

  auto hdr = static_cast<fidl_message_header_t*>(fidl_msg.bytes);
  // Check if we're receiving a Coordinator request
  {
    zx::unowned_channel conn = channel();
    DevmgrFidlTxn txn(std::move(conn), hdr->txid);
    bool dispatched =
        llcpp::fuchsia::device::manager::Coordinator::TryDispatch(this, &fidl_msg, &txn);
    auto status = txn.Status();
    if (dispatched) {
      if (status == ZX_OK && state_ == Device::State::kDead) {
        // We have removed the device. Signal that we are done with this channel.
        return ZX_ERR_STOP;
      }
      return status;
    }
  }

  LOGF(ERROR, "Unsupported FIDL protocol (ordinal %#16lx) for device %p '%s'", hdr->ordinal, this,
       name_.data());
  zx_handle_close_many(fidl_msg.handles, fidl_msg.num_handles);
  return ZX_ERR_IO;
}

zx_status_t Device::SetProps(fbl::Array<const zx_device_prop_t> props) {
  // This function should only be called once
  ZX_DEBUG_ASSERT(props_.data() == nullptr);

  props_ = std::move(props);
  topo_prop_ = nullptr;

  for (const auto prop : props_) {
    if (prop.id >= BIND_TOPO_START && prop.id <= BIND_TOPO_END) {
      if (topo_prop_ != nullptr) {
        return ZX_ERR_INVALID_ARGS;
      }
      topo_prop_ = &prop;
    }
  }
  return ZX_OK;
}

void Device::set_host(fbl::RefPtr<DriverHost> host) {
  if (host_) {
    host_->devices().erase(*this);
  }
  host_ = std::move(host);
  set_local_id(0);
  if (host_) {
    host_->devices().push_back(this);
    set_local_id(host_->new_device_id());
  }
}

const char* Device::GetTestDriverName() {
  for (auto& child : children()) {
    return this->coordinator->LibnameToDriver(child.libname().data())->name.data();
  }
  return nullptr;
}

zx_status_t Device::DriverCompatibiltyTest() {
  thrd_t t;
  if (test_state() != TestStateMachine::kTestNotStarted) {
    return ZX_ERR_ALREADY_EXISTS;
  }
  auto cb = [](void* arg) -> int {
    auto dev = fbl::RefPtr(reinterpret_cast<Device*>(arg));
    return dev->RunCompatibilityTests();
  };
  int ret = thrd_create_with_name(&t, cb, this, "compatibility-tests-thread");
  if (ret != thrd_success) {
    LOGF(ERROR, "Failed to create thread for driver compatibility test '%s': %d",
         GetTestDriverName(), ret);
    if (test_reply_required_) {
      dh_send_complete_compatibility_tests(
          this, fuchsia_device_manager_CompatibilityTestStatus_ERR_INTERNAL);
    }
    return ZX_ERR_NO_RESOURCES;
  }
  thrd_detach(t);
  return ZX_OK;
}

#define TEST_LOGF(severity, message...) FX_LOGF(severity, "compatibility", message)

int Device::RunCompatibilityTests() {
  const char* test_driver_name = GetTestDriverName();
  TEST_LOGF(INFO, "Running test '%s'", test_driver_name);
  auto cleanup = fbl::MakeAutoCall([this]() {
    if (test_reply_required_) {
      dh_send_complete_compatibility_tests(this, test_status_);
    }
    test_event().reset();
    set_test_state(Device::TestStateMachine::kTestDone);
    set_test_reply_required(false);
  });
  // Device should be bound for test to work
  if (!(flags & DEV_CTX_BOUND) || children().is_empty()) {
    TEST_LOGF(ERROR, "[  FAILED  ] %s: Parent device not bound", test_driver_name);
    test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_BIND_NO_DDKADD;
    return ZX_ERR_BAD_STATE;
  }
  zx_status_t status = zx::event::create(0, &test_event());
  if (status != ZX_OK) {
    TEST_LOGF(ERROR, "[  FAILED  ] %s: Event creation failed, %s", test_driver_name,
              zx_status_get_string(status));
    test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_INTERNAL;
    return ZX_ERR_INTERNAL;
  }

  // Issue unbind on all its children.
  for (auto itr = children().begin(); itr != children().end();) {
    auto& child = *itr;
    itr++;
    this->set_test_state(Device::TestStateMachine::kTestUnbindSent);
    coordinator->ScheduleDriverHostRequestedRemove(fbl::RefPtr<Device>(&child),
                                                   true /* unbind_self */);
  }

  zx_signals_t observed = 0;
  // Now wait for the device to be removed.
  status =
      test_event().wait_one(TEST_REMOVE_DONE_SIGNAL, zx::deadline_after(test_time()), &observed);
  if (status != ZX_OK) {
    if (status == ZX_ERR_TIMED_OUT) {
      // The Remove did not complete.
      TEST_LOGF(ERROR,
                "[  FAILED  ] %s: Timed out waiting for device to be removed, check if"
                " device_remove() was called in the unbind routine of the driver",
                test_driver_name, status);
      test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_UNBIND_TIMEOUT;
    } else {
      TEST_LOGF(ERROR, "[  FAILED  ] %s: Error waiting for device to be removed: %s",
                test_driver_name, zx_status_get_string(status));
      test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_INTERNAL;
    }
    return ZX_ERR_INTERNAL;
  }
  this->set_test_state(Device::TestStateMachine::kTestBindSent);
  this->coordinator->HandleNewDevice(fbl::RefPtr(this));
  observed = 0;
  status = test_event().wait_one(TEST_BIND_DONE_SIGNAL, zx::deadline_after(test_time()), &observed);
  if (status != ZX_OK) {
    if (status == ZX_ERR_TIMED_OUT) {
      // The Bind did not complete.
      TEST_LOGF(ERROR,
                "[  FAILED  ] %s: Timed out waiting for driver to be bound, check if there"
                " is blocking IO in the driver's bind()",
                test_driver_name, status);
      test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_BIND_TIMEOUT;
    } else {
      TEST_LOGF(ERROR, "[  FAILED  ] %s: Error waiting for driver to be bound: %s",
                test_driver_name, zx_status_get_string(status));
      test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_INTERNAL;
    }
    return ZX_ERR_INTERNAL;
  }
  this->set_test_state(Device::TestStateMachine::kTestBindDone);
  if (this->children().is_empty()) {
    TEST_LOGF(
        ERROR,
        "[  FAILED  ] %s: Driver did not add a child device in bind(), check if it called DdkAdd()",
        test_driver_name);
    test_status_ = fuchsia_device_manager_CompatibilityTestStatus_ERR_BIND_NO_DDKADD;
    return -1;
  }
  TEST_LOGF(INFO, "[  PASSED  ] %s", test_driver_name);
  // TODO(ravoorir): Test Suspend and Resume hooks
  test_status_ = fuchsia_device_manager_CompatibilityTestStatus_OK;
  return ZX_OK;
}

#undef TEST_LOGF

void Device::AddDevice(::zx::channel coordinator, ::zx::channel device_controller_client,
                       ::fidl::VectorView<llcpp::fuchsia::device::manager::DeviceProperty> props,
                       ::fidl::StringView name_view, uint32_t protocol_id,
                       ::fidl::StringView driver_path_view, ::fidl::StringView args_view,
                       llcpp::fuchsia::device::manager::AddDeviceConfig device_add_config,
                       bool has_init, ::zx::channel client_remote,
                       AddDeviceCompleter::Sync completer) {
  auto parent = fbl::RefPtr(this);
  fbl::StringPiece name(name_view.data(), name_view.size());
  fbl::StringPiece driver_path(driver_path_view.data(), driver_path_view.size());
  fbl::StringPiece args(args_view.data(), args_view.size());

  fbl::RefPtr<Device> device;
  zx_status_t status = parent->coordinator->AddDevice(
      parent, std::move(device_controller_client), std::move(coordinator), props.data(),
      props.count(), name, protocol_id, driver_path, args, false /* invisible */, has_init,
      kEnableAlwaysInit, std::move(client_remote), &device);
  if (device != nullptr &&
      (device_add_config &
       llcpp::fuchsia::device::manager::AddDeviceConfig::ALLOW_MULTI_COMPOSITE)) {
    device->flags |= DEV_CTX_ALLOW_MULTI_COMPOSITE;
  }
  uint64_t local_id = device != nullptr ? device->local_id() : 0;
  llcpp::fuchsia::device::manager::Coordinator_AddDevice_Result response;
  if (status != ZX_OK) {
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
  } else {
    llcpp::fuchsia::device::manager::Coordinator_AddDevice_Response resp{.local_device_id =
                                                                             local_id};
    response.set_response(fidl::unowned_ptr(&resp));
    completer.Reply(std::move(response));
  }
}

void Device::PublishMetadata(::fidl::StringView device_path, uint32_t key,
                             ::fidl::VectorView<uint8_t> data,
                             PublishMetadataCompleter::Sync completer) {
  auto dev = fbl::RefPtr(this);
  char path[fuchsia_device_manager_DEVICE_PATH_MAX + 1];
  memcpy(path, device_path.data(), device_path.size());
  path[device_path.size()] = 0;
  zx_status_t status = dev->coordinator->PublishMetadata(dev, path, key, data.data(),
                                                         static_cast<uint32_t>(data.count()));
  llcpp::fuchsia::device::manager::Coordinator_PublishMetadata_Result response;
  if (status != ZX_OK) {
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
  } else {
    fidl::aligned<llcpp::fuchsia::device::manager::Coordinator_PublishMetadata_Response> resp;
    response.set_response(fidl::unowned_ptr(&resp));
    completer.Reply(std::move(response));
  }
}

void Device::AddDeviceInvisible(
    ::zx::channel coordinator, ::zx::channel device_controller_client,
    ::fidl::VectorView<llcpp::fuchsia::device::manager::DeviceProperty> props,
    ::fidl::StringView name_view, uint32_t protocol_id, ::fidl::StringView driver_path_view,
    ::fidl::StringView args_view, bool has_init, ::zx::channel client_remote,
    AddDeviceInvisibleCompleter::Sync completer) {
  auto parent = fbl::RefPtr(this);
  fbl::StringPiece name(name_view.data(), name_view.size());
  fbl::StringPiece driver_path(driver_path_view.data(), driver_path_view.size());
  fbl::StringPiece args(args_view.data(), args_view.size());

  fbl::RefPtr<Device> device;
  zx_status_t status = parent->coordinator->AddDevice(
      parent, std::move(device_controller_client), std::move(coordinator), props.data(),
      props.count(), name, protocol_id, driver_path, args, true /* invisible */, has_init,
      kEnableAlwaysInit, std::move(client_remote), &device);
  uint64_t local_id = device != nullptr ? device->local_id() : 0;
  llcpp::fuchsia::device::manager::Coordinator_AddDeviceInvisible_Result response;
  if (status != ZX_OK) {
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
  } else {
    llcpp::fuchsia::device::manager::Coordinator_AddDeviceInvisible_Response resp{.local_device_id =
                                                                                      local_id};
    response.set_response(fidl::unowned_ptr(&resp));
    completer.Reply(std::move(response));
  }
}

void Device::ScheduleRemove(bool unbind_self, ScheduleRemoveCompleter::Sync completer) {
  auto dev = fbl::RefPtr(this);

  VLOGF(1, "Scheduling remove of device %p '%s'", dev.get(), dev->name().data());

  dev->coordinator->ScheduleDriverHostRequestedRemove(dev, unbind_self);
}

void Device::ScheduleUnbindChildren(ScheduleUnbindChildrenCompleter::Sync completer) {
  auto dev = fbl::RefPtr(this);

  VLOGF(1, "Scheduling unbind of children for device %p '%s'", dev.get(), dev->name().data());

  dev->coordinator->ScheduleDriverHostRequestedUnbindChildren(dev);
}

void Device::MakeVisible(MakeVisibleCompleter::Sync completer) {
  auto dev = fbl::RefPtr(this);
  llcpp::fuchsia::device::manager::Coordinator_MakeVisible_Result response;
  if (dev->coordinator->InSuspend()) {
    LOGF(ERROR, "'make-visible' is forbidden in suspend");
    zx_status_t status = ZX_ERR_BAD_STATE;
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
    return;
  }
  VLOGF(1, "'make-visible' device %p '%s'", dev.get(), dev->name().data());
  // TODO(teisenbe): MakeVisibile can return errors.  We should probably
  // act on it, but the existing code being migrated does not.
  dev->coordinator->MakeVisible(dev);
  fidl::aligned<llcpp::fuchsia::device::manager::Coordinator_MakeVisible_Response> resp;
  response.set_response(fidl::unowned_ptr(&resp));
  completer.Reply(std::move(response));
}

void Device::BindDevice(::fidl::StringView driver_path_view, BindDeviceCompleter::Sync completer) {
  auto dev = fbl::RefPtr(this);
  fbl::StringPiece driver_path(driver_path_view.data(), driver_path_view.size());

  llcpp::fuchsia::device::manager::Coordinator_BindDevice_Result response;
  if (dev->coordinator->InSuspend()) {
    LOGF(ERROR, "'bind-device' is forbidden in suspend");
    zx_status_t status = ZX_ERR_BAD_STATE;
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
    return;
  }

  VLOGF(1, "'bind-device' device %p '%s'", dev.get(), dev->name().data());
  zx_status_t status = dev->coordinator->BindDevice(dev, driver_path, false /* new device */);
  if (status != ZX_OK) {
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
  } else {
    fidl::aligned<llcpp::fuchsia::device::manager::Coordinator_BindDevice_Response> resp;
    response.set_response(fidl::unowned_ptr(&resp));
    completer.Reply(std::move(response));
  }
}

void Device::GetTopologicalPath(GetTopologicalPathCompleter::Sync completer) {
  auto dev = fbl::RefPtr(this);
  char path[fuchsia_device_manager_DEVICE_PATH_MAX + 1];
  zx_status_t status;
  llcpp::fuchsia::device::manager::Coordinator_GetTopologicalPath_Result response;
  if ((status = dev->coordinator->GetTopologicalPath(dev, path, sizeof(path))) != ZX_OK) {
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
    return;
  }
  auto path_view = ::fidl::unowned_str(path, strlen(path));
  llcpp::fuchsia::device::manager::Coordinator_GetTopologicalPath_Response resp{
      .path = std::move(path_view)};
  response.set_response(fidl::unowned_ptr(&resp));
  completer.Reply(std::move(response));
}

void Device::LoadFirmware(::fidl::StringView fw_path_view, LoadFirmwareCompleter::Sync completer) {
  auto dev = fbl::RefPtr(this);

  char fw_path[fuchsia_device_manager_DEVICE_PATH_MAX + 1];
  memcpy(fw_path, fw_path_view.data(), fw_path_view.size());
  fw_path[fw_path_view.size()] = 0;
  llcpp::fuchsia::device::manager::Coordinator_LoadFirmware_Result response;

  zx::vmo vmo;
  uint64_t size = 0;
  zx_status_t status;
  if ((status = dev->coordinator->LoadFirmware(dev, fw_path, &vmo, &size)) != ZX_OK) {
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
    return;
  }

  llcpp::fuchsia::device::manager::Coordinator_LoadFirmware_Response resp{
      .vmo = std::move(vmo),
      .size = size,
  };
  response.set_response(fidl::unowned_ptr(&resp));
  completer.Reply(std::move(response));
}

void Device::GetMetadata(uint32_t key, GetMetadataCompleter::Sync completer) {
  auto dev = fbl::RefPtr(this);
  uint8_t data[fuchsia_device_manager_METADATA_BYTES_MAX];
  size_t actual = 0;
  llcpp::fuchsia::device::manager::Coordinator_GetMetadata_Result response;
  zx_status_t status = dev->coordinator->GetMetadata(dev, key, data, sizeof(data), &actual);
  if (status != ZX_OK) {
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
    return;
  }
  auto data_view = ::fidl::VectorView<uint8_t>(fidl::unowned_ptr(data), actual);
  llcpp::fuchsia::device::manager::Coordinator_GetMetadata_Response resp{.data =
                                                                             std::move(data_view)};
  response.set_response(fidl::unowned_ptr(&resp));
  completer.Reply(std::move(response));
}

void Device::GetMetadataSize(uint32_t key, GetMetadataSizeCompleter::Sync completer) {
  auto dev = fbl::RefPtr(this);
  size_t size;
  llcpp::fuchsia::device::manager::Coordinator_GetMetadataSize_Result response;
  zx_status_t status = dev->coordinator->GetMetadataSize(dev, key, &size);
  if (status != ZX_OK) {
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
    return;
  }
  llcpp::fuchsia::device::manager::Coordinator_GetMetadataSize_Response resp{.size = size};
  response.set_response(fidl::unowned_ptr(&resp));
  completer.Reply(std::move(response));
}

void Device::AddMetadata(uint32_t key, ::fidl::VectorView<uint8_t> data,
                         AddMetadataCompleter::Sync completer) {
  auto dev = fbl::RefPtr(this);
  zx_status_t status =
      dev->coordinator->AddMetadata(dev, key, data.data(), static_cast<uint32_t>(data.count()));
  llcpp::fuchsia::device::manager::Coordinator_AddMetadata_Result response;
  if (status != ZX_OK) {
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
  } else {
    fidl::aligned<llcpp::fuchsia::device::manager::Coordinator_AddMetadata_Response> resp;
    response.set_response(fidl::unowned_ptr(&resp));
    completer.Reply(std::move(response));
  }
}
void Device::RunCompatibilityTests(int64_t hook_wait_time,
                                   RunCompatibilityTestsCompleter::Sync completer) {
  auto dev = fbl::RefPtr(this);
  fbl::RefPtr<Device>& real_parent = dev;
  zx_status_t status = ZX_OK;
  if (dev->flags & DEV_CTX_PROXY) {
    real_parent = dev->parent();
  }
  zx::duration test_time = zx::nsec(hook_wait_time);
  real_parent->set_test_time(test_time);
  real_parent->set_test_reply_required(true);
  status = real_parent->DriverCompatibiltyTest();
  llcpp::fuchsia::device::manager::Coordinator_RunCompatibilityTests_Result response;
  if (status != ZX_OK) {
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
  } else {
    fidl::aligned<llcpp::fuchsia::device::manager::Coordinator_RunCompatibilityTests_Response> resp;
    response.set_response(fidl::unowned_ptr(&resp));
    completer.Reply(std::move(response));
  }
}

void Device::DirectoryWatch(uint32_t mask, uint32_t options, ::zx::channel watcher,
                            DirectoryWatchCompleter::Sync completer) {
  llcpp::fuchsia::device::manager::Coordinator_DirectoryWatch_Result response;
  if (mask & (~fuchsia_io_WATCH_MASK_ALL) || options != 0) {
    zx_status_t status = ZX_ERR_INVALID_ARGS;
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
    return;
  }

  zx_status_t status = devfs_watch(this->self, std::move(watcher), mask);
  if (status != ZX_OK) {
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
  } else {
    fidl::aligned<llcpp::fuchsia::device::manager::Coordinator_DirectoryWatch_Response> resp;
    response.set_response(fidl::unowned_ptr(&resp));
    completer.Reply(std::move(response));
  }
}

void Device::AddCompositeDevice(
    ::fidl::StringView name_view,
    llcpp::fuchsia::device::manager::CompositeDeviceDescriptor comp_desc,
    AddCompositeDeviceCompleter::Sync completer) {
  auto dev = fbl::RefPtr(this);
  fbl::StringPiece name(name_view.data(), name_view.size());
  zx_status_t status = this->coordinator->AddCompositeDevice(dev, name, std::move(comp_desc));
  llcpp::fuchsia::device::manager::Coordinator_AddCompositeDevice_Result response;
  if (status != ZX_OK) {
    response.set_err(fidl::unowned_ptr(&status));
    completer.Reply(std::move(response));
  } else {
    fidl::aligned<llcpp::fuchsia::device::manager::Coordinator_AddCompositeDevice_Response> resp;
    response.set_response(fidl::unowned_ptr(&resp));
    completer.Reply(std::move(response));
  }
}
