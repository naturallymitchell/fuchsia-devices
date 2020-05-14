// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "power.h"

#include <zircon/syscalls/system.h>

#include <acpica/acpi.h>
#include <ddk/debug.h>

void poweroff(void) {
  ACPI_STATUS status = AcpiEnterSleepStatePrep(5);
  if (status == AE_OK) {
    AcpiEnterSleepState(5);
  }
}

void reboot(void) {
  // Please do not use get_root_resource() in new code. See ZX-1467.
  zx_status_t status = zx_system_powerctl(get_root_resource(), ZX_SYSTEM_POWERCTL_REBOOT, NULL);
  if (status != ZX_OK)
    zxlogf(ERROR, "acpi: Failed to enter reboot: %d", status);
  AcpiReset();
}

void reboot_bootloader(void) {
  // Please do not use get_root_resource() in new code. See ZX-1467.
  zx_status_t status =
      zx_system_powerctl(get_root_resource(), ZX_SYSTEM_POWERCTL_REBOOT_BOOTLOADER, NULL);
  if (status != ZX_OK)
    zxlogf(ERROR, "acpi: Failed to enter bootloader reboot: %d", status);
  AcpiReset();
}

void reboot_recovery(void) {
  // Please do not use get_root_resource() in new code. See ZX-1467.
  zx_status_t status =
      zx_system_powerctl(get_root_resource(), ZX_SYSTEM_POWERCTL_REBOOT_RECOVERY, NULL);
  if (status != ZX_OK)
    zxlogf(ERROR, "acpi: Failed to enter recovery reboot: %d", status);
  AcpiReset();
}

zx_status_t suspend_to_ram(void) {
  zx_status_t status = ZX_OK;

  acpica_enable_noncontested_mode();

  // Please do not use get_root_resource() in new code. See ZX-1467.
  status = zx_system_powerctl(get_root_resource(), ZX_SYSTEM_POWERCTL_DISABLE_ALL_CPUS_BUT_PRIMARY,
                              NULL);
  if (status != ZX_OK) {
    zxlogf(ERROR, "acpi: Failed to shutdown CPUs: %d", status);
    goto cleanup;
  }

  ACPI_STATUS acpi_status;
  acpi_status = AcpiEnterSleepStatePrep(3);
  if (acpi_status != AE_OK) {
    zxlogf(ERROR, "acpi: Failed to prep enter sleep state: %x", acpi_status);
    // TODO: I think we need to do LeaveSleepState{Prep,} on failure
    status = ZX_ERR_INTERNAL;
    goto cleanup;
  }

  acpi_status = AcpiEnterSleepState(3);
  if (acpi_status != AE_OK) {
    status = ZX_ERR_INTERNAL;
    zxlogf(ERROR, "acpi: Failed to enter sleep state: %x", acpi_status);
    // Continue executing to try to get the system back to where it was
  }
  zxlogf(DEBUG, "acpi: Woke up from sleep");

  acpi_status = AcpiLeaveSleepStatePrep(3);
  if (acpi_status != AE_OK) {
    status = ZX_ERR_INTERNAL;
    zxlogf(ERROR, "acpi: Failed to prep leave sleep state: %x", acpi_status);
  }

  acpi_status = AcpiLeaveSleepState(3);
  if (acpi_status != AE_OK) {
    status = ZX_ERR_INTERNAL;
    zxlogf(ERROR, "acpi: Failed to leave sleep state: %x", acpi_status);
  }

cleanup:
  zx_status_t status2;
  // Please do not use get_root_resource() in new code. See ZX-1467.
  status2 = zx_system_powerctl(get_root_resource(), ZX_SYSTEM_POWERCTL_ENABLE_ALL_CPUS, NULL);
  if (status2 != ZX_OK) {
    zxlogf(ERROR, "acpi: Re-enabling all cpus failed: %d", status2);
  }

  acpica_disable_noncontested_mode();

  zxlogf(INFO, "acpi: Finished processing suspend: %d", status);
  return status;
}
