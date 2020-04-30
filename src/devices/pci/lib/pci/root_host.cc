// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <lib/pci/root.h>
#include <lib/pci/root_host.h>
#include <lib/zx/object.h>
#include <lib/zx/port.h>
#include <lib/zx/process.h>
#include <lib/zx/time.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/port.h>
#include <zircon/syscalls/resource.h>
#include <zircon/time.h>
#include <zircon/types.h>

#include <array>
#include <thread>

#include <ddk/debug.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <region-alloc/region-alloc.h>

const char* kWorkerTag = "RootHostWorker:";
const char* kMainTag = "RootHost:";

// This worker thread exists to monitor eventpair peer closures so it can
// release the allocated regions back to the RootHost's allocators.
// RegionAllocator regions contain a recycle() method which will add them back
// to the allocators they came from. When the allocation thread needs to create a
// window allocation it synchronizes with the worker thread via a syn/ack pair of
// port messages. This is to ensure the worker thread has handled all signals before
// that point, otherwise there can be a race between a process dying and the allocator
// thread trying to re-allocate a window before the worker has has time to act on the
// signal that was generated by the eventpair death. This allows for the port to be
// drained as messages come in rather than when allocations happen which gives the
// RootHost a more current view of resources.
//
// The kernel's bookkeeping for the regions will be handled by the resource
// handles themselves being closed.
//
// TODO(32978): This more complicated book-keeping will be simplified when we
// have devhost isolation between the root host and root implemtnations and will
// be able to use channel endpoints closing for similar notifications.
void PciRootHost::WorkerThreadEntry() {
  while (true) {
    zx_port_packet packet;
    zx_status_t st = worker_port_.wait(zx::time::infinite(), &packet);
    if (st == ZX_OK) {
      switch (packet.type) {
        // The parent thread uses a message to notify to exit.
        case ZX_PKT_TYPE_USER:
          switch (packet.user.u32[0]) {
            case PciRootHost::Message::kSyn: {
              SendMessage(kWorkerTag, &main_port_, PciRootHost::Message::kAck);
              break;
            }
            case PciRootHost::Message::kExit:
              zxlogf(TRACE, "%s received exit signal\n", kWorkerTag);
              return;
            default:
              zxlogf(ERROR, "%s Unexpected message type received by worker: %d\n", kWorkerTag,
                     packet.user.u32[0]);
          }
          break;
        case ZX_PKT_TYPE_SIGNAL_ONE: {
          // An eventpair downstream has died meaning that some resources need
          // to be freedom up based on its key.
          ZX_ASSERT(packet.signal.observed == ZX_EVENTPAIR_PEER_CLOSED);
          fbl::AutoLock lock(&lock_);
          allocations_.erase(packet.key);
        }
      }
    }
  }
}

zx_status_t PciRootHost::SendMessage(const char* tag, zx::port* port, PciRootHost::Message msg) {
  zx_port_packet_t packet = {};
  packet.type = ZX_PKT_TYPE_USER;
  packet.user.u32[0] = msg;
  zx_status_t st = port->queue(&packet);
  if (st != ZX_OK) {
    zxlogf(ERROR, "%s Failed to send msg %#x: %d\n", tag, msg, st);
  }
  return st;
}

// Send a packet to the worker and wait for it to acknowledge it to ensure it has
// handled any eventpair signals in its port queue.
zx_status_t PciRootHost::WaitForWorkerAck() {
  zx_status_t st = SendMessage(kMainTag, &worker_port_, PciRootHost::Message::kSyn);
  if (st != ZX_OK) {
    return st;
  }

  zx_port_packet_t in;
  st = main_port_.wait(zx::time::infinite(), &in);
  if (st != ZX_OK) {
    return st;
  }
  zxlogf(TRACE, "%s received ack\n", kMainTag);

  if (in.type != ZX_PKT_TYPE_USER || in.user.u32[0] != PciRootHost::Message::kAck) {
    zxlogf(ERROR, "%s unexpected packet type (%#x) or payload (%#x)\n", kMainTag, in.type,
           in.user.u32[0]);
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

zx_status_t PciRootHost::AllocateWindow(AllocationType type, zx_paddr_t base, size_t size,
                                        zx::resource* out_resource, zx::eventpair* out_endpoint) {
  RegionAllocator* allocator = nullptr;
  const char* allocator_name = nullptr;
  uint32_t rsrc_kind = 0;
  if (type == kIo) {
    allocator = &io_alloc_;
    allocator_name = "Io";
    rsrc_kind = ZX_RSRC_KIND_IOPORT;
  } else if (type == kMmio32) {
    allocator = &mmio32_alloc_;
    allocator_name = "Mmio32";
    rsrc_kind = ZX_RSRC_KIND_MMIO;
  } else if (type == kMmio64) {
    allocator = &mmio64_alloc_;
    allocator_name = "Mmio64";
    rsrc_kind = ZX_RSRC_KIND_MMIO;
  } else {
    return ZX_ERR_INVALID_ARGS;
  }

  zx_status_t st = WaitForWorkerAck();
  if (st != ZX_OK) {
    return st;
  }

  // If |base| is set then we have been requested to find address space starting
  // at a given |base|. If it's zero then we just need a region big enough for
  // the request, starting anywhere. Some address space requests will want a
  // given address / size because they are for devices already configured by the
  // bios at boot.
  RegionAllocator::Region::UPtr region_uptr;
  if (base) {
    const ralloc_region_t region = {
        .base = base,
        .size = size,
    };
    st = allocator->GetRegion(region, region_uptr);
  } else {
    st = allocator->GetRegion(static_cast<uint64_t>(size), region_uptr);
  }

  if (st != ZX_OK) {
    zxlogf(TRACE, "%s failed to allocate %s %#lx-%#lx: %d.\n", kMainTag, allocator_name, base,
           base + size, st);
    if (zxlog_level_enabled(TRACE)) {
      zxlogf(TRACE, "%s Regions available:\n", kMainTag);
      allocator->WalkAvailableRegions([](const ralloc_region_t* r) -> bool {
        zxlogf(TRACE, "    %#lx - %#lx\n", r->base, r->base + r->size);
        return true;
      });
    }
    return st;
  }

  // Names will be generated in the format of: PCI [Mm]io[32|64]
  std::array<char, ZX_MAX_NAME_LEN> name = {};
  snprintf(name.data(), name.size(), "PCI %s", allocator_name);

  // Craft a resource handle for the request. All information for the allocation that the
  // caller needs is held in the resource, so we don't need explicitly pass back other parameters.
  st = zx::resource::create(*zx::unowned_resource(root_resource_),
                            rsrc_kind | ZX_RSRC_FLAG_EXCLUSIVE, region_uptr->base,
                            region_uptr->size, name.data(), name.size(), out_resource);
  if (st != ZX_OK) {
    return st;
  }

  // Cache the allocated region's values for output later before the uptr is moved.
  uint64_t new_base = region_uptr->base;
  size_t new_size = region_uptr->size;
  st = RecordAllocation(std::move(region_uptr), out_endpoint);
  if (st != ZX_OK) {
    return st;
  }

  // Discard the lifecycle aspect of the returned pointer, we'll be tracking it on the bus
  // side of things.
  zxlogf(TRACE, "%s assigned %s %#lx-%#lx to PciRoot.\n", kMainTag, allocator_name, new_base,
         new_base + new_size);
  return ZX_OK;
}

zx_status_t PciRootHost::RecordAllocation(RegionAllocator::Region::UPtr region,
                                          zx::eventpair* out_endpoint) {
  zx::eventpair root_host_endpoint;
  zx_status_t st = zx::eventpair::create(0, &root_host_endpoint, out_endpoint);
  if (st != ZX_OK) {
    zxlogf(TRACE, "%s Failed to create eventpair: %d\n", kMainTag, st);
    return st;
  }

  // If |out_endpoint| is closed we can reap the resource allocation given to the bus driver.
  fbl::AutoLock lock(&lock_);
  uint64_t key = ++alloc_key_cnt_;
  st = root_host_endpoint.wait_async(worker_port_, key, ZX_EVENTPAIR_PEER_CLOSED, 0 /* options */);
  if (st != ZX_OK) {
    zxlogf(TRACE, "%s Failed to set up tracking for resource allocation: %d\n", kMainTag, st);
    return st;
  }

  // Storing the same |key| value allows us to track the eventpair peer closure
  // through the packet sent back on the port.
  allocations_[key] =
      std::make_unique<WindowAllocation>(std::move(root_host_endpoint), std::move(region));
  return ZX_OK;
}

zx_status_t PciRootHost::Init(const zx::unowned_resource root_resource) {
  assert(!initialized_);

  zx_status_t st = zx::port::create(0, &worker_port_);
  if (st != ZX_OK) {
    return st;
  }

  st = zx::port::create(0, &main_port_);
  if (st != ZX_OK) {
    return st;
  }

  lifetime_thrd_ = std::thread(&PciRootHost::WorkerThreadEntry, this);
  root_resource_ = root_resource;
  initialized_ = true;

  return ZX_OK;
}

PciRootHost::~PciRootHost() {
  SendMessage(kMainTag, &worker_port_, PciRootHost::Message::kExit);
  lifetime_thrd_.join();
}
