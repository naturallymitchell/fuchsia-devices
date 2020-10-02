// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "driver_host.h"

#include <dlfcn.h>
#include <fuchsia/device/manager/c/fidl.h>
#include <fuchsia/device/manager/llcpp/fidl.h>
#include <fuchsia/io/c/fidl.h>
#include <inttypes.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/receiver.h>
#include <lib/async/cpp/wait.h>
#include <lib/fdio/fdio.h>
#include <lib/fidl/coding.h>
#include <lib/zx/debuglog.h>
#include <lib/zx/process.h>
#include <lib/zx/resource.h>
#include <lib/zx/vmo.h>
#include <lib/zxio/inception.h>
#include <lib/zxio/null.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/dlfcn.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/log.h>

#include <memory>
#include <new>
#include <utility>
#include <vector>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <fbl/function.h>
#include <fbl/string_printf.h>

#include "async_loop_owned_rpc_handler.h"
#include "composite_device.h"
#include "connection_destroyer.h"
#include "device_controller_connection.h"
#include "env.h"
#include "fidl_txn.h"
#include "log.h"
#include "main.h"
#include "proxy_iostate.h"
#include "scheduler_profile.h"
#include "tracing.h"

namespace {

llcpp::fuchsia::device::manager::DeviceProperty convert_device_prop(const zx_device_prop_t& prop) {
  return llcpp::fuchsia::device::manager::DeviceProperty{
      .id = prop.id,
      .reserved = prop.reserved,
      .value = prop.value,
  };
}

static fx_log_severity_t log_min_severity(const char* name, const char* flag) {
  if (!strcasecmp(flag, "error")) {
    return FX_LOG_ERROR;
  }
  if (!strcasecmp(flag, "warning")) {
    return FX_LOG_WARNING;
  }
  if (!strcasecmp(flag, "info")) {
    return FX_LOG_INFO;
  }
  if (!strcasecmp(flag, "debug")) {
    return FX_LOG_DEBUG;
  }
  if (!strcasecmp(flag, "trace")) {
    return FX_LOG_TRACE;
  }
  if (!strcasecmp(flag, "serial")) {
    return DDK_LOG_SERIAL;
  }
  LOGF(WARNING, "Invalid minimum log severity '%s' for driver '%s', will log all", flag, name);
  return FX_LOG_ALL;
}

zx_status_t log_rpc_result(const fbl::RefPtr<zx_device_t>& dev, const char* opname,
                           zx_status_t status, zx_status_t call_status = ZX_OK) {
  if (status != ZX_OK) {
    LOGD(ERROR, *dev, "Failed %s RPC: %s", opname, zx_status_get_string(status));
    return status;
  }
  if (call_status != ZX_OK && call_status != ZX_ERR_NOT_FOUND) {
    LOGD(ERROR, *dev, "Failed %s: %s", opname, zx_status_get_string(call_status));
  }
  return call_status;
}

}  // namespace

const char* mkdevpath(const zx_device_t& dev, char* const path, size_t max) {
  if (max == 0) {
    return "";
  }
  char* end = path + max;
  char sep = 0;

  auto append_name = [&end, &path, &sep](const zx_device_t& dev) {
    *(--end) = sep;

    size_t len = strlen(dev.name());
    if (len > static_cast<size_t>(end - path)) {
      return;
    }
    end -= len;
    memcpy(end, dev.name(), len);
    sep = '/';
  };

  append_name(dev);

  fbl::RefPtr<zx_device> itr_dev = dev.parent();
  while (itr_dev && end > path) {
    append_name(*itr_dev);
    itr_dev = itr_dev->parent();
  }

  // If devpath is longer than |max|, add an ellipsis.
  constexpr char ellipsis[] = "...";
  constexpr size_t ellipsis_len = sizeof(ellipsis) - 1;
  if (*end == sep && max > ellipsis_len) {
    if (ellipsis_len > static_cast<size_t>(end - path)) {
      end = path;
    } else {
      end -= ellipsis_len;
    }
    memcpy(end, ellipsis, ellipsis_len);
  }

  return end;
}

zx_status_t zx_driver::Create(std::string_view libname, InspectNodeCollection& drivers,
                              fbl::RefPtr<zx_driver>* out_driver) {
  char process_name[ZX_MAX_NAME_LEN] = {};
  zx::process::self()->get_property(ZX_PROP_NAME, process_name, sizeof(process_name));
  const char* tags[] = {process_name, "driver"};
  fx_logger_config_t config{
      .min_severity = FX_LOG_SEVERITY_DEFAULT,
      .console_fd = getenv_bool("devmgr.log-to-debuglog", false) ? dup(STDOUT_FILENO) : -1,
      .log_service_channel = ZX_HANDLE_INVALID,
      .tags = tags,
      .num_tags = std::size(tags),
  };
  fx_logger_t* logger;
  zx_status_t status = fx_logger_create(&config, &logger);
  if (status != ZX_OK) {
    return status;
  }

  *out_driver = fbl::AdoptRef(new zx_driver(logger, libname, drivers));
  return ZX_OK;
}

zx_driver::zx_driver(fx_logger_t* logger, std::string_view libname, InspectNodeCollection& drivers)
    : logger_(logger), libname_(libname), inspect_(drivers, std::string(libname)) {}

zx_driver::~zx_driver() { fx_logger_destroy(logger_); }

zx_status_t DriverHostContext::SetupRootDevcoordinatorConnection(zx::channel ch) {
  auto conn = std::make_unique<internal::DevhostControllerConnection>(this);
  if (conn == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }

  conn->set_channel(std::move(ch));
  return internal::DevhostControllerConnection::BeginWait(std::move(conn), loop_.dispatcher());
}

zx_status_t DriverHostContext::ScheduleWork(const fbl::RefPtr<zx_device_t>& dev,
                                            void (*callback)(void*), void* cookie) {
  if (!callback) {
    return ZX_ERR_INVALID_ARGS;
  }
  PushWorkItem(dev, [callback, cookie]() { callback(cookie); });
  return ZX_OK;
}

// Send message to driver_manager asking to add child device to
// parent device.  Called under the api lock.
zx_status_t DriverHostContext::DriverManagerAdd(const fbl::RefPtr<zx_device_t>& parent,
                                                const fbl::RefPtr<zx_device_t>& child,
                                                const char* proxy_args,
                                                const zx_device_prop_t* props, uint32_t prop_count,
                                                zx::vmo inspect, zx::channel client_remote) {
  bool add_invisible = child->flags() & DEV_FLAG_INVISIBLE;
  fuchsia::device::manager::AddDeviceConfig add_device_config;

  if (child->flags() & DEV_FLAG_ALLOW_MULTI_COMPOSITE) {
    add_device_config |= fuchsia::device::manager::AddDeviceConfig::ALLOW_MULTI_COMPOSITE;
  }
  if (add_invisible) {
    add_device_config |= fuchsia::device::manager::AddDeviceConfig::INVISIBLE;
  }
  if (child->flags() & DEV_FLAG_UNBINDABLE) {
    add_device_config |= fuchsia::device::manager::AddDeviceConfig::SKIP_AUTOBIND;
  }

  zx_status_t status;
  zx::channel coordinator, coordinator_remote;
  if ((status = zx::channel::create(0, &coordinator, &coordinator_remote)) != ZX_OK) {
    return status;
  }

  zx::channel device_controller, device_controller_remote;
  if ((status = zx::channel::create(0, &device_controller, &device_controller_remote)) != ZX_OK) {
    return status;
  }

  std::unique_ptr<DeviceControllerConnection> conn;
  status = DeviceControllerConnection::Create(this, child, std::move(device_controller),
                                              std::move(coordinator), &conn);
  if (status != ZX_OK) {
    return status;
  }

  std::vector<llcpp::fuchsia::device::manager::DeviceProperty> props_list = {};
  for (size_t i = 0; i < prop_count; i++) {
    props_list.push_back(convert_device_prop(props[i]));
  }

  const zx::channel& rpc = *parent->coordinator_rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  size_t proxy_args_len = proxy_args ? strlen(proxy_args) : 0;
  zx_status_t call_status = ZX_OK;
  static_assert(sizeof(zx_device_prop_t) == sizeof(uint64_t));
  uint64_t device_id = 0;
  auto response = fuchsia::device::manager::Coordinator::Call::AddDevice(
      zx::unowned_channel(rpc.get()), std::move(coordinator_remote),
      std::move(device_controller_remote), ::fidl::unowned_vec(props_list),
      ::fidl::unowned_str(child->name(), strlen(child->name())), child->protocol_id(),
      ::fidl::unowned_str(child->driver->libname()),
      ::fidl::unowned_str(proxy_args, proxy_args_len), add_device_config,
      child->ops()->init /* has_init */, std::move(inspect), std::move(client_remote));
  status = response.status();
  if (status == ZX_OK) {
    if (response.Unwrap()->result.is_response()) {
      device_id = response.Unwrap()->result.response().local_device_id;
      if (add_invisible) {
        // Mark child as invisible until the init function is replied.
        child->set_flag(DEV_FLAG_INVISIBLE);
      }
    } else {
      call_status = response.Unwrap()->result.err();
    }
  }

  status = log_rpc_result(parent, "add-device", status, call_status);
  if (status != ZX_OK) {
    return status;
  }

  child->set_local_id(device_id);
  return DeviceControllerConnection::BeginWait(std::move(conn), loop_.dispatcher());
}

// Send message to driver_manager informing it that this device
// is being removed.  Called under the api lock.
zx_status_t DriverHostContext::DriverManagerRemove(fbl::RefPtr<zx_device_t> dev) {
  DeviceControllerConnection* conn = dev->conn.load();
  if (conn == nullptr) {
    LOGD(ERROR, *dev, "Invalid device controller connection");
    return ZX_ERR_INTERNAL;
  }
  VLOGD(1, *dev, "Removing device %p", dev.get());

  // This must be done before the RemoveDevice message is sent to
  // driver_manager, since driver_manager will close the channel in response.
  // The async loop may see the channel close before it sees the queued
  // shutdown packet, so it needs to check if dev->conn has been nulled to
  // handle that gracefully.
  dev->conn.store(nullptr);

  // Drop the device vnode, since no one should be able to open connections anymore.
  // This will break the reference cycle between the DevfsVnode and the zx_device.
  dev->vnode.reset();

  // respond to the remove fidl call
  dev->removal_cb(ZX_OK);

  // Forget our local ID, to release the reference stored by the local ID map
  dev->set_local_id(0);

  // Forget about our rpc channel since after the port_queue below it may be
  // closed.
  dev->rpc = zx::unowned_channel();
  dev->coordinator_rpc = zx::unowned_channel();

  // queue an event to destroy the connection
  ConnectionDestroyer::Get()->QueueDeviceControllerConnection(loop_.dispatcher(), conn);

  // shut down our proxy rpc channel if it exists
  ProxyIosDestroy(dev);

  return ZX_OK;
}

void DriverHostContext::ProxyIosDestroy(const fbl::RefPtr<zx_device_t>& dev) {
  fbl::AutoLock guard(&dev->proxy_ios_lock);

  if (dev->proxy_ios) {
    dev->proxy_ios->CancelLocked(loop_.dispatcher());
  }
}

zx_status_t DriverHostContext::FindDriver(std::string_view libname, zx::vmo vmo,
                                          fbl::RefPtr<zx_driver_t>* out) {
  // check for already-loaded driver first
  for (auto& drv : drivers_) {
    if (!libname.compare(drv.libname())) {
      *out = fbl::RefPtr(&drv);
      return drv.status();
    }
  }

  fbl::RefPtr<zx_driver> new_driver;
  zx_status_t status = zx_driver::Create(libname, inspect().drivers(), &new_driver);
  if (status != ZX_OK) {
    return status;
  }

  // Let the |drivers_| list and our out parameter each have a refcount.
  drivers_.push_back(new_driver);
  *out = new_driver;

  const char* c_libname = new_driver->libname().c_str();

  void* dl = dlopen_vmo(vmo.get(), RTLD_NOW);
  if (dl == nullptr) {
    LOGF(ERROR, "Cannot load '%s': %s", c_libname, dlerror());
    new_driver->set_status(ZX_ERR_IO);
    return new_driver->status();
  }

  auto dn = static_cast<const zircon_driver_note_t*>(dlsym(dl, "__zircon_driver_note__"));
  if (dn == nullptr) {
    LOGF(ERROR, "Driver '%s' missing __zircon_driver_note__ symbol", c_libname);
    new_driver->set_status(ZX_ERR_IO);
    return new_driver->status();
  }
  auto ops = static_cast<const zx_driver_ops_t**>(dlsym(dl, "__zircon_driver_ops__"));
  auto dr = static_cast<zx_driver_rec_t*>(dlsym(dl, "__zircon_driver_rec__"));
  if (dr == nullptr) {
    LOGF(ERROR, "Driver '%s' missing __zircon_driver_rec__ symbol", c_libname);
    new_driver->set_status(ZX_ERR_IO);
    return new_driver->status();
  }
  // TODO(kulakowski) Eventually just check __zircon_driver_ops__,
  // when bind programs are standalone.
  if (ops == nullptr) {
    ops = &dr->ops;
  }
  if (!(*ops)) {
    LOGF(ERROR, "Driver '%s' has nullptr ops", c_libname);
    new_driver->set_status(ZX_ERR_INVALID_ARGS);
    return new_driver->status();
  }
  if ((*ops)->version != DRIVER_OPS_VERSION) {
    LOGF(ERROR, "Driver '%s' has bad driver ops version %#lx, expecting %#lx", c_libname,
         (*ops)->version, DRIVER_OPS_VERSION);
    new_driver->set_status(ZX_ERR_INVALID_ARGS);
    return new_driver->status();
  }

  new_driver->set_driver_rec(dr);
  new_driver->set_name(dn->payload.name);
  new_driver->set_ops(*ops);
  dr->driver = new_driver.get();

  // Check for minimum log severity of driver.
  const auto flag_name = fbl::StringPrintf("driver.%s.log", new_driver->name());
  const char* flag_value = getenv(flag_name.data());
  if (flag_value != nullptr) {
    fx_log_severity_t min_severity = log_min_severity(new_driver->name(), flag_value);
    status = fx_logger_set_min_severity(new_driver->logger(), min_severity);
    if (status != ZX_OK) {
      LOGF(ERROR, "Failed to set minimum log severity for driver '%s': %s", new_driver->name(),
           zx_status_get_string(status));
    } else {
      LOGF(INFO, "Driver '%s' set minimum log severity to %d", new_driver->name(), min_severity);
    }
  }

  if (new_driver->has_init_op()) {
    new_driver->set_status(new_driver->InitOp());
    if (new_driver->status() != ZX_OK) {
      LOGF(ERROR, "Driver '%s' failed in init: %s", c_libname,
           zx_status_get_string(new_driver->status()));
    }
  } else {
    new_driver->set_status(ZX_OK);
  }

  return new_driver->status();
}

namespace internal {

namespace {

// We need a global pointer to a DriverHostContext so that we can implement the functions exported
// to drivers.  Some of these functions unfortunately do not take an argument that can be used to
// find a context.
DriverHostContext* kContextForApi = nullptr;

}  // namespace

void RegisterContextForApi(DriverHostContext* context) {
  ZX_ASSERT((context == nullptr) != (kContextForApi == nullptr));
  kContextForApi = context;
}
DriverHostContext* ContextForApi() { return kContextForApi; }

void DevhostControllerConnection::CreateDevice(zx::channel coordinator_rpc,
                                               zx::channel device_controller_rpc,
                                               ::fidl::StringView driver_path_view,
                                               ::zx::vmo driver_vmo, ::zx::handle parent_proxy,
                                               ::fidl::StringView proxy_args,
                                               uint64_t local_device_id,
                                               CreateDeviceCompleter::Sync& completer) {
  std::string_view driver_path(driver_path_view.data(), driver_path_view.size());
  // This does not operate under the driver_host api lock,
  // since the newly created device is not visible to
  // any API surface until a driver is bound to it.
  // (which can only happen via another message on this thread)

  // named driver -- ask it to create the device
  fbl::RefPtr<zx_driver_t> drv;
  zx_status_t r = driver_host_context_->FindDriver(driver_path, std::move(driver_vmo), &drv);
  if (r != ZX_OK) {
    LOGF(ERROR, "Failed to load driver '%.*s': %s", driver_path.size(), driver_path.data(),
         zx_status_get_string(r));
    return;
  }
  if (!drv->has_create_op()) {
    LOGF(ERROR, "Driver does not support create operation");
    return;
  }

  // Create a dummy parent device for use in this call to Create
  fbl::RefPtr<zx_device> parent;
  r = zx_device::Create(driver_host_context_, "device_create dummy", drv.get(), &parent);
  if (r != ZX_OK) {
    LOGF(ERROR, "Failed to create device: %s", zx_status_get_string(r));
    return;
  }
  // magic cookie for device create handshake
  CreationContext creation_context = {
      .parent = std::move(parent),
      .child = nullptr,
      .device_controller_rpc = zx::unowned_channel(device_controller_rpc),
      .coordinator_rpc = zx::unowned_channel(coordinator_rpc),
  };

  r = drv->CreateOp(&creation_context, creation_context.parent, "proxy", proxy_args.data(),
                    parent_proxy.release());

  // Suppress a warning about dummy device being in a bad state.  The
  // message is spurious in this case, since the dummy parent never
  // actually begins its device lifecycle.  This flag is ordinarily
  // set by device_remove().
  creation_context.parent->set_flag(DEV_FLAG_DEAD);

  if (r != ZX_OK) {
    LOGF(ERROR, "Failed to create driver: %s", zx_status_get_string(r));
    return;
  }

  auto new_device = std::move(creation_context.child);
  if (new_device == nullptr) {
    LOGF(ERROR, "Driver did not create a device");
    return;
  }

  new_device->set_local_id(local_device_id);
  std::unique_ptr<DeviceControllerConnection> newconn;
  r = DeviceControllerConnection::Create(driver_host_context_, std::move(new_device),
                                         std::move(device_controller_rpc),
                                         std::move(coordinator_rpc), &newconn);
  if (r != ZX_OK) {
    return;
  }

  // TODO: inform devcoord
  VLOGF(1, "Created device %p '%.*s'", new_device.get(), driver_path.size(), driver_path.data());
  r = DeviceControllerConnection::BeginWait(std::move(newconn),
                                            driver_host_context_->loop().dispatcher());
  if (r != ZX_OK) {
    LOGF(ERROR, "Failed to wait for device controller connection: %s", zx_status_get_string(r));
    return;
  }
}

void DevhostControllerConnection::CreateCompositeDevice(
    zx::channel coordinator_rpc, zx::channel device_controller_rpc,
    ::fidl::VectorView<::llcpp::fuchsia::device::manager::Fragment> fragments,
    ::fidl::StringView name, uint64_t local_device_id,
    CreateCompositeDeviceCompleter::Sync& completer) {
  // Convert the fragment IDs into zx_device references
  CompositeFragments fragments_list(new CompositeFragment[fragments.count()], fragments.count());
  {
    // Acquire the API lock so that we don't have to worry about concurrent
    // device removes
    fbl::AutoLock lock(&driver_host_context_->api_lock());

    for (size_t i = 0; i < fragments.count(); ++i) {
      const auto& fragment = fragments.data()[i];
      uint64_t local_id = fragment.id;
      fbl::RefPtr<zx_device_t> dev = zx_device::GetDeviceFromLocalId(local_id);
      if (dev == nullptr || (dev->flags() & DEV_FLAG_DEAD)) {
        completer.Reply(ZX_ERR_NOT_FOUND);
        return;
      }
      fragments_list[i].name = std::string(fragment.name.data(), fragment.name.size());
      fragments_list[i].device = std::move(dev);
    }
  }

  auto driver = GetCompositeDriver(driver_host_context_);
  if (driver == nullptr) {
    completer.Reply(ZX_ERR_INTERNAL);
    return;
  }

  fbl::RefPtr<zx_device_t> dev;
  static_assert(fuchsia_device_manager_DEVICE_NAME_MAX + 1 >= sizeof(dev->name()));
  zx_status_t status = zx_device::Create(driver_host_context_,
                                         std::string(name.data(), name.size()), driver.get(), &dev);
  if (status != ZX_OK) {
    completer.Reply(status);
    return;
  }
  dev->set_local_id(local_device_id);

  std::unique_ptr<DeviceControllerConnection> newconn;
  status = DeviceControllerConnection::Create(driver_host_context_, dev,
                                              std::move(device_controller_rpc),
                                              std::move(coordinator_rpc), &newconn);
  if (status != ZX_OK) {
    completer.Reply(status);
    return;
  }

  status = InitializeCompositeDevice(dev, std::move(fragments_list));
  if (status != ZX_OK) {
    completer.Reply(status);
    return;
  }

  VLOGF(1, "Created composite device %p '%s'", dev.get(), dev->name());
  status = DeviceControllerConnection::BeginWait(std::move(newconn),
                                                 driver_host_context_->loop().dispatcher());
  if (status != ZX_OK) {
    completer.Reply(status);
    return;
  }
  completer.Reply(ZX_OK);
}

void DevhostControllerConnection::CreateDeviceStub(zx::channel coordinator_rpc,
                                                   zx::channel device_controller_rpc,
                                                   uint32_t protocol_id, uint64_t local_device_id,
                                                   CreateDeviceStubCompleter::Sync& completer) {
  // This method is used for creating driverless proxies in case of misc, root, test devices.
  // Since there are no proxy drivers backing the device, a dummy proxy driver will be used for
  // device creation.
  if (!proxy_driver_) {
    auto status =
        zx_driver::Create("proxy", driver_host_context_->inspect().drivers(), &proxy_driver_);
    if (status != ZX_OK) {
      return;
    }
  }

  fbl::RefPtr<zx_device_t> dev;
  zx_status_t r = zx_device::Create(driver_host_context_, "proxy", proxy_driver_.get(), &dev);
  // TODO: dev->ops() and other lifecycle bits
  // no name means a dummy proxy device
  if (r != ZX_OK) {
    return;
  }
  dev->set_protocol_id(protocol_id);
  dev->set_ops(&kDeviceDefaultOps);
  dev->set_local_id(local_device_id);

  std::unique_ptr<DeviceControllerConnection> newconn;
  r = DeviceControllerConnection::Create(driver_host_context_, dev,
                                         std::move(device_controller_rpc),
                                         std::move(coordinator_rpc), &newconn);
  if (r != ZX_OK) {
    return;
  }
  VLOGF(1, "Created device stub %p '%s'", dev.get(), dev->name());
  r = DeviceControllerConnection::BeginWait(std::move(newconn),
                                            driver_host_context_->loop().dispatcher());
  if (r != ZX_OK) {
    return;
  }
}

zx_status_t DevhostControllerConnection::HandleRead() {
  zx::unowned_channel conn = channel();
  uint8_t msg[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t hin[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t msize = sizeof(msg);
  uint32_t hcount = std::size(hin);
  zx_status_t status = conn->read(0, msg, hin, msize, hcount, &msize, &hcount);
  if (status != ZX_OK) {
    return status;
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
  DevmgrFidlTxn txn(std::move(conn), hdr->txid);
  fuchsia::device::manager::DevhostController::Dispatch(this, &fidl_msg, &txn);
  return txn.Status();
}

// handles devcoordinator rpc

void DevhostControllerConnection::HandleRpc(std::unique_ptr<DevhostControllerConnection> conn,
                                            async_dispatcher_t* dispatcher, async::WaitBase* wait,
                                            zx_status_t status, const zx_packet_signal_t* signal) {
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to wait on %p from driver_manager: %s", conn.get(),
         zx_status_get_string(status));
    return;
  }
  if (signal->observed & ZX_CHANNEL_READABLE) {
    status = conn->HandleRead();
    if (status != ZX_OK) {
      LOGF(FATAL, "Unhandled RPC on %p from driver_manager: %s", conn.get(),
           zx_status_get_string(status));
    }
    BeginWait(std::move(conn), dispatcher);
    return;
  }
  if (signal->observed & ZX_CHANNEL_PEER_CLOSED) {
    // This is expected in test environments where driver_manager has terminated.
    // TODO(fxbug.dev/52627): Support graceful termination.
    LOGF(WARNING, "Disconnected %p from driver_manager", conn.get());
    exit(1);
  }
  LOGF(WARNING, "Unexpected signal state %#08x", signal->observed);
  BeginWait(std::move(conn), dispatcher);
}

int main(int argc, char** argv) {
  char process_name[ZX_MAX_NAME_LEN] = {};
  zx::process::self()->get_property(ZX_PROP_NAME, process_name, sizeof(process_name));
  const char* tags[] = {process_name, "device"};
  fx_logger_config_t config{
      .min_severity = getenv_bool("devmgr.verbose", false) ? FX_LOG_ALL : FX_LOG_SEVERITY_DEFAULT,
      .console_fd = getenv_bool("devmgr.log-to-debuglog", false) ? dup(STDOUT_FILENO) : -1,
      .log_service_channel = ZX_HANDLE_INVALID,
      .tags = tags,
      .num_tags = std::size(tags),
  };
  zx_status_t status = fx_log_reconfigure(&config);
  if (status != ZX_OK) {
    return status;
  }

  zx::resource root_resource(zx_take_startup_handle(PA_HND(PA_RESOURCE, 0)));
  if (!root_resource.is_valid()) {
    LOGF(WARNING, "No root resource handle");
  }

  zx::channel root_conn_channel(zx_take_startup_handle(PA_HND(PA_USER0, 0)));
  if (!root_conn_channel.is_valid()) {
    LOGF(ERROR, "Invalid root connection to driver_manager");
    return ZX_ERR_BAD_HANDLE;
  }

  DriverHostContext ctx(&kAsyncLoopConfigAttachToCurrentThread, std::move(root_resource));
  RegisterContextForApi(&ctx);

  status = connect_scheduler_profile_provider();
  if (status != ZX_OK) {
    LOGF(INFO, "Failed to connect to profile provider: %s", zx_status_get_string(status));
    return status;
  }

  if (getenv_bool("driver.tracing.enable", true)) {
    status = start_trace_provider();
    if (status != ZX_OK) {
      LOGF(INFO, "Failed to register trace provider: %s", zx_status_get_string(status));
      // This is not a fatal error.
    }
  }
  auto stop_tracing = fbl::AutoCall([]() { stop_trace_provider(); });

  status = ctx.SetupRootDevcoordinatorConnection(std::move(root_conn_channel));
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to watch root connection to driver_manager: %s",
         zx_status_get_string(status));
    return status;
  }

  status = ctx.inspect().Serve(zx::channel(zx_take_startup_handle(PA_DIRECTORY_REQUEST)),
                               ctx.loop().dispatcher());
  if (status != ZX_OK) {
    LOGF(WARNING, "driver_host: error serving diagnostics directory: %s\n",
         zx_status_get_string(status));
    // This is not a fatal error
  }

  status = ctx.SetupEventWaiter();
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to setup event watcher: %s", zx_status_get_string(status));
    return status;
  }

  return ctx.loop().Run(zx::time::infinite(), false /* once */);
}

}  // namespace internal

void DriverHostContext::MakeVisible(const fbl::RefPtr<zx_device_t>& dev,
                                    const device_make_visible_args_t* args) {
  ZX_ASSERT_MSG(!dev->ops()->init,
                "Cannot call device_make_visible if init hook is implemented."
                "The device will automatically be made visible once the init hook is replied to.");
  const zx::channel& rpc = *dev->coordinator_rpc;
  if (!rpc.is_valid()) {
    return;
  }

  if (args && args->power_states && args->power_state_count != 0) {
    dev->SetPowerStates(args->power_states, args->power_state_count);
  }
  if (args && args->performance_states && (args->performance_state_count != 0)) {
    dev->SetPerformanceStates(args->performance_states, args->performance_state_count);
  }

  // TODO(teisenbe): Handle failures here...
  VLOGD(1, *dev, "make-visible");
  auto response =
      fuchsia::device::manager::Coordinator::Call::MakeVisible(zx::unowned_channel(rpc.get()));
  zx_status_t status = response.status();
  zx_status_t call_status = ZX_OK;
  if (status == ZX_OK) {
    if (response.Unwrap()->result.is_err()) {
      call_status = response.Unwrap()->result.err();
    }
  }
  log_rpc_result(dev, "make-visible", status, call_status);
  dev->unset_flag(DEV_FLAG_INVISIBLE);

  // Reply to any pending bind/rebind requests, if all
  // the children are initialized.
  bool reply_bind_rebind = true;
  for (auto& child : dev->parent()->children()) {
    if (child.flags() & DEV_FLAG_INVISIBLE) {
      reply_bind_rebind = false;
    }
  }
  if (!reply_bind_rebind || !dev->parent()->complete_bind_rebind_after_init()) {
    return;
  }
  status = (status == ZX_OK) ? call_status : status;
  if (auto bind_conn = dev->parent()->take_bind_conn(); bind_conn) {
    bind_conn(status);
  }
  if (auto rebind_conn = dev->parent()->take_rebind_conn(); rebind_conn) {
    rebind_conn(status);
  }
}

zx_status_t DriverHostContext::ScheduleRemove(const fbl::RefPtr<zx_device_t>& dev,
                                              bool unbind_self) {
  const zx::channel& rpc = *dev->coordinator_rpc;
  ZX_ASSERT(rpc.is_valid());
  VLOGD(1, *dev, "schedule-remove");
  auto resp = fuchsia::device::manager::Coordinator::Call::ScheduleRemove(
      zx::unowned_channel(rpc.get()), unbind_self);
  log_rpc_result(dev, "schedule-remove", resp.status());
  return resp.status();
}

zx_status_t DriverHostContext::ScheduleUnbindChildren(const fbl::RefPtr<zx_device_t>& dev) {
  const zx::channel& rpc = *dev->coordinator_rpc;
  ZX_ASSERT(rpc.is_valid());
  VLOGD(1, *dev, "schedule-unbind-children");
  auto resp = fuchsia::device::manager::Coordinator::Call::ScheduleUnbindChildren(
      zx::unowned_channel(rpc.get()));
  log_rpc_result(dev, "schedule-unbind-children", resp.status());
  return resp.status();
}

zx_status_t DriverHostContext::GetTopoPath(const fbl::RefPtr<zx_device_t>& dev, char* path,
                                           size_t max, size_t* actual) {
  fbl::RefPtr<zx_device_t> remote_dev = dev;
  if (dev->flags() & DEV_FLAG_INSTANCE) {
    // Instances cannot be opened a second time. If dev represents an instance, return the path
    // to its parent, prefixed with an '@'.
    if (max < 1) {
      return ZX_ERR_BUFFER_TOO_SMALL;
    }
    path[0] = '@';
    path++;
    max--;
    remote_dev = dev->parent();
  }

  const zx::channel& rpc = *remote_dev->coordinator_rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }

  VLOGD(1, *remote_dev, "get-topo-path");
  auto response = fuchsia::device::manager::Coordinator::Call::GetTopologicalPath(
      zx::unowned_channel(rpc.get()));
  zx_status_t status = response.status();
  zx_status_t call_status = ZX_OK;
  if (status == ZX_OK) {
    if (response.Unwrap()->result.is_err()) {
      call_status = response.Unwrap()->result.err();
    } else {
      auto& r = response.Unwrap()->result.response();
      memcpy(path, r.path.data(), r.path.size());
      *actual = r.path.size();
    }
  }

  log_rpc_result(dev, "get-topo-path", status, call_status);
  if (status != ZX_OK) {
    return status;
  }
  if (call_status != ZX_OK) {
    return status;
  }

  path[*actual] = 0;
  *actual += 1;

  // Account for the prefixed '@' we may have added above.
  if (dev->flags() & DEV_FLAG_INSTANCE) {
    *actual += 1;
  }
  return ZX_OK;
}

zx_status_t DriverHostContext::DeviceBind(const fbl::RefPtr<zx_device_t>& dev,
                                          const char* drv_libname) {
  const zx::channel& rpc = *dev->coordinator_rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  VLOGD(1, *dev, "bind-device");
  auto driver_path = ::fidl::unowned_str(drv_libname, strlen(drv_libname));
  auto response = fuchsia::device::manager::Coordinator::Call::BindDevice(
      zx::unowned_channel(rpc.get()), std::move(driver_path));
  zx_status_t status = response.status();
  zx_status_t call_status = ZX_OK;
  if (status == ZX_OK && response.Unwrap()->result.is_err()) {
    call_status = response.Unwrap()->result.err();
  }
  log_rpc_result(dev, "bind-device", status, call_status);
  if (status != ZX_OK) {
    return status;
  }

  return call_status;
}

zx_status_t DriverHostContext::DeviceRunCompatibilityTests(const fbl::RefPtr<zx_device_t>& dev,
                                                           int64_t hook_wait_time) {
  const zx::channel& rpc = *dev->coordinator_rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  VLOGD(1, *dev, "run-compatibility-test");
  auto response = fuchsia::device::manager::Coordinator::Call::RunCompatibilityTests(
      zx::unowned_channel(rpc.get()), hook_wait_time);
  zx_status_t status = response.status();
  zx_status_t call_status = ZX_OK;
  if (status == ZX_OK && response.Unwrap()->result.is_err()) {
    call_status = response.Unwrap()->result.err();
  }
  log_rpc_result(dev, "run-compatibility-test", status, call_status);
  if (status != ZX_OK) {
    return status;
  }
  return call_status;
}

zx_status_t DriverHostContext::LoadFirmware(const fbl::RefPtr<zx_device_t>& dev, const char* path,
                                            zx_handle_t* vmo_handle, size_t* size) {
  if ((vmo_handle == nullptr) || (size == nullptr)) {
    return ZX_ERR_INVALID_ARGS;
  }

  zx::vmo vmo;
  const zx::channel& rpc = *dev->coordinator_rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  VLOGD(1, *dev, "load-firmware");
  auto str_path = ::fidl::unowned_str(path, strlen(path));
  auto response = fuchsia::device::manager::Coordinator::Call::LoadFirmware(
      zx::unowned_channel(rpc.get()), std::move(str_path));
  zx_status_t status = response.status();
  zx_status_t call_status = ZX_OK;
  auto result = std::move(response.Unwrap()->result);
  if (result.is_err()) {
    call_status = result.err();
  } else {
    auto resp = std::move(result.mutable_response());
    *size = resp.size;
    vmo = std::move(resp.vmo);
  }
  log_rpc_result(dev, "load-firmware", status, call_status);
  if (status != ZX_OK) {
    return status;
  }
  *vmo_handle = vmo.release();
  if (call_status == ZX_OK && *vmo_handle == ZX_HANDLE_INVALID) {
    return ZX_ERR_INTERNAL;
  }
  return call_status;
}

zx_status_t DriverHostContext::GetMetadata(const fbl::RefPtr<zx_device_t>& dev, uint32_t type,
                                           void* buf, size_t buflen, size_t* actual) {
  if (!buf) {
    return ZX_ERR_INVALID_ARGS;
  }

  const zx::channel& rpc = *dev->coordinator_rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  VLOGD(1, *dev, "get-metadata");
  auto response = fuchsia::device::manager::Coordinator::Call::GetMetadata(
      zx::unowned_channel(rpc.get()), type);
  zx_status_t status = response.status();
  zx_status_t call_status = ZX_OK;
  if (status == ZX_OK) {
    if (response->result.is_response()) {
      const auto& r = response.Unwrap()->result.mutable_response();
      if (r.data.count() > buflen) {
        return ZX_ERR_BUFFER_TOO_SMALL;
      }
      memcpy(buf, r.data.data(), r.data.count());
      if (actual != nullptr) {
        *actual = r.data.count();
      }
    } else {
      call_status = response->result.err();
    }
  }
  return log_rpc_result(dev, "get-metadata", status, call_status);
}

zx_status_t DriverHostContext::GetMetadataSize(const fbl::RefPtr<zx_device_t>& dev, uint32_t type,
                                               size_t* out_length) {
  const zx::channel& rpc = *dev->coordinator_rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  VLOGD(1, *dev, "get-metadata-size");
  auto response = fuchsia::device::manager::Coordinator::Call::GetMetadataSize(
      zx::unowned_channel(rpc.get()), type);
  zx_status_t status = response.status();
  zx_status_t call_status = ZX_OK;
  if (status == ZX_OK) {
    if (response->result.is_response()) {
      *out_length = response->result.response().size;
    } else {
      call_status = response->result.err();
    }
  }
  return log_rpc_result(dev, "get-metadata-size", status, call_status);
}

zx_status_t DriverHostContext::AddMetadata(const fbl::RefPtr<zx_device_t>& dev, uint32_t type,
                                           const void* data, size_t length) {
  if (!data && length) {
    return ZX_ERR_INVALID_ARGS;
  }
  const zx::channel& rpc = *dev->coordinator_rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  VLOGD(1, *dev, "add-metadata");
  auto response = fuchsia::device::manager::Coordinator::Call::AddMetadata(
      zx::unowned_channel(rpc.get()), type,
      ::fidl::VectorView(fidl::unowned_ptr(reinterpret_cast<uint8_t*>(const_cast<void*>(data))),
                         length));
  zx_status_t status = response.status();
  zx_status_t call_status = ZX_OK;
  if (status == ZX_OK && response->result.is_err()) {
    call_status = response->result.err();
  }
  return log_rpc_result(dev, "add-metadata", status, call_status);
}

zx_status_t DriverHostContext::PublishMetadata(const fbl::RefPtr<zx_device_t>& dev,
                                               const char* path, uint32_t type, const void* data,
                                               size_t length) {
  if (!path || (!data && length)) {
    return ZX_ERR_INVALID_ARGS;
  }
  const zx::channel& rpc = *dev->coordinator_rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }
  VLOGD(1, *dev, "publish-metadata");
  auto response = fuchsia::device::manager::Coordinator::Call::PublishMetadata(
      zx::unowned_channel(rpc.get()), ::fidl::unowned_str(path, strlen(path)), type,
      ::fidl::VectorView(fidl::unowned_ptr(reinterpret_cast<uint8_t*>(const_cast<void*>(data))),
                         length));
  zx_status_t status = response.status();
  zx_status_t call_status = ZX_OK;
  if (status == ZX_OK && response->result.is_err()) {
    call_status = response->result.err();
  }
  return log_rpc_result(dev, "publish-metadata", status, call_status);
}

zx_status_t DriverHostContext::DeviceAddComposite(const fbl::RefPtr<zx_device_t>& dev,
                                                  const char* name,
                                                  const composite_device_desc_t* comp_desc) {
  if (comp_desc == nullptr || (comp_desc->props == nullptr && comp_desc->props_count > 0) ||
      comp_desc->fragments == nullptr || name == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  const zx::channel& rpc = *dev->coordinator_rpc;
  if (!rpc.is_valid()) {
    return ZX_ERR_IO_REFUSED;
  }

  VLOGD(1, *dev, "create-composite");
  std::vector<fuchsia::device::manager::DeviceFragment> compvec = {};
  for (size_t i = 0; i < comp_desc->fragments_count; i++) {
    ::fidl::Array<fuchsia::device::manager::DeviceFragmentPart,
                  fuchsia::device::manager::DEVICE_FRAGMENT_PARTS_MAX>
        parts{};
    for (uint32_t j = 0; j < comp_desc->fragments[i].parts_count; j++) {
      ::fidl::Array<fuchsia::device::manager::BindInstruction,
                    fuchsia::device::manager::DEVICE_FRAGMENT_PART_INSTRUCTIONS_MAX>
          bind_instructions{};
      for (uint32_t k = 0; k < comp_desc->fragments[i].parts[j].instruction_count; k++) {
        bind_instructions[k] = fuchsia::device::manager::BindInstruction{
            .op = comp_desc->fragments[i].parts[j].match_program[k].op,
            .arg = comp_desc->fragments[i].parts[j].match_program[k].arg,
            .debug = comp_desc->fragments[i].parts[j].match_program[k].debug,
        };
      }
      auto part = fuchsia::device::manager::DeviceFragmentPart{
          .match_program_count = comp_desc->fragments[i].parts[j].instruction_count,
          .match_program = bind_instructions,
      };
      parts[j] = part;
    }
    auto dc = fuchsia::device::manager::DeviceFragment{
        .name = ::fidl::StringView{fidl::unowned_ptr(comp_desc->fragments[i].name),
                                   strnlen(comp_desc->fragments[i].name, 32)},
        .parts_count = comp_desc->fragments[i].parts_count,
        .parts = parts,
    };
    compvec.push_back(std::move(dc));
  }

  std::vector<fuchsia::device::manager::DeviceMetadata> metadata = {};
  for (size_t i = 0; i < comp_desc->metadata_count; i++) {
    auto meta = fuchsia::device::manager::DeviceMetadata{
        .key = comp_desc->metadata_list[i].type,
        .data = fidl::VectorView(fidl::unowned_ptr(reinterpret_cast<uint8_t*>(
                                     const_cast<void*>(comp_desc->metadata_list[i].data))),
                                 comp_desc->metadata_list[i].length)};
    metadata.emplace_back(std::move(meta));
  }

  std::vector<llcpp::fuchsia::device::manager::DeviceProperty> props = {};
  for (size_t i = 0; i < comp_desc->props_count; i++) {
    props.push_back(convert_device_prop(comp_desc->props[i]));
  }

  fuchsia::device::manager::CompositeDeviceDescriptor comp_dev = {
      .props = ::fidl::unowned_vec(props),
      .fragments = ::fidl::unowned_vec(compvec),
      .coresident_device_index = comp_desc->coresident_device_index,
      .metadata = ::fidl::unowned_vec(metadata)};

  static_assert(sizeof(comp_desc->props[0]) == sizeof(uint64_t));
  auto response = fuchsia::device::manager::Coordinator::Call::AddCompositeDevice(
      zx::unowned_channel(rpc.get()), ::fidl::unowned_str(name, strlen(name)), std::move(comp_dev));
  zx_status_t status = response.status();
  zx_status_t call_status = ZX_OK;
  if (status == ZX_OK && response->result.is_err()) {
    call_status = response->result.err();
  }
  return log_rpc_result(dev, "create-composite", status, call_status);
}
