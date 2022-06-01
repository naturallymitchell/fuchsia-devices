// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <zircon/compiler.h>

#include <soc/aml-s905d3/s905d3-hw.h>

#include "nelson.h"
#include "src/devices/board/drivers/nelson/nelson_backlight_bind.h"
#include "src/ui/backlight/drivers/ti-lp8556/ti-lp8556Metadata.h"

namespace nelson {

constexpr pbus_mmio_t backlight_mmios[] = {
    {
        .base = S905D3_GPIO_AO_BASE,
        .length = S905D3_GPIO_AO_LENGTH,
    },
};

constexpr double kMaxBrightnessInNits = 250.0;

zx_status_t Nelson::BacklightInit() {
  TiLp8556Metadata kDeviceMetadata = {
      .panel_id = uint8_t(GetDisplayId()),
      .allow_set_current_scale = false,
      .registers =
          {
              // Registers
              0x01, 0x85,  // Device Control
                           // EPROM
              0xa2, 0x30,  // CFG2
              0xa3, 0x32,  // CFG3
              0xa5, 0x54,  // CFG5
              0xa7, 0xf4,  // CFG7
              0xa9, 0x60,  // CFG9
              0xae, 0x09,  // CFGE
          },
      .register_count = 14,
  };

  pbus_metadata_t backlight_metadata[] = {
      {
          .type = DEVICE_METADATA_BACKLIGHT_MAX_BRIGHTNESS_NITS,
          .data_buffer = reinterpret_cast<const uint8_t*>(&kMaxBrightnessInNits),
          .data_size = sizeof(kMaxBrightnessInNits),
      },
      {
          .type = DEVICE_METADATA_PRIVATE,
          .data_buffer = reinterpret_cast<const uint8_t*>(&kDeviceMetadata),
          .data_size = sizeof(kDeviceMetadata),
      },
  };

  pbus_dev_t backlight_dev = {
      .name = "backlight",
      .vid = PDEV_VID_TI,
      .pid = PDEV_PID_TI_LP8556,
      .did = PDEV_DID_TI_BACKLIGHT,
      .mmio_list = backlight_mmios,
      .mmio_count = std::size(backlight_mmios),
      .metadata_list = backlight_metadata,
      .metadata_count = std::size(backlight_metadata),
  };

  auto status = pbus_.AddComposite(&backlight_dev, reinterpret_cast<uint64_t>(backlight_fragments),
                                   std::size(backlight_fragments), "i2c");
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s CompositeDeviceAdd failed %d", __FUNCTION__, status);
  }
  return status;
}  // namespace nelson

}  // namespace nelson
