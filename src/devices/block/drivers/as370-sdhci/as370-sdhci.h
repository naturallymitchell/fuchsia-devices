// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_BLOCK_DRIVERS_AS370_SDHCI_AS370_SDHCI_H_
#define SRC_STORAGE_BLOCK_DRIVERS_AS370_SDHCI_AS370_SDHCI_H_

#include <lib/mmio/mmio.h>

#include <ddktl/device.h>
#include <ddktl/protocol/sdhci.h>

namespace sdhci {

class As370Sdhci;
using DeviceType = ddk::Device<As370Sdhci>;

class As370Sdhci : public DeviceType, public ddk::SdhciProtocol<As370Sdhci, ddk::base_protocol> {
 public:
  static zx_status_t Create(void* ctx, zx_device_t* parent);

  virtual ~As370Sdhci() = default;

  void DdkRelease() { delete this; }

  zx_status_t Init();

  zx_status_t SdhciGetInterrupt(zx::interrupt* out_irq);
  zx_status_t SdhciGetMmio(zx::vmo* out_mmio, zx_off_t* out_offset);
  zx_status_t SdhciGetBti(uint32_t index, zx::bti* out_bti);
  uint32_t SdhciGetBaseClock();
  uint64_t SdhciGetQuirks();
  void SdhciHwReset();

 private:
  As370Sdhci(zx_device_t* parent, ddk::MmioBuffer core_mmio, zx::interrupt irq)
      : DeviceType(parent), core_mmio_(std::move(core_mmio)), irq_(std::move(irq)) {}

  int IrqThread();

  ddk::MmioBuffer core_mmio_;
  zx::interrupt irq_;
};

}  // namespace sdhci

#endif  // SRC_STORAGE_BLOCK_DRIVERS_AS370_SDHCI_AS370_SDHCI_H_
