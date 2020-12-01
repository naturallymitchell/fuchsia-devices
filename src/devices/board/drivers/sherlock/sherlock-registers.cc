// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <soc/aml-common/aml-registers.h>

#include "sherlock.h"
#include "src/devices/lib/metadata/llcpp/registers.h"

namespace sherlock {

namespace {

enum MmioMetadataIdx {
  RESET_MMIO,
#ifdef FACTORY_BUILD
  USB_FACTORY_MMIO,
#endif  // FACTORY_BUILD

  MMIO_COUNT,
};

}  // namespace

zx_status_t Sherlock::RegistersInit() {
  const pbus_mmio_t registers_mmios[] = {
      {
          .base = T931_RESET_BASE,
          .length = T931_RESET_LENGTH,
      },
#ifdef FACTORY_BUILD
      {
          .base = T931_USB_BASE,
          .length = T931_USB_LENGTH,
      },
#endif  // FACTORY_BUILD
  };

  fidl::BufferThenHeapAllocator<2048> allocator;
  fidl::VectorView<registers::MmioMetadataEntry> mmio_entries;
  mmio_entries.set_data(allocator.make<registers::MmioMetadataEntry[]>(MMIO_COUNT));
  mmio_entries.set_count(MMIO_COUNT);

  mmio_entries[RESET_MMIO] = registers::BuildMetadata(allocator, RESET_MMIO);

  fidl::VectorView<registers::RegistersMetadataEntry> register_entries;
  register_entries.set_data(
      allocator.make<registers::RegistersMetadataEntry[]>(aml_registers::REGISTER_ID_COUNT));
  register_entries.set_count(aml_registers::REGISTER_ID_COUNT);

  register_entries[aml_registers::REGISTER_USB_PHY_V2_RESET] =
      registers::BuildMetadata(allocator, aml_registers::REGISTER_USB_PHY_V2_RESET, RESET_MMIO,
                               std::vector<registers::MaskEntryBuilder<uint32_t>>{
                                   {
                                       .mask = aml_registers::USB_RESET1_REGISTER_UNKNOWN_1_MASK |
                                               aml_registers::USB_RESET1_REGISTER_UNKNOWN_2_MASK,
                                       .mmio_offset = T931_RESET1_REGISTER,
                                       .reg_count = 1,
                                   },
                                   {
                                       .mask = aml_registers::USB_RESET1_LEVEL_MASK,
                                       .mmio_offset = T931_RESET1_LEVEL,
                                       .reg_count = 1,
                                   },
                               });

  register_entries[aml_registers::REGISTER_NNA_RESET_LEVEL2] =
      registers::BuildMetadata(allocator, aml_registers::REGISTER_NNA_RESET_LEVEL2, RESET_MMIO,
                               std::vector<registers::MaskEntryBuilder<uint32_t>>{
                                   {
                                       .mask = aml_registers::NNA_RESET2_LEVEL_MASK,
                                       .mmio_offset = T931_RESET2_LEVEL,
                                       .reg_count = 1,
                                   },
                               });
#ifdef FACTORY_BUILD
  mmio_entries[USB_FACTORY_MMIO] = registers::BuildMetadata(allocator, USB_FACTORY_MMIO);

  register_entries[aml_registers::REGISTER_USB_PHY_FACTORY] =
      registers::BuildMetadata(allocator, aml_registers::REGISTER_USB_PHY_FACTORY, USB_FACTORY_MMIO,
                               std::vector<registers::MaskEntryBuilder<uint32_t>>{
                                   {
                                       .mask = 0xFFFFFFFF,
                                       .mmio_offset = 0,
                                       .reg_count = T931_USB_LENGTH / sizeof(uint32_t),
                                       .overlap_check_on = false,
                                   },
                               });
#endif  // FACTORY_BUILD

  auto metadata =
      registers::BuildMetadata(allocator, std::move(mmio_entries), std::move(register_entries));
  fidl::OwnedEncodedMessage<registers::Metadata> encoded_metadata(&metadata);
  if (!encoded_metadata.ok() || (encoded_metadata.error() != nullptr)) {
    zxlogf(ERROR, "%s: Could not build metadata %s\n", __func__, encoded_metadata.error());
    return encoded_metadata.status();
  }

  const pbus_metadata_t registers_metadata[] = {
      {
          .type = DEVICE_METADATA_REGISTERS,
          .data_buffer = encoded_metadata.GetOutgoingMessage().bytes(),
          .data_size = encoded_metadata.GetOutgoingMessage().byte_actual(),
      },
  };

  pbus_dev_t registers_dev{
      .name = "registers",
      .vid = PDEV_VID_GENERIC,
      .pid = PDEV_PID_GENERIC,
      .did = PDEV_DID_REGISTERS,
      .mmio_list = registers_mmios,
      .mmio_count = countof(registers_mmios),
      .metadata_list = registers_metadata,
      .metadata_count = countof(registers_metadata),
  };

  zx_status_t status = pbus_.DeviceAdd(&registers_dev);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: DeviceAdd failed %d", __func__, status);
    return status;
  }

  return ZX_OK;
}

}  // namespace sherlock
