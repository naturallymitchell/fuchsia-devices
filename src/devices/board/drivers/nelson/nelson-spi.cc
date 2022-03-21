// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/time.h>

#include <fbl/algorithm.h>
#include <soc/aml-common/aml-registers.h>
#include <soc/aml-common/aml-spi.h>
#include <soc/aml-s905d2/s905d2-gpio.h>
#include <soc/aml-s905d3/s905d3-hw.h>

#include "nelson-gpios.h"
#include "nelson.h"
#include "src/devices/board/drivers/nelson/spi_1_bind.h"
#include "src/devices/lib/fidl-metadata/spi.h"

#define HHI_SPICC_CLK_CNTL (0xf7 * 4)
#define spicc1_clk_sel_fclk_div2 (4 << 23)
#define spicc1_clk_en (1 << 22)
#define spicc1_clk_div(x) (((x)-1) << 16)

namespace nelson {
using spi_channel_t = fidl_metadata::spi::Channel;

// Approximate CPU time required to read a single burst, determined with tracing.
constexpr zx::duration kSelinaCapacity = zx::usec(3'500);
// The radar sensor interrupts the host with a new burst every 33,333 us.
constexpr zx::duration kSelinaPeriod = zx::usec(33'333);

static const pbus_mmio_t spi_mmios[] = {
    {
        .base = S905D3_SPICC1_BASE,
        .length = S905D3_SPICC1_LENGTH,
    },
};

static const pbus_irq_t spi_irqs[] = {
    {
        .irq = S905D3_SPICC1_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
};

static const spi_channel_t spi_channels[] = {
    // Radar sensor head.
    {
        .bus_id = NELSON_SPICC1,
        .cs = 0,  // index into matching chip-select map
        .vid = PDEV_VID_INFINEON,
        .pid = PDEV_PID_INFINEON_BGT60TR13C,
        .did = PDEV_DID_RADAR_SENSOR,
    },
};

static const amlspi_config_t spi_config = {
    .capacity = kSelinaCapacity.to_nsecs(),
    .period = kSelinaPeriod.to_nsecs(),
    .bus_id = NELSON_SPICC1,
    .cs_count = 1,
    .cs = {0},                          // index into fragments list
    .clock_divider_register_value = 0,  // SCLK = core clock / 4 = 25 MHz
    .use_enhanced_clock_mode = false,
};

static pbus_dev_t spi_dev = []() {
  pbus_dev_t dev = {};
  dev.name = "spi-1";
  dev.vid = PDEV_VID_AMLOGIC;
  dev.pid = PDEV_PID_GENERIC;
  dev.did = PDEV_DID_AMLOGIC_SPI;
  dev.mmio_list = spi_mmios;
  dev.mmio_count = std::size(spi_mmios);
  dev.irq_list = spi_irqs;
  dev.irq_count = std::size(spi_irqs);
  return dev;
}();

// composite binding rules

zx_status_t Nelson::SpiInit() {
  // setup pinmux for SPICC1 bus arbiter.
  gpio_impl_.SetAltFunction(GPIO_SOC_SPI_B_MOSI, 3);  // MOSI
  gpio_impl_.SetDriveStrength(GPIO_SOC_SPI_B_MOSI, 2500, nullptr);

  gpio_impl_.SetAltFunction(GPIO_SOC_SPI_B_MISO, 3);  // MISO
  gpio_impl_.SetDriveStrength(GPIO_SOC_SPI_B_MISO, 2500, nullptr);

  gpio_impl_.ConfigOut(GPIO_SOC_SPI_B_SS0, 1);  // SS0

  gpio_impl_.SetAltFunction(GPIO_SOC_SPI_B_SCLK, 3);  // SCLK
  gpio_impl_.SetDriveStrength(GPIO_SOC_SPI_B_SCLK, 2500, nullptr);

  std::vector<pbus_metadata_t> spi_metadata;
  spi_metadata.emplace_back(pbus_metadata_t{
      .type = DEVICE_METADATA_AMLSPI_CONFIG,
      .data_buffer = reinterpret_cast<const uint8_t*>(&spi_config),
      .data_size = sizeof spi_config,
  });

  auto spi_status = fidl_metadata::spi::SpiChannelsToFidl(spi_channels);
  if (spi_status.is_error()) {
    zxlogf(ERROR, "%s: failed to encode spi channels to fidl: %d", __func__,
           spi_status.error_value());
    return spi_status.error_value();
  }
  auto& data = spi_status.value();

  spi_metadata.emplace_back(pbus_metadata_t{
      .type = DEVICE_METADATA_SPI_CHANNELS,
      .data_buffer = data.data(),
      .data_size = data.size(),
  });

  spi_dev.metadata_list = spi_metadata.data();
  spi_dev.metadata_count = spi_metadata.size();

  // TODO(fxbug.dev/34010): fix this clock enable block when the clock driver can handle the
  // dividers
  {
    // Please do not use get_root_resource() in new code. See fxbug.dev/31358.
    zx::unowned_resource resource(get_root_resource());
    std::optional<fdf::MmioBuffer> buf;
    zx_status_t status = fdf::MmioBuffer::Create(S905D3_HIU_BASE, S905D3_HIU_LENGTH, *resource,
                                                 ZX_CACHE_POLICY_UNCACHED_DEVICE, &buf);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: MmioBuffer::Create failed %d", __func__, status);
      return status;
    }

    // SPICC1 clock enable @200MHz (fclk_div2(1GHz) / N(5)).  For final SCLK frequency, see
    // CONREG[16:18] in the SPI controller.  This clock config produces a SCLK frequency of 50MHz
    // assuming a default value for CONREG[16:18].
    //
    // Some timing instability was observed which may have been an individual board artifact.  To
    // debug, consider configuring the SCLK=25MHz (i.e. set spicc1_cli_div(10)).
    buf->Write32(spicc1_clk_sel_fclk_div2 | spicc1_clk_en | spicc1_clk_div(10), HHI_SPICC_CLK_CNTL);
  }

  zx_status_t status = pbus_.AddComposite(&spi_dev, reinterpret_cast<uint64_t>(spi_1_fragments),
                                          std::size(spi_1_fragments), "pdev");
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: DeviceAdd failed %d", __func__, status);
    return status;
  }

  return ZX_OK;
}

}  // namespace nelson
