// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/pci/cpp/banjo.h>
#include <zircon/errors.h>
#include <zircon/hw/pci.h>
#include <zircon/syscalls/object.h>
#include <zircon/syscalls/pci.h>

#include <zxtest/zxtest.h>

#include "src/devices/pci/testing/pci_protocol_fake.h"

class FakePciProtocolTests : public zxtest::Test {
 protected:
  void SetUp() final {
    fake_pci_.reset();
    pci_ = ddk::PciProtocolClient(&fake_pci_.get_protocol());
  }
  pci::FakePciProtocol& fake_pci() { return fake_pci_; }
  ddk::PciProtocolClient& pci() { return pci_; }

 private:
  pci::FakePciProtocol fake_pci_;
  ddk::PciProtocolClient pci_;
};

TEST_F(FakePciProtocolTests, ResetDevice) {
  uint32_t reset_cnt = 0;
  ASSERT_EQ(reset_cnt++, fake_pci().GetResetCount());
  ASSERT_OK(pci().ResetDevice());
  ASSERT_EQ(reset_cnt++, fake_pci().GetResetCount());
  ASSERT_OK(pci().ResetDevice());
  ASSERT_EQ(reset_cnt++, fake_pci().GetResetCount());
}

TEST_F(FakePciProtocolTests, GetBti) {
  zx::bti bti{};

  ASSERT_OK(pci().GetBti(0, &bti));
  zx_info_bti_t info;
  // Verify it's a BTI at least.
  ASSERT_OK(bti.get_info(ZX_INFO_BTI, &info, sizeof(info), /*actual_count=*/nullptr,
                         /*avail_count=*/nullptr));
}

TEST_F(FakePciProtocolTests, EnableBusMaster) {
  // If enable has never been called there should be no value.
  ASSERT_FALSE(fake_pci().GetBusMasterEnabled().has_value());

  ASSERT_OK(pci().EnableBusMaster(true));
  ASSERT_TRUE(fake_pci().GetBusMasterEnabled().value());

  ASSERT_OK(pci().EnableBusMaster(false));
  ASSERT_FALSE(fake_pci().GetBusMasterEnabled().value());
}

TEST_F(FakePciProtocolTests, GetDeviceInfo) {
  zx_pcie_device_info_t actual{};
  zx_pcie_device_info_t zeroed{};
  ASSERT_OK(pci().GetDeviceInfo(&actual));
  ASSERT_EQ(0, memcmp(&zeroed, &actual, sizeof(zeroed)));

  zx_pcie_device_info_t expected = {
      .vendor_id = 0x1,
      .device_id = 0x2,

      .base_class = 0x3,
      .sub_class = 0x4,
      .program_interface = 0x5,
      .revision_id = 0x6,

      .bus_id = 0x7,
      .dev_id = 0x8,
      .func_id = 0x9,
  };
  fake_pci().SetDeviceInfo(expected);
  ASSERT_OK(pci().GetDeviceInfo(&actual));
  ASSERT_EQ(0, memcmp(&expected, &actual, sizeof(expected)));

  // Did we update the config header to match the device structure?
  uint8_t val8;
  uint16_t val16;
  ASSERT_OK(pci().ConfigRead16(PCI_CFG_VENDOR_ID, &val16));
  ASSERT_EQ(expected.vendor_id, val16);
  ASSERT_OK(pci().ConfigRead16(PCI_CFG_DEVICE_ID, &val16));
  ASSERT_EQ(expected.device_id, val16);
  ASSERT_OK(pci().ConfigRead8(PCI_CFG_REVISION_ID, &val8));
  ASSERT_EQ(expected.revision_id, val8);
  ASSERT_OK(pci().ConfigRead8(PCI_CFG_CLASS_CODE_BASE, &val8));
  ASSERT_EQ(expected.base_class, val8);
  ASSERT_OK(pci().ConfigRead8(PCI_CFG_CLASS_CODE_SUB, &val8));
  ASSERT_EQ(expected.sub_class, val8);
  ASSERT_OK(pci().ConfigRead8(PCI_CFG_CLASS_CODE_INTR, &val8));
  ASSERT_EQ(expected.program_interface, val8);
}

TEST_F(FakePciProtocolTests, QueryIrqMode) {
  uint32_t irq_cnt = 0;
  ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, pci().QueryIrqMode(PCI_IRQ_MODE_LEGACY, &irq_cnt));
  ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, pci().QueryIrqMode(PCI_IRQ_MODE_MSI, &irq_cnt));
  ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, pci().QueryIrqMode(PCI_IRQ_MODE_MSI_X, &irq_cnt));

  fake_pci().AddLegacyInterrupt();
  ASSERT_EQ(ZX_OK, pci().QueryIrqMode(PCI_IRQ_MODE_LEGACY, &irq_cnt));
  ASSERT_EQ(1, irq_cnt);

  // MSI supports interrupt configuration via powers of two, so ensure that we
  // round down if not enough have been added.
  fake_pci().AddMsiInterrupt();
  ASSERT_EQ(ZX_OK, pci().QueryIrqMode(PCI_IRQ_MODE_MSI, &irq_cnt));
  ASSERT_EQ(1, irq_cnt);
  fake_pci().AddMsiInterrupt();
  ASSERT_EQ(ZX_OK, pci().QueryIrqMode(PCI_IRQ_MODE_MSI, &irq_cnt));
  ASSERT_EQ(2, irq_cnt);
  fake_pci().AddMsiInterrupt();
  ASSERT_EQ(ZX_OK, pci().QueryIrqMode(PCI_IRQ_MODE_MSI, &irq_cnt));
  ASSERT_EQ(2, irq_cnt);
  fake_pci().AddMsiInterrupt();
  ASSERT_EQ(ZX_OK, pci().QueryIrqMode(PCI_IRQ_MODE_MSI, &irq_cnt));
  ASSERT_EQ(4, irq_cnt);

  // MSI-X doesn't care about alignment, so any value should work.
  fake_pci().AddMsixInterrupt();
  ASSERT_EQ(ZX_OK, pci().QueryIrqMode(PCI_IRQ_MODE_MSI_X, &irq_cnt));
  ASSERT_EQ(1, irq_cnt);
  fake_pci().AddMsixInterrupt();
  ASSERT_EQ(ZX_OK, pci().QueryIrqMode(PCI_IRQ_MODE_MSI_X, &irq_cnt));
  ASSERT_EQ(2, irq_cnt);
  fake_pci().AddMsixInterrupt();
  ASSERT_EQ(ZX_OK, pci().QueryIrqMode(PCI_IRQ_MODE_MSI_X, &irq_cnt));
  ASSERT_EQ(3, irq_cnt);
}

TEST_F(FakePciProtocolTests, SetIrqMode) {
  fake_pci().AddLegacyInterrupt();
  fake_pci().AddMsiInterrupt();
  fake_pci().AddMsiInterrupt();
  fake_pci().AddMsiInterrupt();
  fake_pci().AddMsiInterrupt();
  fake_pci().AddMsixInterrupt();
  fake_pci().AddMsixInterrupt();

  pci_irq_mode_t mode = PCI_IRQ_MODE_LEGACY;
  ASSERT_OK(pci().SetIrqMode(mode, 1));
  ASSERT_EQ(1, fake_pci().GetIrqCount());
  ASSERT_EQ(mode, fake_pci().GetIrqMode());
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, pci().SetIrqMode(mode, 2));

  mode = PCI_IRQ_MODE_MSI;
  ASSERT_OK(pci().SetIrqMode(mode, 1));
  ASSERT_EQ(1, fake_pci().GetIrqCount());
  ASSERT_EQ(mode, fake_pci().GetIrqMode());

  ASSERT_OK(pci().SetIrqMode(mode, 2));
  ASSERT_EQ(2, fake_pci().GetIrqCount());
  ASSERT_EQ(mode, fake_pci().GetIrqMode());

  ASSERT_EQ(ZX_ERR_INVALID_ARGS, pci().SetIrqMode(mode, 3));
  ASSERT_EQ(2, fake_pci().GetIrqCount());
  ASSERT_EQ(mode, fake_pci().GetIrqMode());

  ASSERT_OK(pci().SetIrqMode(mode, 4));
  ASSERT_EQ(4, fake_pci().GetIrqCount());
  ASSERT_EQ(mode, fake_pci().GetIrqMode());
}

TEST_F(FakePciProtocolTests, ConfigureIrqMode) {
  // The intent is to check that the IRQ modes are always favored in order of
  // MSI-X > MSI > Legacy, but also choosing based on how many interrupts each
  // mode is configured to provide.
  fake_pci().AddLegacyInterrupt();
  ASSERT_OK(pci().ConfigureIrqMode(1));
  ASSERT_EQ(1, fake_pci().GetIrqCount());
  ASSERT_EQ(PCI_IRQ_MODE_LEGACY, fake_pci().GetIrqMode());

  fake_pci().AddMsiInterrupt();
  ASSERT_OK(pci().ConfigureIrqMode(1));
  ASSERT_EQ(1, fake_pci().GetIrqCount());
  ASSERT_EQ(PCI_IRQ_MODE_MSI, fake_pci().GetIrqMode());

  fake_pci().AddMsixInterrupt();
  ASSERT_OK(pci().ConfigureIrqMode(1));
  ASSERT_EQ(1, fake_pci().GetIrqCount());
  ASSERT_EQ(PCI_IRQ_MODE_MSI_X, fake_pci().GetIrqMode());

  // Ensure it will find the mode that supports the number necessary.
  fake_pci().AddMsiInterrupt();
  ASSERT_OK(pci().ConfigureIrqMode(2));
  ASSERT_EQ(2, fake_pci().GetIrqCount());
  ASSERT_EQ(PCI_IRQ_MODE_MSI, fake_pci().GetIrqMode());

  fake_pci().AddMsixInterrupt();
  ASSERT_OK(pci().ConfigureIrqMode(2));
  ASSERT_EQ(2, fake_pci().GetIrqCount());
  ASSERT_EQ(PCI_IRQ_MODE_MSI_X, fake_pci().GetIrqMode());
}

namespace {
// When interrupts are added to the fake a borrowed copy of the interrupt is
// returned for comparison by tests later. Its koid should match the koid of the
// duplicated handle returned by MapInterrupt.
template <typename T>
bool MatchKoids(const zx::unowned<T>& first, const zx::object<T>& second) {
  zx_info_handle_basic finfo{}, sinfo{};
  ZX_ASSERT(first->get_info(ZX_INFO_HANDLE_BASIC, &finfo, sizeof(finfo), nullptr, nullptr) ==
            ZX_OK);
  ZX_ASSERT(second.get_info(ZX_INFO_HANDLE_BASIC, &sinfo, sizeof(sinfo), nullptr, nullptr) ==
            ZX_OK);

  return finfo.koid == sinfo.koid;
}
}  // namespace

TEST_F(FakePciProtocolTests, MapInterrupt) {
  // One notable difference between this fake and the real PCI protocol is that
  // it is an error to call SetIrqMode and switch modes if an existing MSI is
  // mapped still. In the fake though, it's fine to do so. Switching IRQ modes
  // is not something drivers do in practice, so it's fine if they encounter
  // ZX_ERR_BAD_STATE at runtime if documentation details it.
  zx::unowned_interrupt legacy = fake_pci().AddLegacyInterrupt();
  zx::unowned_interrupt msi0 = fake_pci().AddMsiInterrupt();
  zx::unowned_interrupt msi1 = fake_pci().AddMsiInterrupt();
  zx::unowned_interrupt msix0 = fake_pci().AddMsixInterrupt();
  zx::unowned_interrupt msix1 = fake_pci().AddMsixInterrupt();
  zx::unowned_interrupt msix2 = fake_pci().AddMsixInterrupt();

  zx::interrupt interrupt{};
  uint32_t irq_cnt = 1;
  ASSERT_OK(pci().SetIrqMode(PCI_IRQ_MODE_LEGACY, irq_cnt));
  ASSERT_OK(pci().MapInterrupt(0, &interrupt));
  ASSERT_TRUE(MatchKoids(legacy, interrupt));
  ASSERT_FALSE(MatchKoids(msi0, interrupt));
  ASSERT_FALSE(MatchKoids(msi1, interrupt));
  ASSERT_FALSE(MatchKoids(msix0, interrupt));
  ASSERT_FALSE(MatchKoids(msix1, interrupt));
  ASSERT_FALSE(MatchKoids(msix2, interrupt));
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, pci().MapInterrupt(irq_cnt, &interrupt));

  irq_cnt = 2;
  ASSERT_OK(pci().SetIrqMode(PCI_IRQ_MODE_MSI, irq_cnt));
  ASSERT_OK(pci().MapInterrupt(0, &interrupt));
  ASSERT_FALSE(MatchKoids(legacy, interrupt));
  ASSERT_TRUE(MatchKoids(msi0, interrupt));
  ASSERT_FALSE(MatchKoids(msi1, interrupt));
  ASSERT_FALSE(MatchKoids(msix0, interrupt));
  ASSERT_FALSE(MatchKoids(msix1, interrupt));
  ASSERT_FALSE(MatchKoids(msix2, interrupt));
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, pci().MapInterrupt(irq_cnt, &interrupt));

  ASSERT_OK(pci().MapInterrupt(1, &interrupt));
  ASSERT_FALSE(MatchKoids(legacy, interrupt));
  ASSERT_FALSE(MatchKoids(msi0, interrupt));
  ASSERT_TRUE(MatchKoids(msi1, interrupt));
  ASSERT_FALSE(MatchKoids(msix0, interrupt));
  ASSERT_FALSE(MatchKoids(msix1, interrupt));
  ASSERT_FALSE(MatchKoids(msix2, interrupt));

  irq_cnt = 3;
  ASSERT_OK(pci().SetIrqMode(PCI_IRQ_MODE_MSI_X, irq_cnt));
  ASSERT_OK(pci().MapInterrupt(0, &interrupt));
  ASSERT_FALSE(MatchKoids(legacy, interrupt));
  ASSERT_FALSE(MatchKoids(msi0, interrupt));
  ASSERT_FALSE(MatchKoids(msi1, interrupt));
  ASSERT_TRUE(MatchKoids(msix0, interrupt));
  ASSERT_FALSE(MatchKoids(msix1, interrupt));
  ASSERT_FALSE(MatchKoids(msix2, interrupt));

  ASSERT_OK(pci().MapInterrupt(1, &interrupt));
  ASSERT_FALSE(MatchKoids(legacy, interrupt));
  ASSERT_FALSE(MatchKoids(msi0, interrupt));
  ASSERT_FALSE(MatchKoids(msi1, interrupt));
  ASSERT_FALSE(MatchKoids(msix0, interrupt));
  ASSERT_TRUE(MatchKoids(msix1, interrupt));
  ASSERT_FALSE(MatchKoids(msix2, interrupt));

  ASSERT_OK(pci().MapInterrupt(2, &interrupt));
  ASSERT_FALSE(MatchKoids(legacy, interrupt));
  ASSERT_FALSE(MatchKoids(msi0, interrupt));
  ASSERT_FALSE(MatchKoids(msi1, interrupt));
  ASSERT_FALSE(MatchKoids(msix0, interrupt));
  ASSERT_FALSE(MatchKoids(msix1, interrupt));
  ASSERT_TRUE(MatchKoids(msix2, interrupt));
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, pci().MapInterrupt(irq_cnt, &interrupt));
}

TEST_F(FakePciProtocolTests, ConfigRW) {
  auto config = fake_pci().GetConfigVmo();

  // Verify the header space range. Reads can read the header [0, 63], but
  // writes cannot. All IO must fit within the config space [0, 255].
  uint8_t val8;
  ASSERT_DEATH([&]() { pci().ConfigWrite8(0, 0xFF); });
  ASSERT_NO_DEATH([&]() { pci().ConfigRead8(0, &val8); });
  ASSERT_DEATH([&]() { pci().ConfigWrite8(PCI_CFG_HEADER_SIZE - 1, 0xFF); });
  ASSERT_NO_DEATH([&]() { pci().ConfigRead8(PCI_CFG_HEADER_SIZE - 1, &val8); });
  // The ensures we also verify that offset + read/write size is within bounds.
  uint32_t val32;
  ASSERT_DEATH([&]() { pci().ConfigWrite32(PCI_BASE_CONFIG_SIZE - 2, 0xFF); });
  ASSERT_DEATH([&]() { pci().ConfigRead32(PCI_BASE_CONFIG_SIZE - 2, &val32); });

  for (uint16_t off = PCI_CFG_HEADER_SIZE; off < PCI_BASE_CONFIG_SIZE; off++) {
    uint8_t val8;
    pci().ConfigWrite8(off, off);
    pci().ConfigRead8(off, &val8);
    ASSERT_EQ(off, val8);
    ASSERT_OK(config->read(&val8, off, sizeof(val8)));
    ASSERT_EQ(off, val8);
  }

  for (uint16_t off = PCI_CFG_HEADER_SIZE; off < PCI_BASE_CONFIG_SIZE - 1; off++) {
    uint16_t val16;
    pci().ConfigWrite16(off, off);
    pci().ConfigRead16(off, &val16);
    ASSERT_EQ(off, val16);
    ASSERT_OK(config->read(&val16, off, sizeof(val16)));
    ASSERT_EQ(off, val16);
  }

  for (uint16_t off = PCI_CFG_HEADER_SIZE; off < PCI_BASE_CONFIG_SIZE - 3; off++) {
    uint32_t val32;
    pci().ConfigWrite32(off, off);
    pci().ConfigRead32(off, &val32);
    ASSERT_EQ(off, val32);
    ASSERT_OK(config->read(&val32, off, sizeof(val32)));
    ASSERT_EQ(off, val32);
  }
}
