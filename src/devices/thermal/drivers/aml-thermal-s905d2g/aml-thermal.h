// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_THERMAL_DRIVERS_AML_THERMAL_S905D2G_AML_THERMAL_H_
#define SRC_DEVICES_THERMAL_DRIVERS_AML_THERMAL_S905D2G_AML_THERMAL_H_

#include <fuchsia/hardware/composite/cpp/banjo.h>
#include <fuchsia/hardware/thermal/c/fidl.h>
#include <fuchsia/hardware/thermal/cpp/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl-utils/bind.h>
#include <threads.h>

#include <memory>
#include <utility>

#include <ddk/device.h>
#include <ddktl/device.h>

#include "aml-tsensor.h"

namespace thermal {

class AmlThermal;
using DeviceType = ddk::Device<AmlThermal, ddk::Unbindable, ddk::Messageable>;

class AmlThermal : public DeviceType, public ddk::ThermalProtocol<AmlThermal, ddk::base_protocol> {
 public:
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AmlThermal);
  AmlThermal(zx_device_t* device, std::unique_ptr<thermal::AmlTSensor> tsensor,
             fuchsia_hardware_thermal_ThermalDeviceInfo thermal_config)
      : DeviceType(device),
        tsensor_(std::move(tsensor)),
        thermal_config_(std::move(thermal_config)),
        loop_(&kAsyncLoopConfigNoAttachToCurrentThread) {}

  static zx_status_t Create(void* ctx, zx_device_t* device);

  // Ddk Hooks
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();
  zx_status_t DdkMessage(fidl_incoming_msg_t* msg, fidl_txn_t* txn);

  // Implements ZX_PROTOCOL_THERMAL
  zx_status_t ThermalConnect(zx::channel ch);

 private:
  zx_status_t GetInfo(fidl_txn_t* txn);
  zx_status_t GetDeviceInfo(fidl_txn_t* txn);
  zx_status_t GetDvfsInfo(fuchsia_hardware_thermal_PowerDomain power_domain, fidl_txn_t* txn);
  zx_status_t GetTemperatureCelsius(fidl_txn_t* txn);
  zx_status_t GetStateChangeEvent(fidl_txn_t* txn);
  zx_status_t GetStateChangePort(fidl_txn_t* txn);
  zx_status_t SetTripCelsius(uint32_t id, float temp, fidl_txn_t* txn);
  zx_status_t GetDvfsOperatingPoint(fuchsia_hardware_thermal_PowerDomain power_domain,
                                    fidl_txn_t* txn);
  zx_status_t SetDvfsOperatingPoint(uint16_t op_idx,
                                    fuchsia_hardware_thermal_PowerDomain power_domain,
                                    fidl_txn_t* txn);
  zx_status_t GetFanLevel(fidl_txn_t* txn);
  zx_status_t SetFanLevel(uint32_t fan_level, fidl_txn_t* txn);

  static constexpr fuchsia_hardware_thermal_Device_ops_t fidl_ops = {
      .GetTemperatureCelsius =
          fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetTemperatureCelsius>,
      .GetInfo = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetInfo>,
      .GetDeviceInfo = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetDeviceInfo>,
      .GetDvfsInfo = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetDvfsInfo>,
      .GetStateChangeEvent = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetStateChangeEvent>,
      .GetStateChangePort = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetStateChangePort>,
      .SetTripCelsius = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::SetTripCelsius>,
      .GetDvfsOperatingPoint =
          fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetDvfsOperatingPoint>,
      .SetDvfsOperatingPoint =
          fidl::Binder<AmlThermal>::BindMember<&AmlThermal::SetDvfsOperatingPoint>,
      .GetFanLevel = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::GetFanLevel>,
      .SetFanLevel = fidl::Binder<AmlThermal>::BindMember<&AmlThermal::SetFanLevel>,
  };

  zx_status_t StartConnectDispatchThread();

  std::unique_ptr<thermal::AmlTSensor> tsensor_;
  fuchsia_hardware_thermal_ThermalDeviceInfo thermal_config_;
  async::Loop loop_;
};
}  // namespace thermal

#endif  // SRC_DEVICES_THERMAL_DRIVERS_AML_THERMAL_S905D2G_AML_THERMAL_H_
