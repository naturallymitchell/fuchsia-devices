// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of tag() source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "pci.h"

#include <assert.h>
#include <lib/zx/handle.h>
#include <lib/zx/port.h>
#include <zircon/syscalls/port.h>

#include <ddk/debug.h>
#include <ddk/protocol/pci.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <virtio/virtio.h>

namespace virtio {

PciBackend::PciBackend(pci_protocol_t pci, zx_pcie_device_info_t info) : pci_(pci), info_(info) {
  snprintf(tag_, sizeof(tag_), "pci[%02x:%02x.%1x]", info_.bus_id, info_.dev_id, info_.func_id);
}

zx_status_t PciBackend::Bind() {
  zx_handle_t tmp_handle;
  zx_status_t st;

  st = zx::port::create(/*options=*/ZX_PORT_BIND_TO_INTERRUPT, &wait_port_);
  if (st != ZX_OK) {
    zxlogf(ERROR, "%s: cannot create wait port: %d\n", tag(), st);
    return st;
  }

  // enable bus mastering
  if ((st = pci_enable_bus_master(&pci_, true)) != ZX_OK) {
    zxlogf(ERROR, "%s: cannot enable bus master %d\n", tag(), st);
    return st;
  }

  // try to set up our IRQ mode
  uint32_t avail_irqs = 0;
  zx_pci_irq_mode_t mode = ZX_PCIE_IRQ_MODE_MSI;
  if ((st = pci_query_irq_mode(&pci_, mode, &avail_irqs)) != ZX_OK || avail_irqs == 0) {
    mode = ZX_PCIE_IRQ_MODE_LEGACY;
    if ((st = pci_query_irq_mode(&pci_, mode, &avail_irqs)) != ZX_OK || avail_irqs == 0) {
      zxlogf(ERROR, "%s: no available IRQs found\n", tag());
      return st;
    }
  }

  if ((st = pci_set_irq_mode(&pci_, mode, 1)) != ZX_OK) {
    zxlogf(ERROR, "%s: failed to set irq mode %u\n", tag(), mode);
    return st;
  }

  if ((st = pci_map_interrupt(&pci_, 0, &tmp_handle)) != ZX_OK) {
    zxlogf(ERROR, "%s: failed to map irq %d\n", tag(), st);
    return st;
  }

  st = zx_interrupt_bind(tmp_handle, wait_port_.get(), /*key=*/0, /*options=*/0);
  if (st != ZX_OK) {
    zxlogf(ERROR, "%s: failed to bind interrupt %d\n", tag(), st);
    return st;
  }

  irq_handle_.reset(tmp_handle);
  zxlogf(SPEW, "%s: irq handle %u\n", tag(), irq_handle_.get());
  return Init();
}

zx_status_t PciBackend::InterruptValid() {
  if (!irq_handle_.get()) {
    return ZX_ERR_BAD_HANDLE;
  }
  return ZX_OK;
}

zx_status_t PciBackend::WaitForInterrupt() {
  zx_port_packet packet;
  return wait_port_.wait(zx::deadline_after(zx::msec(100)), &packet);
}

void PciBackend::InterruptAck() { zx_interrupt_ack(irq_handle_.get()); }

}  // namespace virtio
