// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_CONTIGUOUS_POOLED_MEMORY_ALLOCATOR_H_
#define SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_CONTIGUOUS_POOLED_MEMORY_ALLOCATOR_H_

#include <lib/async/wait.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/zx/bti.h>
#include <lib/zx/event.h>
#include <zircon/limits.h>

#include <fbl/vector.h>
#include <region-alloc/region-alloc.h>

#include "allocator.h"

namespace sysmem_driver {

class ContiguousPooledMemoryAllocator : public MemoryAllocator {
 public:
  ContiguousPooledMemoryAllocator(Owner* parent_device, const char* allocation_name,
                                  inspect::Node* parent_node, uint64_t pool_id, uint64_t size,
                                  bool is_cpu_accessible, bool is_ready, bool can_be_torn_down,
                                  async_dispatcher_t* dispatcher = nullptr);

  ~ContiguousPooledMemoryAllocator();

  // Default to page alignment.
  zx_status_t Init(uint32_t alignment_log2 = ZX_PAGE_SHIFT);

  // TODO(fxbug.dev/13609): Use this for VDEC.
  //
  // This uses a physical VMO as the parent VMO.
  zx_status_t InitPhysical(zx_paddr_t paddr);

  zx_status_t Allocate(uint64_t size, std::optional<std::string> name,
                       zx::vmo* parent_vmo) override;
  zx_status_t SetupChildVmo(const zx::vmo& parent_vmo, const zx::vmo& child_vmo,
                            llcpp::fuchsia::sysmem2::SingleBufferSettings buffer_settings) override;
  void Delete(zx::vmo parent_vmo) override;
  bool is_empty() override {
    // If the contiguous VMO has been marked as secure there's no way to unmark it as secure, so
    // unbinding would never be safe.
    return regions_.empty() && (can_be_torn_down_ || !is_ready_);
  }

  zx_status_t GetPhysicalMemoryInfo(uint64_t* base, uint64_t* size) override {
    *base = start_;
    *size = size_;
    return ZX_OK;
  }

  void set_ready() override;
  bool is_ready() override;

  const zx::vmo& GetPoolVmoForTest() { return contiguous_vmo_; }

 private:
  struct RegionData {
    std::string name;
    zx_koid_t koid;
    inspect::Node node;
    inspect::UintProperty size_property;
    inspect::UintProperty koid_property;
    RegionAllocator::Region::UPtr ptr;
  };

  zx_status_t InitCommon(zx::vmo local_contiguous_vmo);
  void TraceObserverCallback(async_dispatcher_t* dispatcher, async::WaitBase* wait,
                             zx_status_t status, const zx_packet_signal_t* signal);
  void DumpPoolStats();
  void DumpPoolHighWaterMark();
  void TracePoolSize(bool initial_trace);
  Owner* const parent_device_{};
  const char* const allocation_name_{};
  const uint64_t pool_id_{};
  char child_name_[ZX_MAX_NAME_LEN] = {};
  zx::vmo contiguous_vmo_;
  zx::pmt pool_pmt_;
  RegionAllocator region_allocator_;
  // From parent_vmo handle to std::unique_ptr<>
  std::map<zx_handle_t, RegionData> regions_;
  uint64_t start_{};
  uint64_t size_{};
  bool is_cpu_accessible_{};
  bool is_ready_{};
  // True if the allocator can be deleted after it's marked ready.
  bool can_be_torn_down_{};

  uint64_t high_water_mark_used_size_{};
  uint64_t max_free_size_at_high_water_mark_{};

  inspect::Node node_;
  inspect::UintProperty size_property_;
  inspect::UintProperty high_water_mark_property_;
  inspect::UintProperty used_size_property_;
  inspect::UintProperty allocations_failed_property_;
  inspect::UintProperty last_allocation_failed_timestamp_ns_property_;
  // Keeps track of how many allocations would have succeeded but failed due to fragmentation.
  inspect::UintProperty allocations_failed_fragmentation_property_;
  // This is the size of a the largest free contiguous region when high_water_mark_property_ was
  // last modified. It can be used to determine how much space was wasted due to fragmentation.
  inspect::UintProperty max_free_at_high_water_property_;
  // size - high_water_mark. This is used for cobalt reporting.
  inspect::UintProperty free_at_high_water_mark_property_;
  inspect::BoolProperty is_ready_property_;

  zx::event trace_observer_event_;
  async::WaitMethod<ContiguousPooledMemoryAllocator,
                    &ContiguousPooledMemoryAllocator::TraceObserverCallback>
      wait_{this};
};

}  // namespace sysmem_driver

#endif  // SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_CONTIGUOUS_POOLED_MEMORY_ALLOCATOR_H_
