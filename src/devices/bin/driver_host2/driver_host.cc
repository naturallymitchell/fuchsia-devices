// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "driver_host.h"

#include <fuchsia/io/llcpp/fidl.h>
#include <lib/async-loop/loop.h>
#include <lib/fdio/directory.h>
#include <zircon/dlfcn.h>

#include <fs/service.h>

#include "src/devices/lib/driver2/start_args.h"
#include "src/devices/lib/log/log.h"

namespace fdf = llcpp::fuchsia::driver::framework;
namespace fio = llcpp::fuchsia::io;
namespace frunner = llcpp::fuchsia::component::runner;

namespace {

DriverRecordV1* find_record(void* library) {
  if (library == nullptr) {
    return nullptr;
  }
  auto record = static_cast<DriverRecordV1*>(dlsym(library, "__fuchsia_driver_record__"));
  if (record->version != 1) {
    LOGF(ERROR, "Failed to start driver, unknown driver record version: %lu", record->version);
    return nullptr;
  }
  return record;
}

}  // namespace

Driver::Driver(zx::vmo vmo)
    : library_(dlopen_vmo(vmo.get(), RTLD_NOW)), record_(find_record(library_)), opaque_(nullptr) {}

Driver::~Driver() {
  zx_status_t status = record_->stop(opaque_);
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to stop driver: %s", zx_status_get_string(status));
  }
  dlclose(library_);

  if (binding_.has_value()) {
    binding_->Unbind();
  }
}

bool Driver::ok() const { return record_ != nullptr; }

void Driver::set_binding(
    fidl::ServerBindingRef<llcpp::fuchsia::driver::framework::Driver> binding) {
  binding_ = std::make_optional(std::move(binding));
}

zx::status<> Driver::Start(fidl_msg_t* msg, async_dispatcher_t* dispatcher) {
  zx_status_t status = record_->start(msg, dispatcher, &opaque_);
  return zx::make_status(status);
}

DriverHost::DriverHost(async_dispatcher_t* dispatcher) : dispatcher_(dispatcher) {}

zx::status<> DriverHost::PublishDriverHost(const fbl::RefPtr<fs::PseudoDir>& svc_dir) {
  const auto service = [this](zx::channel request) {
    auto result = fidl::BindServer(dispatcher_, std::move(request), this);
    if (result.is_error()) {
      LOGF(ERROR, "Failed to bind channel to '%s': %s", fdf::DriverHost::Name,
           zx_status_get_string(result.error()));
      return result.error();
    }
    return ZX_OK;
  };
  zx_status_t status =
      svc_dir->AddEntry(fdf::DriverHost::Name, fbl::MakeRefCounted<fs::Service>(service));
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to add directory entry '%s': %s", fdf::DriverHost::Name,
         zx_status_get_string(status));
  }
  return zx::make_status(status);
}

void DriverHost::Start(fdf::DriverStartArgs start_args, zx::channel request,
                       StartCompleter::Sync& completer) {
  auto pkg = start_args.has_ns() ? start_args::ns_value(start_args.ns(), "/pkg")
                                 : zx::error(ZX_ERR_INVALID_ARGS);
  if (pkg.is_error()) {
    LOGF(ERROR, "Failed to start driver, missing '/pkg' directory: %s",
         zx_status_get_string(pkg.error_value()));
    completer.Close(pkg.error_value());
    return;
  }
  auto binary = start_args.has_program() ? start_args::program_value(start_args.program(), "binary")
                                         : zx::error(ZX_ERR_INVALID_ARGS);
  if (binary.is_error()) {
    LOGF(ERROR, "Failed to start driver, missing 'binary' argument: %s",
         zx_status_get_string(binary.error_value()));
    completer.Close(binary.error_value());
    return;
  }
  // Open the driver's binary within the driver's package.
  zx::channel client_end, server_end;
  zx_status_t status = zx::channel::create(0, &client_end, &server_end);
  if (status != ZX_OK) {
    completer.Close(status);
    return;
  }
  status =
      fdio_open_at(pkg.value()->get(), binary.value().data(),
                   fio::OPEN_RIGHT_READABLE | fio::OPEN_RIGHT_EXECUTABLE, server_end.release());
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to start driver '/pkg/%s', could not open library: %s",
         binary.value().data(), zx_status_get_string(status));
    completer.Close(status);
    return;
  }
  // We encode start_args outside of callback in order to access stack-allocated
  // data before it is destroyed.
  auto storage = std::make_unique<start_args::Storage>();
  const char* error;
  auto encode = start_args::Encode(storage.get(), std::move(start_args), &error);
  if (encode.is_error()) {
    LOGF(ERROR, "Failed to start driver '/pkg/%s', could not encode start args: %s",
         binary.value().data(), error);
    completer.Close(encode.error_value());
    return;
  }
  // Once we receive the VMO from the call to GetBuffer, we can load the driver
  // into this driver host. We move the storage and encoded for start_args into
  // this callback to extend its lifetime.
  fidl::Client<fio::File> file(std::move(client_end), dispatcher_);
  auto file_ptr = file.get();
  auto callback = [this, request = std::move(request), completer = completer.ToAsync(),
                   binary = std::move(binary.value()), storage = std::move(storage),
                   msg = encode.value(),
                   file = std::move(file)](zx_status_t status, auto buffer) mutable {
    if (status != ZX_OK) {
      LOGF(ERROR, "Failed to start driver '/pkg/%s', could not get library VMO: %s", binary.data(),
           zx_status_get_string(status));
      completer.Close(status);
      return;
    }
    status = buffer->vmo.set_property(ZX_PROP_NAME, binary.data(), binary.size());
    if (status != ZX_OK) {
      LOGF(ERROR, "Failed to start driver '/pkg/%s', could not name library VMO: %s", binary.data(),
           zx_status_get_string(status));
      completer.Close(status);
      return;
    }
    auto driver = std::make_unique<Driver>(std::move(buffer->vmo));
    if (!driver->ok()) {
      LOGF(ERROR, "Failed to start driver '/pkg/%s', could not load library", binary.data());
      completer.Close(ZX_ERR_INTERNAL);
      return;
    }
    auto driver_ptr = driver.get();
    auto bind = fidl::BindServer<Driver>(
        dispatcher_, std::move(request), driver_ptr, [this](Driver* driver, auto, auto) {
          drivers_.erase(*driver);
          // If this is the last driver, shutdown the driver host.
          if (drivers_.is_empty()) {
            async_loop_quit(async_loop_from_dispatcher(dispatcher_));
          }
        });
    if (bind.is_error()) {
      LOGF(ERROR,
           "Failed to start driver '/pkg/%s', could not bind channel to "
           "'fuchsia.driver.framework.DriverHost': %s",
           binary.data(), zx_status_get_string(bind.error()));
      completer.Close(bind.error());
      return;
    }
    driver->set_binding(bind.take_value());
    drivers_.push_back(std::move(driver));

    auto start = driver_ptr->Start(&msg, dispatcher_);
    if (start.is_error()) {
      LOGF(ERROR, "Failed to start driver '/pkg/%s': %s", binary.data(), start.status_string());
      completer.Close(start.error_value());
      return;
    }
    LOGF(INFO, "Started '%s'", binary.data());
  };
  file_ptr->GetBuffer(fio::VMO_FLAG_READ | fio::VMO_FLAG_EXEC, std::move(callback));
}
