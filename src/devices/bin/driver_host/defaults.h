// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include <fuchsia/device/llcpp/fidl.h>
#include <fuchsia/device/manager/llcpp/fidl.h>

#include <ddk/device.h>

namespace internal {

extern const zx_protocol_device_t kDeviceDefaultOps;
extern const device_power_state_info_t kDeviceDefaultPowerStates[2];
extern const device_performance_state_info_t kDeviceDefaultPerfStates[1];
extern const std::array<::llcpp::fuchsia::device::SystemPowerStateInfo,
                        ::llcpp::fuchsia::device::manager::MAX_SYSTEM_POWER_STATES>
    kDeviceDefaultStateMapping;

}  // namespace internal
