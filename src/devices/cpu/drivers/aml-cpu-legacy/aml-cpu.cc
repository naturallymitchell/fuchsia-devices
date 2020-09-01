// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-cpu.h"

#include <lib/device-protocol/pdev.h>
#include <lib/inspect/cpp/inspector.h>
#include <lib/mmio/mmio.h>

#include <memory>
#include <optional>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <ddktl/fidl.h>
#include <ddktl/protocol/composite.h>
#include <ddktl/protocol/platform/device.h>
#include <ddktl/protocol/thermal.h>

#include "fuchsia/hardware/thermal/llcpp/fidl.h"

namespace {
using llcpp::fuchsia::device::MAX_DEVICE_PERFORMANCE_STATES;
using llcpp::fuchsia::hardware::thermal::MAX_DVFS_DOMAINS;
using llcpp::fuchsia::hardware::thermal::PowerDomain;

constexpr size_t kFragmentPdev = 0;
constexpr size_t kFragmentThermal = 1;
constexpr size_t kFragmentCount = 2;
constexpr zx_off_t kCpuVersionOffset = 0x220;

uint16_t PstateToOperatingPoint(const uint32_t pstate, const size_t n_operating_points) {
  ZX_ASSERT(pstate < n_operating_points);
  ZX_ASSERT(n_operating_points < MAX_DEVICE_PERFORMANCE_STATES);

  // Operating points are indexed 0 to N-1.
  return static_cast<uint16_t>(n_operating_points - pstate - 1);
}

std::optional<amlogic_cpu::fuchsia_thermal::Device::SyncClient> CreateFidlClient(
    const ddk::ThermalProtocolClient& protocol_client, zx_status_t* status) {
  // This channel pair will be used to talk to the Thermal Device's FIDL
  // interface.
  zx::channel channel_local, channel_remote;
  *status = zx::channel::create(0, &channel_local, &channel_remote);
  if (*status != ZX_OK) {
    zxlogf(ERROR, "aml-cpu: Failed to create channel pair, st = %d\n", *status);
    return {};
  }

  // Pass one end of the channel to the Thermal driver. The thermal driver will
  // serve its FIDL interface over this channel.
  *status = protocol_client.Connect(std::move(channel_remote));
  if (*status != ZX_OK) {
    zxlogf(ERROR, "aml-cpu: failed to connect to thermal driver, st = %d\n", *status);
    return {};
  }

  return amlogic_cpu::fuchsia_thermal::Device::SyncClient(std::move(channel_local));
}

}  // namespace
namespace amlogic_cpu {

zx_status_t AmlCpu::Create(void* context, zx_device_t* parent) {
  zx_status_t status;

  ddk::CompositeProtocolClient composite(parent);
  if (!composite.is_valid()) {
    zxlogf(ERROR, "%s: failed to get composite protocol", __func__);
    return ZX_ERR_INTERNAL;
  }

  zx_device_t* devices[kFragmentCount];
  size_t actual;
  composite.GetFragments(devices, kFragmentCount, &actual);
  if (actual != kFragmentCount) {
    zxlogf(ERROR, "%s: Expected to get %lu fragments, actually got %lu", __func__, kFragmentCount,
           actual);
    return ZX_ERR_INTERNAL;
  }

  // Initialize an array with the maximum possible number of PStates since we
  // determine the actual number of PStates at runtime by querying the thermal
  // driver.
  device_performance_state_info_t perf_states[MAX_DEVICE_PERFORMANCE_STATES];
  for (size_t i = 0; i < MAX_DEVICE_PERFORMANCE_STATES; i++) {
    perf_states[i].state_id = static_cast<uint8_t>(i);
    perf_states[i].restore_latency = 0;
  }

  // The Thermal Driver is our parent and it exports an interface with one
  // method (Connect) which allows us to connect to its FIDL interface.
  zx_device_t* thermal_device = devices[kFragmentThermal];
  ddk::ThermalProtocolClient thermal_protocol_client;
  status = ddk::ThermalProtocolClient::CreateFromDevice(thermal_device, &thermal_protocol_client);
  if (status != ZX_OK) {
    zxlogf(ERROR, "aml-cpu: Failed to get thermal protocol client, st = %d", status);
    return status;
  }

  auto thermal_fidl_client = CreateFidlClient(thermal_protocol_client, &status);
  if (!thermal_fidl_client) {
    return status;
  }

  auto device_info = thermal_fidl_client->GetDeviceInfo();
  if (device_info.status() != ZX_OK) {
    zxlogf(ERROR, "aml-cpu: failed to get device info, st = %d", device_info.status());
    return device_info.status();
  }

  const fuchsia_thermal::ThermalDeviceInfo* info = device_info->info.get();

  // Ensure there is at least one non-empty power domain. We expect one to exist if this function
  // has been called.
  {
    bool found_nonempty_domain = false;
    for (size_t i = 0; i < MAX_DVFS_DOMAINS; i++) {
      if (info->opps[i].count > 0) {
        found_nonempty_domain = true;
        break;
      }
    }
    if (!found_nonempty_domain) {
      zxlogf(ERROR, "aml-cpu: No cpu devices were created; all power domains are empty\n");
      return ZX_ERR_INTERNAL;
    }
  }

  // Look up the CPU version.
  uint32_t cpu_version_packed = 0;
  {
    zx_device_t* platform_device = devices[kFragmentPdev];
    ddk::PDev pdev_client(platform_device);

    // Map AOBUS registers
    std::optional<ddk::MmioBuffer> mmio_buffer;

    if ((status = pdev_client.MapMmio(0, &mmio_buffer)) != ZX_OK) {
      zxlogf(ERROR, "aml-cpu: Failed to map mmio, st = %d", status);
      return status;
    }

    cpu_version_packed = mmio_buffer->Read32(kCpuVersionOffset);
  }

  // Create an AmlCpu for each power domain with nonempty operating points.
  for (size_t i = 0; i < MAX_DVFS_DOMAINS; i++) {
    const fuchsia_thermal::OperatingPoint& opps = info->opps[i];

    // If this domain is empty, don't create a driver.
    if (opps.count == 0) {
      continue;
    }

    if (opps.count > MAX_DEVICE_PERFORMANCE_STATES) {
      zxlogf(ERROR, "aml-cpu: cpu power domain %zu has more operating points than we support\n", i);
      return ZX_ERR_INTERNAL;
    }

    const uint8_t perf_state_count = static_cast<uint8_t>(opps.count);
    zxlogf(INFO, "aml-cpu: Creating CPU Device for domain %zu with %u operating points\n", i,
           opps.count);

    // If the FIDL client has been previously consumed, create a new one. Then build the CPU device
    // and consume the FIDL client.
    if (!thermal_fidl_client) {
      thermal_fidl_client = CreateFidlClient(thermal_protocol_client, &status);
      if (!thermal_fidl_client) {
        return status;
      }
    }
    auto cpu_device = std::make_unique<AmlCpu>(thermal_device, std::move(*thermal_fidl_client), i);
    thermal_fidl_client.reset();

    cpu_device->SetCpuInfo(cpu_version_packed);

    status = cpu_device->DdkAdd(ddk::DeviceAddArgs("cpu")
                                    .set_flags(DEVICE_ADD_NON_BINDABLE)
                                    .set_proto_id(ZX_PROTOCOL_CPU_CTRL)
                                    .set_performance_states({perf_states, perf_state_count})
                                    .set_inspect_vmo(cpu_device->inspector_.DuplicateVmo()));

    if (status != ZX_OK) {
      zxlogf(ERROR, "aml-cpu: Failed to add cpu device for domain %zu, st = %d\n", i, status);
      return status;
    }

    // Intentionally leak this device because it's owned by the driver framework.
    __UNUSED auto unused = cpu_device.release();
  }

  return ZX_OK;
}

zx_status_t AmlCpu::DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
  DdkTransaction transaction(txn);
  fuchsia_cpuctrl::Device::Dispatch(this, msg, &transaction);
  return transaction.Status();
}

void AmlCpu::DdkRelease() { delete this; }

zx_status_t AmlCpu::DdkSetPerformanceState(uint32_t requested_state, uint32_t* out_state) {
  zx_status_t status;
  fuchsia_thermal::OperatingPoint opps;

  status = GetThermalOperatingPoints(&opps);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Failed to get Thermal operating points, st = %d", __func__, status);
    return status;
  }

  if (requested_state >= opps.count) {
    zxlogf(ERROR, "%s: Requested device performance state is out of bounds", __func__);
    return ZX_ERR_OUT_OF_RANGE;
  }

  const uint16_t pstate = PstateToOperatingPoint(requested_state, opps.count);

  const auto result =
      thermal_client_.SetDvfsOperatingPoint(pstate, static_cast<PowerDomain>(power_domain_index_));

  if (!result.ok() || result->status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to set dvfs operating point.", __func__);
    return ZX_ERR_INTERNAL;
  }

  *out_state = requested_state;
  return ZX_OK;
}

zx_status_t AmlCpu::DdkConfigureAutoSuspend(bool enable, uint8_t requested_sleep_state) {
  return ZX_ERR_NOT_SUPPORTED;
}

void AmlCpu::GetPerformanceStateInfo(uint32_t state,
                                     GetPerformanceStateInfoCompleter::Sync completer) {
  // Get all performance states.
  zx_status_t status;
  fuchsia_thermal::OperatingPoint opps;

  status = GetThermalOperatingPoints(&opps);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Failed to get Thermal operating points, st = %d", __func__, status);
    completer.ReplyError(status);
  }

  // Make sure that the state is in bounds?
  if (state >= opps.count) {
    zxlogf(ERROR, "%s: requested pstate index out of bounds, requested = %u, count = %u", __func__,
           state, opps.count);
    completer.ReplyError(ZX_ERR_OUT_OF_RANGE);
    return;
  }

  const uint16_t pstate = PstateToOperatingPoint(state, opps.count);

  llcpp::fuchsia::hardware::cpu::ctrl::CpuPerformanceStateInfo result;
  result.frequency_hz = opps.opp[pstate].freq_hz;
  result.voltage_uv = opps.opp[pstate].volt_uv;
  completer.ReplySuccess(result);
}

zx_status_t AmlCpu::GetThermalOperatingPoints(fuchsia_thermal::OperatingPoint* out) {
  auto result = thermal_client_.GetDeviceInfo();
  if (!result.ok() || result->status != ZX_OK) {
    zxlogf(ERROR, "%s: Failed to get thermal device info", __func__);
    return ZX_ERR_INTERNAL;
  }

  fuchsia_thermal::ThermalDeviceInfo* info = result->info.get();

  memcpy(out, &info->opps[power_domain_index_], sizeof(*out));
  return ZX_OK;
}

void AmlCpu::GetNumLogicalCores(GetNumLogicalCoresCompleter::Sync completer) {
  unsigned int result = zx_system_get_num_cpus();
  completer.Reply(result);
}

void AmlCpu::GetLogicalCoreId(uint64_t index, GetLogicalCoreIdCompleter::Sync completer) {
  // Placeholder.
  completer.Reply(0);
}

void AmlCpu::SetCpuInfo(uint32_t cpu_version_packed) {
  const uint8_t major_revision = (cpu_version_packed >> 24) & 0xff;
  const uint8_t minor_revision = (cpu_version_packed >> 8) & 0xff;
  const uint8_t cpu_package_id = (cpu_version_packed >> 20) & 0x0f;
  zxlogf(INFO, "major revision number: 0x%x", major_revision);
  zxlogf(INFO, "minor revision number: 0x%x", minor_revision);
  zxlogf(INFO, "cpu package id number: 0x%x", cpu_package_id);

  cpu_info_.CreateUint("cpu_major_revision", major_revision, &inspector_);
  cpu_info_.CreateUint("cpu_minor_revision", minor_revision, &inspector_);
  cpu_info_.CreateUint("cpu_package_id", cpu_package_id, &inspector_);
}

}  // namespace amlogic_cpu

static constexpr zx_driver_ops_t aml_cpu_driver_ops = []() {
  zx_driver_ops_t result = {};
  result.version = DRIVER_OPS_VERSION;
  result.bind = amlogic_cpu::AmlCpu::Create;
  return result;
}();

// clang-format off
ZIRCON_DRIVER_BEGIN(aml_cpu, aml_cpu_driver_ops, "zircon", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_COMPOSITE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GOOGLE),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_DID, PDEV_DID_GOOGLE_AMLOGIC_CPU),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_SHERLOCK),
ZIRCON_DRIVER_END(aml_cpu)
