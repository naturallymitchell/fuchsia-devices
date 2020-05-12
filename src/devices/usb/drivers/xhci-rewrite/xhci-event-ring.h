// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_USB_DRIVERS_XHCI_REWRITE_XHCI_EVENT_RING_H_
#define SRC_DEVICES_USB_DRIVERS_XHCI_REWRITE_XHCI_EVENT_RING_H_
#include <lib/dma-buffer/buffer.h>
#include <lib/mmio/mmio.h>
#include <lib/zx/bti.h>
#include <zircon/hw/usb.h>

#include <fbl/auto_lock.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/mutex.h>

#include "registers.h"
#include "synchronous_executor.h"
#include "xhci-context.h"
#include "xhci-hub.h"

namespace usb_xhci {

// Event Ring Segment table entry (6.5)
struct ERSTEntry {
  uint64_t address;
  union {
    uint64_t value;
    // Number of entries in ring segment.
    // Must be between 16 and 4096.
    // In practice, this will always be set to 256
    uint16_t size;
  } u;
};

// Used for managing event ring segments.
// This table can be expanded and shrunk as event ring
// segments are added and removed.
class EventRingSegmentTable {
 public:
  zx_status_t Init(size_t page_size, const zx::bti& bti, bool is_32bit, uint32_t erst_max,
                   ERSTSZ erst_size, dma_buffer::BufferFactory* factory, ddk::MmioBuffer* mmio);
  zx_status_t AddSegment(zx_paddr_t paddr);
  ERSTEntry* entries() { return entries_; }
  zx_paddr_t erst() { return erst_->phys()[0]; }
  // Returns the number of segments in this ERST
  uint32_t SegmentCount() { return offset_; }
  uint64_t TrbCount() { return (SegmentCount() * page_size_) / sizeof(TRB); }
  void AddPressure() { erst_pressure_++; }
  size_t Pressure() { return erst_pressure_; }
  void RemovePressure() { erst_pressure_--; }

 private:
  size_t erst_pressure_ = 0;
  ERSTSZ erst_size_;
  std::unique_ptr<dma_buffer::PagedBuffer> erst_;
  // Entries in the event ring segment table.
  // This is valid after Init() is called which
  // allocates the event ring segment table.
  ERSTEntry* entries_;
  // Number of ERST entries
  size_t count_ = 0;
  // Offset in ERST table
  uint32_t offset_ = 0;
  // BTI used for obtaining physical memory addresses.
  // This is valid for the lifetime of the UsbXhci driver,
  // and is owned by UsbXhci.
  const zx::bti* bti_;
  size_t page_size_;
  bool is_32bit_;
  std::optional<ddk::MmioView> mmio_;
};

// Keeps track of events received from the XHCI controller
class UsbXhci;
struct PortState;
class CommandRing;
class EventRing {
 public:
  // Adds a segment to the event ring.
  zx_status_t Init(size_t page_size, const zx::bti& bti, ddk::MmioBuffer* buffer, bool is_32bit,
                   uint32_t erst_max, ERSTSZ erst_size, ERDP erdp_reg, IMAN iman_reg,
                   uint8_t cap_length, HCSPARAMS1 hcs_params_1, CommandRing* command_ring,
                   DoorbellOffset doorbell_offset, UsbXhci* hci, HCCPARAMS1 hcc_params_1,
                   uint64_t* dcbaa);
  // Disable thread safety analysis here.
  // We don't need to hold the mutex just to read the ERST
  // paddr, as this will never change (it is effectively a constant).
  // We don't need to incurr the overhead of acquiring the mutex for this.
  zx_paddr_t erst() __TA_NO_THREAD_SAFETY_ANALYSIS { return segments_.erst(); }
  void RemovePressure();
  size_t GetPressure();
  zx_status_t AddSegmentIfNone();
  zx_status_t AddTRB();
  zx_status_t AddSegment();
  zx_paddr_t erdp_phys() { return erdp_phys_; }
  TRB* erdp_virt() { return erdp_virt_; }
  zx_status_t HandleIRQ();
  zx_status_t Ring0Bringup();
  void ScheduleTask(fit::promise<TRB*, zx_status_t> promise);
  void RunUntilIdle();

 private:
  synchronous_executor executor_;
  zx_status_t HandlePortStatusChangeEvent(uint8_t port_id);
  zx_status_t LinkUp(uint8_t port_id);
  Control AdvanceErdp() {
    fbl::AutoLock _(&segment_mutex_);
    erdp_ = (erdp_ + 1) % segments_.TrbCount();
    if (unlikely(!erdp_)) {
      erdp_virt_ = reinterpret_cast<TRB*>(buffers_.front().virt());
      erdp_phys_ = buffers_.front().phys();
      ccs_ = !ccs_;
      buffers_it_ = buffers_.begin();
      segment_index_ =
          static_cast<uint8_t>((segment_index_ + 1) % segments_.SegmentCount()) & 0b111;
    } else {
      if (unlikely((reinterpret_cast<size_t>(erdp_virt_ + 1) / 4096) !=
                   (reinterpret_cast<size_t>(erdp_virt_) / 4096))) {
        // Page transition -- next buffer
        buffers_it_++;
        erdp_virt_ = reinterpret_cast<TRB*>((*buffers_it_).virt());
        erdp_phys_ = (*buffers_it_).phys();
      } else {
        erdp_virt_++;
        erdp_phys_ += sizeof(TRB);
      }
    }
    return Control::FromTRB(erdp_virt_);
  }
  // USB 3.0 device attach
  void Usb3DeviceAttach(uint16_t port_id);
  // USB 2.0 device attach
  void Usb2DeviceAttach(uint16_t port_id);

  fbl::DoublyLinkedList<std::unique_ptr<dma_buffer::ContiguousBuffer>> buffers_;
  fbl::DoublyLinkedList<std::unique_ptr<dma_buffer::ContiguousBuffer>>::iterator buffers_it_;

  // Virtual address of the event ring dequeue pointer
  TRB* erdp_virt_ = nullptr;

  // Event ring dequeue pointer (index)
  size_t erdp_ = 0;

  // Event ring dequeue pointer (physical address)
  zx_paddr_t erdp_phys_ = 0;

  // Current Cycle State
  bool ccs_ = 1;
  fbl::Mutex segment_mutex_;
  size_t trbs_ __TA_GUARDED(segment_mutex_);
  EventRingSegmentTable segments_ __TA_GUARDED(segment_mutex_);
  // BTI used for obtaining physical memory addresses.
  // This is valid for the lifetime of the UsbXhci driver,
  // and is owned by UsbXhci.
  const zx::bti* bti_;
  size_t page_size_;
  bool is_32bit_;
  // Pointer to the MMIO buffer for writing to xHCI registers
  // This is valid for the lifetime of the UsbXhci driver,
  // and is owned by UsbXhci.
  ddk::MmioBuffer* mmio_;

  // Event ring dequeue pointer register
  ERDP erdp_reg_;

  // Interrupt management register
  IMAN iman_reg_;
  uint8_t segment_index_ = 0;
  UsbXhci* hci_;
  uint8_t cap_length_;
  HCSPARAMS1 hcs_params_1_;
  CommandRing* command_ring_;
  DoorbellOffset doorbell_offset_;
  HCCPARAMS1 hcc_params_1_;

  // Device context base address array
  // This is a pointer into the buffer
  // owned by UsbXhci, which this is a child of.
  // When xHCI shuts down, this pointer will be invalid.
  uint64_t* dcbaa_;
};
}  // namespace usb_xhci

#endif  // SRC_DEVICES_USB_DRIVERS_XHCI_REWRITE_XHCI_EVENT_RING_H_
