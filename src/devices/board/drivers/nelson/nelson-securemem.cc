// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/platform/bus/c/banjo.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/platform-defs.h>

#include <cstdint>

#include "nelson.h"
#include "src/devices/board/drivers/nelson/aml_secure_mem_bind.h"

namespace nelson {

static const pbus_bti_t nelson_secure_mem_btis[] = {
    {
        .iommu_index = 0,
        .bti_id = BTI_AML_SECURE_MEM,
    },
};

static const pbus_dev_t secure_mem_dev = []() {
  pbus_dev_t dev = {};
  dev.name = "aml-secure-mem";
  dev.vid = PDEV_VID_AMLOGIC;
  dev.pid = PDEV_PID_AMLOGIC_S905D2;
  dev.did = PDEV_DID_AMLOGIC_SECURE_MEM;
  dev.bti_list = nelson_secure_mem_btis;
  dev.bti_count = countof(nelson_secure_mem_btis);
  return dev;
}();

zx_status_t Nelson::SecureMemInit() {
  zx_status_t status =
      pbus_.AddComposite(&secure_mem_dev, reinterpret_cast<uint64_t>(aml_secure_mem_fragments),
                         countof(aml_secure_mem_fragments), "pdev");
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: CompositeDeviceAdd failed: %d", __func__, status);
    return status;
  }
  return ZX_OK;
}

}  // namespace nelson
