// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BOARD_DRIVERS_VIM3_VIM3_H_
#define SRC_DEVICES_BOARD_DRIVERS_VIM3_VIM3_H_

#include <fuchsia/hardware/clockimpl/cpp/banjo.h>
#include <fuchsia/hardware/gpioimpl/cpp/banjo.h>
#include <fuchsia/hardware/iommu/cpp/banjo.h>
#include <fuchsia/hardware/platform/bus/cpp/banjo.h>
#include <threads.h>

#include <optional>

#include <ddk/device.h>
#include <ddktl/device.h>
#include <fbl/macros.h>
#include <soc/aml-a311d/a311d-hw.h>

namespace vim3 {

// BTI IDs for our devices
enum {
  BTI_EMMC,
  BTI_ETHERNET,
  BTI_SD,
  BTI_SDIO,
  BTI_SYSMEM,
  BTI_NNA,
  BTI_USB,
  BTI_MALI,
};

// MAC address metadata indices
enum {
  MACADDR_WIFI = 0,
  MACADDR_BLUETOOTH = 1,
};

class Vim3;
using Vim3Type = ddk::Device<Vim3, ddk::Initializable>;

// This is the main class for the platform bus driver.
class Vim3 : public Vim3Type {
 public:
  Vim3(zx_device_t* parent, pbus_protocol_t* pbus, iommu_protocol_t* iommu)
      : Vim3Type(parent), pbus_(pbus), iommu_(iommu) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  // Device protocol implementation.
  void DdkInit(ddk::InitTxn txn);
  void DdkRelease() {}

 private:
  DISALLOW_COPY_ASSIGN_AND_MOVE(Vim3);

  zx_status_t ClkInit();
  zx_status_t EmmcInit();
  zx_status_t EthInit();
  zx_status_t GpioInit();
  zx_status_t I2cInit();
  zx_status_t RegistersInit();
  zx_status_t SdInit();
  zx_status_t SdioInit();
  zx_status_t Start();
  zx_status_t SysmemInit();
  zx_status_t NnaInit();
  zx_status_t UsbInit();
  zx_status_t MaliInit();

  int Thread();

  ddk::PBusProtocolClient pbus_;
  std::optional<ddk::InitTxn> init_txn_;
  ddk::IommuProtocolClient iommu_;
  ddk::GpioImplProtocolClient gpio_impl_;
  ddk::ClockImplProtocolClient clk_impl_;
  thrd_t thread_;
};

}  // namespace vim3

#endif  // SRC_DEVICES_BOARD_DRIVERS_VIM3_VIM3_H_
