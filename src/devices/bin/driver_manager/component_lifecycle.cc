// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "component_lifecycle.h"

#include <lib/fidl-async/cpp/bind.h>
#include <zircon/status.h>

#include "fuchsia/hardware/power/statecontrol/llcpp/fidl.h"
#include "src/devices/lib/log/log.h"

namespace power_fidl = llcpp::fuchsia::hardware::power;

namespace devmgr {

zx_status_t ComponentLifecycleServer::Create(async_dispatcher_t* dispatcher, Coordinator* dev_coord,
                                             zx::channel chan) {
  zx_status_t status = fidl::BindSingleInFlightOnly(
      dispatcher, std::move(chan), std::make_unique<ComponentLifecycleServer>(dev_coord));
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to bind component lifecycle service:%s", zx_status_get_string(status));
    return status;
  }
  return ZX_OK;
}

void ComponentLifecycleServer::Stop(StopCompleter::Sync completer) {
  auto callback = [completer = completer.ToAsync()](zx_status_t status) mutable {
    if (status != ZX_OK) {
      LOGF(ERROR, "Error suspending devices while stopping the component:%s",
           zx_status_get_string(status));
    }
    LOGF(INFO, "Exiting driver manager gracefully");
    // TODO(fxb:52627) This event handler should teardown devices and driver hosts
    // properly for system state transitions where driver manager needs to go down.
    // Exiting like so, will not run all the destructors and clean things up properly.
    // Instead the main devcoordinator loop should be quit.
    exit(0);
  };

  dev_coord_->Suspend(SuspendContext(SuspendContext::Flags::kSuspend,
                                     dev_coord_->GetSuspendFlagsFromSystemPowerState(
                                         dev_coord_->shutdown_system_state())),
                      std::move(callback));
}
}  // namespace devmgr
