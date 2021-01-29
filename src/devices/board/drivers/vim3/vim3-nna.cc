// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/platform/bus/c/banjo.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/platform-defs.h>
#include <soc/aml-a311d/a311d-hw.h>
#include <soc/aml-common/aml-registers.h>

#include "vim3.h"

namespace vim3 {

static pbus_mmio_t vim3_nna_mmios[] = {
    {
        .base = A311D_NNA_BASE,
        .length = A311D_NNA_LENGTH,
    },
    // HIU for clocks.
    {
        .base = A311D_HIU_BASE,
        .length = A311D_HIU_LENGTH,
    },
    // Power domain
    {
        .base = A311D_POWER_DOMAIN_BASE,
        .length = A311D_POWER_DOMAIN_LENGTH,
    },
    // Memory PD
    {
        .base = A311D_MEMORY_PD_BASE,
        .length = A311D_MEMORY_PD_LENGTH,
    },
    // AXI SRAM
    {
        .base = A311D_NNA_SRAM_BASE,
        .length = A311D_NNA_SRAM_LENGTH,
    },
};

static pbus_bti_t nna_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = BTI_NNA,
    },
};

static pbus_irq_t nna_irqs[] = {
    {
        .irq = A311D_NNA_IRQ,
        .mode = ZX_INTERRUPT_MODE_LEVEL_HIGH,
    },
};

static uint64_t s_external_sram_phys_base = A311D_NNA_SRAM_BASE;

static pbus_metadata_t nna_metadata[] = {
    {
        .type = 0,
        .data_buffer = reinterpret_cast<const uint8_t*>(&s_external_sram_phys_base),
        .data_size = sizeof(s_external_sram_phys_base),
    },
};

static pbus_dev_t nna_dev = []() {
  pbus_dev_t dev = {};
  dev.name = "aml-nna";
  dev.vid = PDEV_VID_AMLOGIC;
  dev.pid = PDEV_PID_AMLOGIC_A311D;
  dev.did = PDEV_DID_AMLOGIC_NNA;
  dev.mmio_list = vim3_nna_mmios;
  dev.mmio_count = countof(vim3_nna_mmios);
  dev.bti_list = nna_btis;
  dev.bti_count = countof(nna_btis);
  dev.irq_list = nna_irqs;
  dev.irq_count = countof(nna_irqs);
  dev.metadata_list = nna_metadata;
  dev.metadata_count = countof(nna_metadata);
  return dev;
}();

static const zx_bind_inst_t root_match[] = {
    BI_MATCH(),
};

static const zx_bind_inst_t reset_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_REGISTERS),
    BI_MATCH_IF(EQ, BIND_REGISTER_ID, aml_registers::REGISTER_NNA_RESET_LEVEL2),
};

static const device_fragment_part_t reset_fragment[] = {
    {countof(root_match), root_match},
    {countof(reset_match), reset_match},
};

static const device_fragment_t fragments[] = {
    {"register-reset", countof(reset_fragment), reset_fragment},
};

zx_status_t Vim3::NnaInit() {
  zx_status_t status =
      pbus_.CompositeDeviceAdd(&nna_dev, fragments, countof(fragments), UINT32_MAX);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Vim3::NnaInit: pbus_device_add() failed for nna: %d", status);
    return status;
  }

  return ZX_OK;
}

}  // namespace vim3
