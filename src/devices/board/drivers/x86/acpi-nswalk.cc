// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/acpi/c/banjo.h>
#include <fuchsia/hardware/pciroot/c/banjo.h>
#include <fuchsia/hardware/sysmem/c/banjo.h>
#include <inttypes.h>
#include <lib/ddk/debug.h>
#include <limits.h>
#include <sys/types.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/iommu.h>
#include <zircon/syscalls/resource.h>
#include <zircon/threads.h>

#include <variant>
#include <vector>

#include <acpica/acpi.h>
#include <fbl/auto_lock.h>

#include "acpi-private.h"
#include "acpi.h"
#include "acpi/device.h"
#include "acpi/manager.h"
#include "acpi/status.h"
#include "dev.h"
#include "errors.h"
#include "methods.h"
#include "nhlt.h"
#include "pci.h"
#include "power.h"
#include "src/devices/board/drivers/x86/acpi/resources.h"
#include "src/devices/lib/iommu/iommu.h"
#include "sysmem.h"
#include "util.h"

namespace {

const std::string_view hid_from_acpi_devinfo(const ACPI_DEVICE_INFO& info) {
  if ((info.Valid & ACPI_VALID_HID) && (info.HardwareId.Length > 0) &&
      ((info.HardwareId.Length - 1) <= sizeof(uint64_t))) {
    // ACPICA string lengths include the NULL terminator.
    return std::string_view{info.HardwareId.String, info.HardwareId.Length - 1};
  }

  return std::string_view{};
}

// TODO(fxbug.dev/81684): remove these hacks once we have a proper solution for managing power of
// ACPI devices.
void acpi_apply_workarounds(acpi::Acpi* acpi, ACPI_HANDLE object, ACPI_DEVICE_INFO* info) {
  // Slate workaround: Turn on the HID controller.
  if (!memcmp(&info->Name, "I2C0", 4)) {
    auto acpi_status = acpi->EvaluateObject(object, "H00A._PR0", std::nullopt);
    if (acpi_status.is_ok()) {
      acpi::UniquePtr<ACPI_OBJECT> pkg = std::move(acpi_status.value());
      for (unsigned i = 0; i < pkg->Package.Count; i++) {
        ACPI_OBJECT* ref = &pkg->Package.Elements[i];
        if (ref->Type != ACPI_TYPE_LOCAL_REFERENCE) {
          zxlogf(DEBUG, "acpi: Ignoring wrong type 0x%x", ref->Type);
        } else {
          zxlogf(DEBUG, "acpi: Enabling HID controller at I2C0.H00A._PR0[%u]", i);
          acpi_status = acpi->EvaluateObject(ref->Reference.Handle, "_ON", std::nullopt);
          if (acpi_status.is_error()) {
            zxlogf(ERROR, "acpi: acpi error 0x%x in I2C0._PR0._ON", acpi_status.error_value());
          }
        }
      }
    }

    // Atlas workaround: Turn on the HID controller.
    acpi_status = acpi->EvaluateObject(object, "H049._PR0", std::nullopt);
    if (acpi_status.is_ok()) {
      acpi::UniquePtr<ACPI_OBJECT> pkg = std::move(acpi_status.value());
      for (unsigned i = 0; i < pkg->Package.Count; i++) {
        ACPI_OBJECT* ref = &pkg->Package.Elements[i];
        if (ref->Type != ACPI_TYPE_LOCAL_REFERENCE) {
          zxlogf(DEBUG, "acpi: Ignoring wrong type 0x%x", ref->Type);
        } else {
          zxlogf(DEBUG, "acpi: Enabling HID controller at I2C0.H049._PR0[%u]", i);
          acpi_status = acpi->EvaluateObject(ref->Reference.Handle, "_ON", std::nullopt);
          if (acpi_status.is_error()) {
            zxlogf(ERROR, "acpi: acpi error 0x%x in I2C0._PR0._ON", acpi_status.error_value());
          }
        }
      }
    }

  }
  // Acer workaround: Turn on the HID controller.
  else if (!memcmp(&info->Name, "I2C1", 4)) {
    zxlogf(DEBUG, "acpi: Enabling HID controller at I2C1");
    auto acpi_status = acpi->EvaluateObject(object, "_PS0", std::nullopt);
    if (acpi_status.is_error()) {
      zxlogf(ERROR, "acpi: acpi error in I2C1._PS0: 0x%x", acpi_status.error_value());
    }
  }
}

// A small helper class we can use to track the BBN (either "Base Bus
// Number" or "Bios Bus Number") of the last PCI bus node we encountered while
// walking the ACPI namespace.
class LastPciBbnTracker {
 public:
  explicit LastPciBbnTracker(acpi::Acpi* acpi) : acpi_(acpi) {}

  // If we are ascending through the level where we noticed a valid PCI BBN,
  // then we are no longer valid.
  void Ascend(uint32_t level) {
    if (valid_ && (level == level_)) {
      valid_ = false;
    }
  }

  zx_status_t Descend(uint32_t level, ACPI_HANDLE object, const ACPI_DEVICE_INFO& obj_info) {
    // Are we descending into a device node which has a hardware ID, and does
    // that hardware ID indicate a PCI/PCIe bus?  If so, try to extract the base
    // bus number and stash it as our last seen PCI bus number.
    const std::string_view hid = hid_from_acpi_devinfo(obj_info);
    if ((hid == PCI_EXPRESS_ROOT_HID_STRING) || (hid == PCI_ROOT_HID_STRING)) {
      auto bbn_ret = acpi_->CallBbn(object);
      uint8_t bbn;

      if (acpi_to_zx_status(bbn_ret.status_value()) == ZX_ERR_NOT_FOUND) {
        zxlogf(WARNING, "acpi: PCI/PCIe device \"%s\" missing _BBN entry, defaulting to 0",
               fourcc_to_string(obj_info.Name).str);
        bbn = 0;
      } else if (bbn_ret.is_error()) {
        zxlogf(ERROR, "acpi: failed to fetch BBN for PCI/PCIe device \"%s\"",
               fourcc_to_string(obj_info.Name).str);
        return ZX_ERR_BAD_STATE;
      } else {
        bbn = bbn_ret.value();
      }

      if (valid_) {
        zxlogf(ERROR,
               "acpi: Nested PCI roots detected when descending into PCI/PCIe device \"%s\" (prev "
               "bbn %u at level %u, child bbn %u at "
               "level %u",
               fourcc_to_string(obj_info.Name).str, bbn_, level_, bbn, level);
        return ZX_ERR_BAD_STATE;
      }

      valid_ = true;
      level_ = level;
      bbn_ = bbn;
    }

    return ZX_OK;
  }

  bool has_value() const { return valid_; }
  uint8_t bbn() const {
    ZX_DEBUG_ASSERT(valid_);
    return bbn_;
  }

 private:
  bool valid_ = false;
  uint32_t level_ = 0;
  uint8_t bbn_ = 0;
  acpi::Acpi* acpi_;
};

}  // namespace

namespace acpi {

acpi::status<UniquePtr<ACPI_DEVICE_INFO>> GetObjectInfo(ACPI_HANDLE obj) {
  ACPI_DEVICE_INFO* raw = nullptr;
  ACPI_STATUS acpi_status = AcpiGetObjectInfo(obj, &raw);
  UniquePtr<ACPI_DEVICE_INFO> ret{raw};

  if (ACPI_SUCCESS(acpi_status)) {
    return acpi::ok(std::move(ret));
  }

  return acpi::error(acpi_status);
}

}  // namespace acpi

zx_status_t acpi_suspend(uint8_t requested_state, bool enable_wake, uint8_t suspend_reason,
                         uint8_t* out_state) {
  switch (suspend_reason & DEVICE_MASK_SUSPEND_REASON) {
    case DEVICE_SUSPEND_REASON_MEXEC: {
      AcpiTerminate();
      return ZX_OK;
    }
    case DEVICE_SUSPEND_REASON_REBOOT:
      if (suspend_reason == DEVICE_SUSPEND_REASON_REBOOT_BOOTLOADER) {
        reboot_bootloader();
      } else if (suspend_reason == DEVICE_SUSPEND_REASON_REBOOT_RECOVERY) {
        reboot_recovery();
      } else {
        reboot();
      }
      // Kill this driver so that the IPC channel gets closed; devmgr will
      // perform a fallback that should shutdown or reboot the machine.
      exit(0);
    case DEVICE_SUSPEND_REASON_POWEROFF:
      poweroff();
      exit(0);
    case DEVICE_SUSPEND_REASON_SUSPEND_RAM:
      return suspend_to_ram();
    default:
      return ZX_ERR_NOT_SUPPORTED;
  };
}

zx_status_t publish_acpi_devices(acpi::Acpi* acpi, zx_device_t* platform_bus,
                                 zx_device_t* acpi_root) {
  zx_status_t status = pwrbtn_init(acpi_root);
  if (status != ZX_OK) {
    zxlogf(ERROR, "acpi: failed to initialize pwrbtn device: %d", status);
  }

  // Walk the devices in the ACPI tree, handling any device specific quirks as
  // we go, and publishing any static metadata we need to publish before
  // publishing any devices.
  //
  // TODO(fxbug.dev/56832): Remove this pass when we have a better way to manage
  // driver dependencies on ACPI.  Once drivers can access their metadata
  // directly via a connection to the ACPI driver, we will not need to bother
  // with publishing static metadata before we publish devices.
  LastPciBbnTracker last_pci_bbn(acpi);

  acpi::status<> acpi_status = acpi->WalkNamespace(
      ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT, MAX_NAMESPACE_DEPTH,
      [acpi_root, acpi, &last_pci_bbn](ACPI_HANDLE object, uint32_t level,
                                       acpi::WalkDirection dir) -> acpi::status<> {
        // If we are ascending, tell our PciBbn tracker so that it can properly
        // invalidate our last BBN when needed.
        if (dir == acpi::WalkDirection::Ascending) {
          last_pci_bbn.Ascend(level);
          return acpi::ok();
        }

        // We are descending.  Grab our object info.
        acpi::UniquePtr<ACPI_DEVICE_INFO> info;
        if (auto res = acpi::GetObjectInfo(object); res.is_error()) {
          return res.take_error();
        } else {
          info = std::move(res.value());
        }

        // Apply any workarounds for quirks.
        acpi_apply_workarounds(acpi, object, info.get());

        // If this is a PCI node we are passing through, track it's BBN.  We
        // will need it in order to publish metadata for the devices we
        // encounter.  If we encounter a fatal condition, terminate the walk.
        if (last_pci_bbn.Descend(level, object, *info) != ZX_OK) {
          return acpi::error(AE_ERROR);
        }

        // Is this an HDAS (Intel HDA audio controller)?
        // If so, attempt to publish their relevant metadata so that the
        // device driver can access it when the PCI device itself is finally
        // published.
        //
        // TODO(fxbug.dev/56832): Remove this when we have a better way to manage driver
        // dependencies on ACPI.
        constexpr uint32_t kHDAS_Id = make_fourcc('H', 'D', 'A', 'S');

        if (info->Name == kHDAS_Id) {
          // We must have already seen at least one PCI root due to traversal order.
          if (!last_pci_bbn.has_value()) {
            zxlogf(WARNING,
                   "acpi: Found HDAS/I2Cx node (\"%s\"), but no prior PCI root was discovered!",
                   fourcc_to_string(info->Name).str);
          } else if (!(info->Valid & ACPI_VALID_ADR)) {
            zxlogf(WARNING, "acpi: no valid ADR found for device \"%s\"",
                   fourcc_to_string(info->Name).str);
          } else {
            if (info->Name == kHDAS_Id) {
              // Attaching metadata to the HDAS device /dev/sys/platform/pci/...
              zx_status_t status =
                  nhlt_publish_metadata(acpi, acpi_root, last_pci_bbn.bbn(),
                                        static_cast<uint64_t>(info->Address), object);
              if ((status != ZX_OK) && (status != ZX_ERR_NOT_FOUND)) {
                zxlogf(ERROR, "acpi: failed to publish NHLT metadata");
              }
            }
          }
        }

        return acpi::ok();
      });

  if (acpi_status.is_error()) {
    zxlogf(WARNING, "acpi: Error (%d) during fixup and metadata pass", acpi_status.error_value());
  }

  // Now walk the ACPI namespace looking for devices we understand, and publish
  // them.  For now, publish only the first PCI bus we encounter.
  // TODO(fxbug.dev/78349): remove this when all drivers are removed from the x86 board driver.
  acpi_status = acpi->WalkNamespace(
      ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT, MAX_NAMESPACE_DEPTH,
      [acpi_root](ACPI_HANDLE object, uint32_t level, acpi::WalkDirection dir) -> acpi::status<> {
        // We don't have anything useful to do during the ascent phase.  Just
        // skip it.
        if (dir == acpi::WalkDirection::Ascending) {
          return acpi::ok();
        }

        // We are descending.  Grab our object info.
        acpi::UniquePtr<ACPI_DEVICE_INFO> info;
        if (auto res = acpi::GetObjectInfo(object); res.is_error()) {
          return res.take_error();
        } else {
          info = std::move(res.value());
        }

        // Extract pointers to the hardware ID and the compatible ID if present.
        // If there is no hardware ID, just skip the device.
        const std::string_view hid = hid_from_acpi_devinfo(*info);
        if (hid.empty()) {
          return acpi::ok();
        }

        // Now, if we recognize the HID, go ahead and deal with publishing the
        // device.
        if (hid == BATTERY_HID_STRING) {
          battery_init(acpi_root, object);
        } else if (hid == LID_HID_STRING) {
          lid_init(acpi_root, object);
        } else if (hid == PWRSRC_HID_STRING) {
          pwrsrc_init(acpi_root, object);
        } else if (hid == EC_HID_STRING) {
          ec_init(acpi_root, object);
        } else if (hid == GOOGLE_TBMC_HID_STRING) {
          tbmc_init(acpi_root, object);
        } else if (hid == GOOGLE_CROS_EC_HID_STRING) {
          cros_ec_lpc_init(acpi_root, object);
        } else if (hid == DPTF_THERMAL_HID_STRING) {
          thermal_init(acpi_root, info.get(), object);
        }
        return acpi::ok();
      });

  acpi::Manager manager(acpi, acpi_root);
  auto result = manager.DiscoverDevices();
  if (result.is_error()) {
    zxlogf(INFO, "discover devices failed");
  }
  result = manager.ConfigureDiscoveredDevices();
  if (result.is_error()) {
    zxlogf(INFO, "configure failed");
  }
  result = manager.PublishDevices(platform_bus);

  if (acpi_status.is_error()) {
    return ZX_ERR_BAD_STATE;
  } else {
    return ZX_OK;
  }
}
