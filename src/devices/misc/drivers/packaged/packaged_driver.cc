// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/driver/framework/llcpp/fidl.h>
#include <lib/svc/outgoing.h>

#include "src/devices/lib/driver2/inspect.h"
#include "src/devices/lib/driver2/logger.h"
#include "src/devices/lib/driver2/namespace.h"
#include "src/devices/lib/driver2/record.h"

namespace {

namespace fdf = fuchsia_driver_framework;

class PackagedDriver {
 public:
  explicit PackagedDriver(async_dispatcher_t* dispatcher)
      : dispatcher_(dispatcher), outgoing_(dispatcher) {}

  zx::status<> Init(fdf::DriverStartArgs* start_args) {
    zx_status_t status = node_.Bind(std::move(start_args->node()), dispatcher_);
    if (status != ZX_OK) {
      return zx::error(status);
    }

    auto ns = Namespace::Create(start_args->ns());
    if (ns.is_error()) {
      return ns.take_error();
    }
    ns_ = std::move(ns.value());

    auto logger = Logger::Create(ns_, dispatcher_, "packaged_driver");
    if (logger.is_error()) {
      return logger.take_error();
    }
    logger_ = std::move(logger.value());

    auto inspect = ExposeInspector(inspector_, outgoing_.root_dir());
    if (inspect.is_error()) {
      FDF_LOG(ERROR, "Failed to expose inspector: %s", inspect.status_string());
      return inspect.take_error();
    }
    inspect_vmo_ = std::move(inspect.value());

    FDF_LOG(INFO, "Hello world");
    auto& root = inspector_.GetRoot();
    root.CreateString("hello", "world", &inspector_);
    status = outgoing_.Serve(std::move(start_args->outgoing_dir()));
    return zx::make_status(status);
  }

 private:
  async_dispatcher_t* dispatcher_;
  svc::Outgoing outgoing_;
  fidl::Client<fdf::Node> node_;
  Namespace ns_;
  Logger logger_;
  inspect::Inspector inspector_;
  zx::vmo inspect_vmo_;
};

zx_status_t PackagedDriverStart(fidl_incoming_msg_t* msg, async_dispatcher_t* dispatcher,
                                void** driver) {
  fdf::DriverStartArgs::DecodedMessage decoded(msg);
  if (!decoded.ok()) {
    return decoded.status();
  }

  auto packaged_driver = new PackagedDriver(dispatcher);
  // If we return failure at any point in this function, PackagedDriverStop()
  // will be called with the value of |driver|.
  *driver = packaged_driver;
  return packaged_driver->Init(decoded.PrimaryObject()).status_value();
}

zx_status_t PackagedDriverStop(void* driver) {
  delete static_cast<PackagedDriver*>(driver);
  return ZX_OK;
}

}  // namespace

FUCHSIA_DRIVER_RECORD_V1(.start = PackagedDriverStart, .stop = PackagedDriverStop);
