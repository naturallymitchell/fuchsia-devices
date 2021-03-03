// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "contiguous_pooled_memory_allocator.h"

#include <fuchsia/sysmem2/llcpp/fidl.h>
#include <lib/zx/clock.h>

#include <ddk/trace/event.h>
#include <fbl/string_printf.h>

#include "lib/fidl/llcpp/fidl_allocator.h"
#include "macros.h"

namespace sysmem_driver {

zx::duration kGuardCheckInterval = zx::sec(5);
namespace {

fuchsia_sysmem2::wire::HeapProperties BuildHeapProperties(fidl::AnyAllocator& allocator,
                                                          bool is_cpu_accessible) {
  using fuchsia_sysmem2::wire::CoherencyDomainSupport;
  using fuchsia_sysmem2::wire::HeapProperties;

  auto coherency_domain_support = CoherencyDomainSupport(allocator);
  coherency_domain_support.set_cpu_supported(allocator, is_cpu_accessible);
  coherency_domain_support.set_ram_supported(allocator, is_cpu_accessible);
  coherency_domain_support.set_inaccessible_supported(allocator, true);

  auto heap_properties = HeapProperties(allocator);
  heap_properties.set_coherency_domain_support(allocator, std::move(coherency_domain_support));
  heap_properties.set_need_clear(allocator, is_cpu_accessible);

  return heap_properties;
}

}  // namespace

ContiguousPooledMemoryAllocator::ContiguousPooledMemoryAllocator(
    Owner* parent_device, const char* allocation_name, inspect::Node* parent_node, uint64_t pool_id,
    uint64_t size, bool is_cpu_accessible, bool is_ready, bool can_be_torn_down,
    async_dispatcher_t* dispatcher)
    : MemoryAllocator(
          parent_device->table_set(),
          BuildHeapProperties(parent_device->table_set().allocator(), is_cpu_accessible)),
      parent_device_(parent_device),
      allocation_name_(allocation_name),
      pool_id_(pool_id),
      region_allocator_(RegionAllocator::RegionPool::Create(std::numeric_limits<size_t>::max())),
      size_(size),
      is_cpu_accessible_(is_cpu_accessible),
      is_ready_(is_ready),
      can_be_torn_down_(can_be_torn_down) {
  snprintf(child_name_, sizeof(child_name_), "%s-child", allocation_name_);
  // Ensure NUL-terminated.
  child_name_[sizeof(child_name_) - 1] = 0;
  node_ = parent_node->CreateChild(allocation_name);
  node_.CreateUint("size", size, &properties_);
  node_.CreateUint("id", id(), &properties_);
  high_water_mark_property_ = node_.CreateUint("high_water_mark", 0);
  free_at_high_water_mark_property_ = node_.CreateUint("free_at_high_water_mark", size);
  used_size_property_ = node_.CreateUint("used_size", 0);
  allocations_failed_property_ = node_.CreateUint("allocations_failed", 0);
  last_allocation_failed_timestamp_ns_property_ =
      node_.CreateUint("last_allocation_failed_timestamp_ns", 0);
  allocations_failed_fragmentation_property_ =
      node_.CreateUint("allocations_failed_fragmentation", 0);
  max_free_at_high_water_property_ = node_.CreateUint("max_free_at_high_water", size);
  is_ready_property_ = node_.CreateBool("is_ready", is_ready_);
  failed_guard_region_checks_property_ =
      node_.CreateUint("failed_guard_region_checks", failed_guard_region_checks_);
  last_failed_guard_region_check_timestamp_ns_property_ =
      node_.CreateUint("last_failed_guard_region_check_timestamp_ns", 0);

  if (dispatcher) {
    zx_status_t status = zx::event::create(0, &trace_observer_event_);
    ZX_ASSERT(status == ZX_OK);
    status = trace_register_observer(trace_observer_event_.get());
    ZX_ASSERT(status == ZX_OK);
    wait_.set_object(trace_observer_event_.get());
    wait_.set_trigger(ZX_EVENT_SIGNALED);

    status = wait_.Begin(dispatcher);
    ZX_ASSERT(status == ZX_OK);
  }
}

void ContiguousPooledMemoryAllocator::InitGuardRegion(size_t guard_region_size,
                                                      bool internal_guard_regions,
                                                      bool crash_on_guard_failure,
                                                      async_dispatcher_t* dispatcher) {
  ZX_DEBUG_ASSERT(!regions_.size());
  ZX_DEBUG_ASSERT(!guard_region_data_.size());
  ZX_DEBUG_ASSERT(contiguous_vmo_.get());
  if (!guard_region_size) {
    return;
  }
  ZX_DEBUG_ASSERT(is_cpu_accessible_);
  ZX_DEBUG_ASSERT(guard_region_size % ZX_PAGE_SIZE == 0);
  has_internal_guard_regions_ = internal_guard_regions;
  guard_region_data_.resize(guard_region_size);
  guard_region_copy_.resize(guard_region_size);
  for (size_t i = 0; i < guard_region_size; i++) {
    guard_region_data_[i] = ((i + 1) % 256);
  }

  crash_on_guard_failure_ = crash_on_guard_failure;

  // Initialize external guard regions.
  ralloc_region_t regions[] = {{.base = 0, .size = guard_region_size},
                               {.base = size_ - guard_region_size, .size = guard_region_size}};

  for (auto& region : regions) {
    zx_status_t status = region_allocator_.SubtractRegion(region);
    ZX_DEBUG_ASSERT(status == ZX_OK);
    status =
        contiguous_vmo_.write(guard_region_data_.data(), region.base, guard_region_data_.size());
    ZX_DEBUG_ASSERT(status == ZX_OK);
  }

  zx_status_t status = guard_checker_.PostDelayed(dispatcher, kGuardCheckInterval);
  ZX_ASSERT(status == ZX_OK);
}

ContiguousPooledMemoryAllocator::~ContiguousPooledMemoryAllocator() {
  ZX_DEBUG_ASSERT(is_empty());
  wait_.Cancel();
  if (trace_observer_event_) {
    trace_unregister_observer(trace_observer_event_.get());
  }
  guard_checker_.Cancel();
}

zx_status_t ContiguousPooledMemoryAllocator::Init(uint32_t alignment_log2) {
  zx::vmo local_contiguous_vmo;
  zx_status_t status = zx::vmo::create_contiguous(parent_device_->bti(), size_, alignment_log2,
                                                  &local_contiguous_vmo);
  if (status != ZX_OK) {
    LOG(ERROR, "Could not allocate contiguous memory, status %d allocation_name_: %s", status,
        allocation_name_);
    return status;
  }

  return InitCommon(std::move(local_contiguous_vmo));
}

zx_status_t ContiguousPooledMemoryAllocator::InitPhysical(zx_paddr_t paddr) {
  zx::vmo local_contiguous_vmo;
  zx_status_t status = parent_device_->CreatePhysicalVmo(paddr, size_, &local_contiguous_vmo);
  if (status != ZX_OK) {
    LOG(ERROR, "Failed to create physical VMO: %d allocation_name_: %s", status, allocation_name_);
    return status;
  }

  return InitCommon(std::move(local_contiguous_vmo));
}

zx_status_t ContiguousPooledMemoryAllocator::InitCommon(zx::vmo local_contiguous_vmo) {
  zx_status_t status =
      local_contiguous_vmo.set_property(ZX_PROP_NAME, allocation_name_, strlen(allocation_name_));
  if (status != ZX_OK) {
    LOG(ERROR, "Failed vmo.set_property(ZX_PROP_NAME, ...): %d", status);
    return status;
  }

  zx_info_vmo_t info;
  status = local_contiguous_vmo.get_info(ZX_INFO_VMO, &info, sizeof(info), nullptr, nullptr);
  if (status != ZX_OK) {
    LOG(ERROR, "Failed local_contiguous_vmo.get_info(ZX_INFO_VMO, ...) - status: %d", status);
    return status;
  }
  // Only secure/protected RAM ever uses a physical VMO.  Not all secure/protected RAM uses a
  // physical VMO.
  ZX_DEBUG_ASSERT(ZX_INFO_VMO_TYPE(info.flags) == ZX_INFO_VMO_TYPE_PAGED || !is_cpu_accessible_);
  // Paged VMOs are cached by default.  Physical VMOs are uncached by default.
  ZX_DEBUG_ASSERT((ZX_INFO_VMO_TYPE(info.flags) == ZX_INFO_VMO_TYPE_PAGED) ==
                  (info.cache_policy == ZX_CACHE_POLICY_CACHED));
  // We'd have this assert, except it doesn't work with fake-bti, so for now we trust that when not
  // running a unit test, we have a VMO with info.flags & ZX_INFO_VMO_CONTIGUOUS.
  // ZX_DEBUG_ASSERT(info.flags & ZX_INFO_VMO_CONTIGUOUS);

  // Regardless of CPU or RAM domain, and regardless of contig VMO or physical VMO, if we use the
  // CPU to access the RAM, we want to use the CPU cache.  If we can't use the CPU to access the RAM
  // (on REE side), then we don't want to use the CPU cache.
  //
  // Why we want cached when is_cpu_accessible_:
  //
  // Without setting cached, in addition to presumably being slower, memcpy tends to fail with
  // non-aligned access faults / syscalls that are trying to copy directly to the VMO can fail
  // without it being obvious that it's an underlying non-aligned access fault triggered by memcpy.
  //
  // Why we want uncached when !is_cpu_accessible_:
  //
  // IIUC, it's possible on aarch64 for a cached mapping to protected memory + speculative execution
  // to cause random faults, while a non-cached mapping only faults if a non-cached mapping is
  // actually touched.
  uint32_t desired_cache_policy =
      is_cpu_accessible_ ? ZX_CACHE_POLICY_CACHED : ZX_CACHE_POLICY_UNCACHED;
  if (info.cache_policy != desired_cache_policy) {
    status = local_contiguous_vmo.set_cache_policy(desired_cache_policy);
    if (status != ZX_OK) {
      // TODO(fxbug.dev/34580): Ideally we'd set ZX_CACHE_POLICY_UNCACHED when !is_cpu_accessible_,
      // since IIUC on aarch64 it's possible for a cached mapping to secure/protected memory +
      // speculative execution to cause random faults, while an uncached mapping only faults if the
      // uncached mapping is actually touched.  However, currently for a VMO created with
      // zx::vmo::create_contiguous(), the .set_cache_policy() doesn't work because the VMO already
      // has pages.  Cases where !is_cpu_accessible_ include both Init() and InitPhysical(), so we
      // can't rely on local_contiguous_vmo being a physical VMO.
      if (ZX_INFO_VMO_TYPE(info.flags) == ZX_INFO_VMO_TYPE_PAGED) {
        LOG(ERROR,
            "Ignoring failure to set_cache_policy() on contig VMO - see fxbug.dev/34580 - status: "
            "%d",
            status);
        status = ZX_OK;
        goto keepGoing;
      }
      LOG(ERROR, "Failed to set_cache_policy(): %d", status);
      return status;
    }
  }
keepGoing:;

  zx_paddr_t addrs;
  // When running a unit test, the src/devices/testing/fake-bti provides a fake zx_bti_pin() that
  // should tolerate ZX_BTI_CONTIGUOUS here despite the local_contiguous_vmo not actually having
  // info.flags ZX_INFO_VMO_CONTIGUOUS.
  status = parent_device_->bti().pin(ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE | ZX_BTI_CONTIGUOUS,
                                     local_contiguous_vmo, 0, size_, &addrs, 1, &pool_pmt_);
  if (status != ZX_OK) {
    LOG(ERROR, "Could not pin memory, status %d", status);
    return status;
  }

  start_ = addrs;
  contiguous_vmo_ = std::move(local_contiguous_vmo);
  ralloc_region_t region = {0, size_};
  region_allocator_.AddRegion(region);
  // It is intentional here that ~pmt doesn't imply zx_pmt_unpin().  If sysmem dies, we'll reboot.
  return ZX_OK;
}

zx_status_t ContiguousPooledMemoryAllocator::Allocate(uint64_t size,
                                                      std::optional<std::string> name,
                                                      zx::vmo* parent_vmo) {
  if (!is_ready_) {
    LOG(ERROR, "allocation_name_: %s is not ready_, failing", allocation_name_);
    return ZX_ERR_BAD_STATE;
  }
  RegionAllocator::Region::UPtr region;
  zx::vmo result_parent_vmo;

  const uint64_t guard_region_size = guard_region_data_.size();
  uint64_t allocation_size = size + guard_region_data_.size() * 2;
  // TODO(fxbug.dev/43184): Use a fragmentation-reducing allocator (such as best fit).
  //
  // The "region" param is an out ref.
  zx_status_t status = region_allocator_.GetRegion(allocation_size, ZX_PAGE_SIZE, region);
  if (status != ZX_OK) {
    LOG(WARNING, "GetRegion failed (out of space?) - size: %zu status: %d", size, status);
    DumpPoolStats();
    allocations_failed_property_.Add(1);
    last_allocation_failed_timestamp_ns_property_.Set(zx::clock::get_monotonic().get());
    uint64_t unused_size = 0;
    region_allocator_.WalkAvailableRegions([&unused_size](const ralloc_region_t* r) -> bool {
      unused_size += r->size;
      return true;
    });
    if (unused_size >= size) {
      // There's enough unused memory total, so the allocation must have failed due to
      // fragmentation.
      allocations_failed_fragmentation_property_.Add(1);
    }
    return status;
  }

  TracePoolSize(false);

  if (guard_region_size) {
    status =
        contiguous_vmo_.write(guard_region_data_.data(), region->base, guard_region_data_.size());
    if (status != ZX_OK) {
      LOG(ERROR, "Failed to write pre-guard region.");
      return status;
    }
    status =
        contiguous_vmo_.write(guard_region_data_.data(), region->base + guard_region_size + size,
                              guard_region_data_.size());
    if (status != ZX_OK) {
      LOG(ERROR, "Failed to write post-guard region.");
      return status;
    }
  }

  // The result_parent_vmo created here is a VMO window to a sub-region of contiguous_vmo_.
  status = contiguous_vmo_.create_child(ZX_VMO_CHILD_SLICE, region->base + guard_region_size, size,
                                        &result_parent_vmo);
  if (status != ZX_OK) {
    LOG(ERROR, "Failed vmo.create_child(ZX_VMO_CHILD_SLICE, ...): %d", status);
    return status;
  }

  // If you see a Sysmem*-child VMO you should know that it doesn't actually
  // take up any space, because the same memory is backed by contiguous_vmo_.
  status = result_parent_vmo.set_property(ZX_PROP_NAME, child_name_, strlen(child_name_));
  if (status != ZX_OK) {
    LOG(ERROR, "Failed vmo.set_property(ZX_PROP_NAME, ...): %d", status);
    return status;
  }

  if (!name) {
    name = "Unknown";
  }

  RegionData data;
  data.name = std::move(*name);
  zx_info_handle_basic_t handle_info;
  status = result_parent_vmo.get_info(ZX_INFO_HANDLE_BASIC, &handle_info, sizeof(handle_info),
                                      nullptr, nullptr);
  ZX_ASSERT(status == ZX_OK);
  data.node = node_.CreateChild(fbl::StringPrintf("vmo-%ld", handle_info.koid).c_str());
  data.size_property = data.node.CreateUint("size", size);
  data.koid = handle_info.koid;
  data.koid_property = data.node.CreateUint("koid", handle_info.koid);
  data.ptr = std::move(region);
  regions_.emplace(std::make_pair(result_parent_vmo.get(), std::move(data)));

  *parent_vmo = std::move(result_parent_vmo);
  return ZX_OK;
}

zx_status_t ContiguousPooledMemoryAllocator::SetupChildVmo(
    const zx::vmo& parent_vmo, const zx::vmo& child_vmo,
    fuchsia_sysmem2::wire::SingleBufferSettings buffer_settings) {
  // nothing to do here
  return ZX_OK;
}

void ContiguousPooledMemoryAllocator::Delete(zx::vmo parent_vmo) {
  TRACE_DURATION("gfx", "ContiguousPooledMemoryAllocator::Delete");
  auto it = regions_.find(parent_vmo.get());
  ZX_ASSERT(it != regions_.end());
  CheckGuardRegionData(it->second);
  regions_.erase(it);
  parent_vmo.reset();
  TracePoolSize(false);
  if (is_empty()) {
    parent_device_->CheckForUnbind();
  }
}

void ContiguousPooledMemoryAllocator::set_ready() {
  is_ready_ = true;
  is_ready_property_.Set(is_ready_);
}

void ContiguousPooledMemoryAllocator::CheckGuardRegion(const char* region_name, size_t region_size,
                                                       bool pre, uint64_t start_offset) {
  const uint64_t guard_region_size = guard_region_data_.size();

  zx_status_t status = contiguous_vmo_.op_range(ZX_VMO_OP_CACHE_CLEAN_INVALIDATE, start_offset,
                                                guard_region_size, nullptr, 0);
  ZX_ASSERT(status == ZX_OK);
  status = contiguous_vmo_.read(guard_region_copy_.data(), start_offset, guard_region_copy_.size());
  ZX_ASSERT(status == ZX_OK);
  if (memcmp(guard_region_copy_.data(), guard_region_data_.data(), guard_region_data_.size()) !=
      0) {
    size_t error_start = UINT64_MAX;
    size_t error_end = 0;
    for (size_t i = 0; i < guard_region_copy_.size(); i++) {
      if (guard_region_copy_[i] != guard_region_data_[i]) {
        error_start = std::min(i, error_start);
        error_end = std::max(i, error_end);
      }
    }
    ZX_DEBUG_ASSERT(error_start < guard_region_copy_.size());

    std::string bad_str;
    std::string good_str;
    constexpr uint32_t kRegionSizeToOutput = 16;
    for (uint32_t i = error_start;
         i < error_start + kRegionSizeToOutput && i < guard_region_data_.size(); i++) {
      bad_str += fbl::StringPrintf(" 0x%x", guard_region_copy_[i]).c_str();
      good_str += fbl::StringPrintf(" 0x%x", guard_region_data_[i]).c_str();
    }

    LOG(ERROR,
        "Region %s of size %ld has corruption in %s guard pages between [0x%lx, 0x%lx] - good "
        "\"%s\" bad \"%s\"",
        region_name, region_size, pre ? "pre" : "post", error_start, error_end, good_str.c_str(),
        bad_str.c_str());
    ZX_ASSERT_MSG(!crash_on_guard_failure_, "Crashing due to guard region failure");
    failed_guard_region_checks_++;
    failed_guard_region_checks_property_.Set(failed_guard_region_checks_);
    last_failed_guard_region_check_timestamp_ns_property_.Set(zx::clock::get_monotonic().get());
  }
}

void ContiguousPooledMemoryAllocator::CheckGuardRegionData(const RegionData& region) {
  const uint64_t guard_region_size = guard_region_data_.size();
  if (guard_region_size == 0 || !has_internal_guard_regions_)
    return;
  uint64_t size = region.ptr->size;
  uint64_t vmo_size = size - guard_region_size * 2;
  ZX_DEBUG_ASSERT(guard_region_data_.size() == guard_region_copy_.size());
  for (uint32_t i = 0; i < 2; i++) {
    uint64_t start_offset = region.ptr->base;
    if (i == 1) {
      // Size includes guard regions.
      start_offset += size - guard_region_size;
    }
    CheckGuardRegion(region.name.c_str(), vmo_size, (i == 0), start_offset);
  }
}

void ContiguousPooledMemoryAllocator::CheckExternalGuardRegions() {
  size_t guard_region_size = guard_region_data_.size();
  if (!guard_region_size)
    return;
  ralloc_region_t regions[] = {{.base = 0, .size = guard_region_size},
                               {.base = size_ - guard_region_size, .size = guard_region_size}};
  for (size_t i = 0; i < std::size(regions); i++) {
    auto& region = regions[i];
    ZX_DEBUG_ASSERT(i < 2);
    ZX_DEBUG_ASSERT(region.size == guard_region_data_.size());
    CheckGuardRegion("External", 0, (i == 0), region.base);
  }
}

bool ContiguousPooledMemoryAllocator::is_ready() { return is_ready_; }

void ContiguousPooledMemoryAllocator::TraceObserverCallback(async_dispatcher_t* dispatcher,
                                                            async::WaitBase* wait,
                                                            zx_status_t status,
                                                            const zx_packet_signal_t* signal) {
  if (status != ZX_OK)
    return;
  trace_observer_event_.signal(ZX_EVENT_SIGNALED, 0);
  // We don't care if tracing was enabled or disabled - if the category is now disabled, the trace
  // will just be ignored anyway.
  TracePoolSize(true);

  trace_notify_observer_updated(trace_observer_event_.get());
  wait_.Begin(dispatcher);
}

void ContiguousPooledMemoryAllocator::CheckGuardPageCallback(async_dispatcher_t* dispatcher,
                                                             async::TaskBase* task,
                                                             zx_status_t status) {
  if (status != ZX_OK)
    return;
  // Ignore status - if the post fails, that means the driver is being shut down.
  guard_checker_.PostDelayed(dispatcher, kGuardCheckInterval);

  CheckExternalGuardRegions();

  if (!has_internal_guard_regions_)
    return;

  for (auto& region_data : regions_) {
    auto& region = region_data.second;
    CheckGuardRegionData(region);
  }
}

void ContiguousPooledMemoryAllocator::DumpPoolStats() {
  uint64_t unused_size = 0;
  uint64_t max_free_size = 0;
  region_allocator_.WalkAvailableRegions(
      [&unused_size, &max_free_size](const ralloc_region_t* r) -> bool {
        unused_size += r->size;
        max_free_size = std::max(max_free_size, r->size);
        return true;
      });

  LOG(INFO,
      "%s unused total: %ld bytes, max free size %ld bytes "
      "AllocatedRegionCount(): %zu AvailableRegionCount(): %zu",
      allocation_name_, unused_size, max_free_size, region_allocator_.AllocatedRegionCount(),
      region_allocator_.AvailableRegionCount());
  for (auto& [vmo, region] : regions_) {
    LOG(INFO, "Region koid %ld name %s size %zu", region.koid, region.name.c_str(),
        region.ptr->size);
  }
}

void ContiguousPooledMemoryAllocator::DumpPoolHighWaterMark() {
  LOG(INFO,
      "%s high_water_mark_used_size_: %ld bytes, max_free_size_at_high_water_mark_ %ld bytes "
      "(not including any failed allocations)",
      allocation_name_, high_water_mark_used_size_, max_free_size_at_high_water_mark_);
}

void ContiguousPooledMemoryAllocator::TracePoolSize(bool initial_trace) {
  uint64_t used_size = 0;
  region_allocator_.WalkAllocatedRegions([&used_size](const ralloc_region_t* r) -> bool {
    used_size += r->size;
    return true;
  });
  used_size_property_.Set(used_size);
  TRACE_COUNTER("gfx", "Contiguous pool size", pool_id_, "size", used_size);
  bool trace_high_water_mark = initial_trace;
  if (used_size > high_water_mark_used_size_) {
    high_water_mark_used_size_ = used_size;
    trace_high_water_mark = true;
    high_water_mark_property_.Set(high_water_mark_used_size_);
    free_at_high_water_mark_property_.Set(size_ - high_water_mark_used_size_);
    uint64_t max_free_size = 0;
    region_allocator_.WalkAvailableRegions([&max_free_size](const ralloc_region_t* r) -> bool {
      max_free_size = std::max(max_free_size, r->size);
      return true;
    });
    max_free_size_at_high_water_mark_ = max_free_size;
    max_free_at_high_water_property_.Set(max_free_size_at_high_water_mark_);
    // This can be a bit noisy at first, but then settles down quickly.
    DumpPoolHighWaterMark();
  }
  if (trace_high_water_mark) {
    TRACE_INSTANT("gfx", "Increased high water mark", TRACE_SCOPE_THREAD, "allocation_name",
                  allocation_name_, "size", high_water_mark_used_size_);
  }
}

uint64_t ContiguousPooledMemoryAllocator::GetVmoRegionOffsetForTest(const zx::vmo& vmo) {
  return regions_[vmo.get()].ptr->base + guard_region_data_.size();
}

}  // namespace sysmem_driver
