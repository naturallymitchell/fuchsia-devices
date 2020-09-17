// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_LOGICAL_BUFFER_COLLECTION_H_
#define SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_LOGICAL_BUFFER_COLLECTION_H_

#include <fuchsia/sysmem/c/fidl.h>
#include <fuchsia/sysmem/llcpp/fidl.h>
#include <fuchsia/sysmem2/llcpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/fidl-async-2/fidl_struct.h>
#include <lib/fidl/llcpp/heap_allocator.h>
#include <lib/zx/channel.h>

#include <list>
#include <map>
#include <memory>

#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>

#include "device.h"

namespace sysmem_driver {

class BufferCollectionToken;
class BufferCollection;
class MemoryAllocator;
class LogicalBufferCollection : public fbl::RefCounted<LogicalBufferCollection> {
 public:
  struct ClientInfo {
    std::string name;
    zx_koid_t id;
  };

  // In sysmem_tests, the max needed was observed to be 12400 bytes, so if we wanted to avoid heap
  // for allocating FIDL table fields, 32KiB would likely be enough most of the time.  However, the
  // difference isn't even reliably measurable sign out of the ~410us +/- ~10us  it takes to
  // allocate a collection with only 4KiB buffer space total on astro.
  //
  // The time cost of zeroing VMO buffer space is far higher than anything controlled by this
  // number, so it'd be more fruitful to focus there before here.
  static constexpr size_t kBufferThenHeapAllocatorSize = 1;
  using FidlAllocator = fidl::BufferThenHeapAllocator<kBufferThenHeapAllocatorSize>;
  using CollectionMap = std::map<BufferCollection*, std::unique_ptr<BufferCollection>>;

  ~LogicalBufferCollection();

  static void Create(zx::channel buffer_collection_token_request, Device* parent_device);

  // |parent_device| the Device* that the calling allocator is part of.  The
  // tokens_by_koid_ for each Device is separate.  If somehow two clients were
  // to get connected to two separate sysmem device instances hosted in the
  // same devhost, those clients (intentionally) won't be able to share a
  // LogicalBufferCollection.
  //
  // |buffer_collection_token| the client end of the BufferCollectionToken
  // being turned in by the client to get a BufferCollection in exchange.
  //
  // |buffer_collection_request| the server end of a BufferCollection channel
  // to be served by the LogicalBufferCollection associated with
  // buffer_collection_token.
  static void BindSharedCollection(Device* parent_device, zx::channel buffer_collection_token,
                                   zx::channel buffer_collection_request,
                                   const ClientInfo* client_info);

  // ZX_OK if the token is known to the server.
  // ZX_ERR_NOT_FOUND if the token isn't known to the server.
  static zx_status_t ValidateBufferCollectionToken(Device* parent_device,
                                                   zx_koid_t token_server_koid);

  // This is used to create the initial BufferCollectionToken, and also used
  // by BufferCollectionToken::Duplicate().
  //
  // The |self| parameter exists only because LogicalBufferCollection can't
  // hold a std::weak_ptr<> to itself because that requires libc++ (the binary
  // not just the headers) which isn't available in Zircon so far.
  void CreateBufferCollectionToken(fbl::RefPtr<LogicalBufferCollection> self,
                                   uint32_t rights_attenuation_mask,
                                   zx::channel buffer_collection_token_request);

  void OnSetConstraints();

  void SetName(uint32_t priority, std::string name);
  void SetDebugTimeoutLogDeadline(int64_t deadline);

  void LogClientError(const ClientInfo* client_info, const char* format, ...) __PRINTFLIKE(3, 4);
  void VLogClientError(const ClientInfo* client_info, const char* format, va_list args);

  struct AllocationResult {
    const llcpp::fuchsia::sysmem2::BufferCollectionInfo* buffer_collection_info = nullptr;
    const zx_status_t status = ZX_OK;
  };
  AllocationResult allocation_result();

  Device* parent_device() const { return parent_device_; }

  const CollectionMap& collection_views() const { return collection_views_; }

  // The returned allocator& lasts as long as the LogicalBufferCollection, which is long enough
  // for child BufferCollection(s) to use the allocator.
  FidlAllocator& fidl_allocator() { return allocator_; }

  inspect::Node& node() { return node_; }

 private:
  enum class CheckSanitizeStage { kInitial, kNotAggregated, kAggregated };

  struct Constraints {
    Constraints(const Constraints&) = delete;
    Constraints(Constraints&&) = default;
    Constraints(llcpp::fuchsia::sysmem2::BufferCollectionConstraints::Builder&& builder,
                ClientInfo&& client)
        : builder(std::move(builder)), client(std::move(client)) {}

    llcpp::fuchsia::sysmem2::BufferCollectionConstraints::Builder builder;
    ClientInfo client;
  };

  LogicalBufferCollection(Device* parent_device);

  // If |format| is nonnull, will log an error. This also cleans out a lot of
  // state that's unnecessary after a failure.
  void Fail(const char* format, ...);

  static void LogInfo(const char* format, ...);
  static void LogErrorStatic(const char* format, ...) __PRINTFLIKE(1, 2);

  // Uses the implicit |current_client_info_| to identify which client has an error.
  void LogError(const char* format, ...) __PRINTFLIKE(2, 3);
  void VLogError(const char* format, va_list args);

  void MaybeAllocate();

  void TryAllocate();

  void SetFailedAllocationResult(zx_status_t status);

  void SetAllocationResult(llcpp::fuchsia::sysmem2::BufferCollectionInfo&& info);

  void SendAllocationResult();

  void BindSharedCollectionInternal(BufferCollectionToken* token,
                                    zx::channel buffer_collection_request);

  // To be called only by CombineConstraints().
  bool IsMinBufferSizeSpecifiedByAnyParticipant();

  bool CombineConstraints();

  bool CheckSanitizeBufferCollectionConstraints(
      CheckSanitizeStage stage,
      llcpp::fuchsia::sysmem2::BufferCollectionConstraints::Builder* constraints);

  bool CheckSanitizeBufferUsage(CheckSanitizeStage stage,
                                llcpp::fuchsia::sysmem2::BufferUsage::Builder* buffer_usage);

  bool CheckSanitizeBufferMemoryConstraints(
      CheckSanitizeStage stage, const llcpp::fuchsia::sysmem2::BufferUsage& buffer_usage,
      llcpp::fuchsia::sysmem2::BufferMemoryConstraints::Builder* constraints);

  bool CheckSanitizeImageFormatConstraints(
      CheckSanitizeStage stage,
      llcpp::fuchsia::sysmem2::ImageFormatConstraints::Builder* constraints);

  bool AccumulateConstraintBufferCollection(
      llcpp::fuchsia::sysmem2::BufferCollectionConstraints::Builder* acc,
      llcpp::fuchsia::sysmem2::BufferCollectionConstraints* c);

  bool AccumulateConstraintsBufferUsage(llcpp::fuchsia::sysmem2::BufferUsage::Builder* acc,
                                        llcpp::fuchsia::sysmem2::BufferUsage* c);

  bool AccumulateConstraintHeapPermitted(fidl::VectorView<llcpp::fuchsia::sysmem2::HeapType>* acc,
                                         fidl::VectorView<llcpp::fuchsia::sysmem2::HeapType>* c);

  bool AccumulateConstraintBufferMemory(
      llcpp::fuchsia::sysmem2::BufferMemoryConstraints::Builder* acc,
      llcpp::fuchsia::sysmem2::BufferMemoryConstraints* c);

  bool AccumulateConstraintImageFormats(
      fidl::VectorView<llcpp::fuchsia::sysmem2::ImageFormatConstraints::Builder>* acc,
      fidl::VectorView<llcpp::fuchsia::sysmem2::ImageFormatConstraints>* c);

  bool AccumulateConstraintImageFormat(
      llcpp::fuchsia::sysmem2::ImageFormatConstraints::Builder* acc,
      llcpp::fuchsia::sysmem2::ImageFormatConstraints* c);

  bool AccumulateConstraintColorSpaces(
      fidl::VectorView<llcpp::fuchsia::sysmem2::ColorSpace::Builder>* acc,
      fidl::VectorView<llcpp::fuchsia::sysmem2::ColorSpace>* c);

  size_t InitialCapacityOrZero(CheckSanitizeStage stage, size_t initial_capacity);

  bool IsColorSpaceEqual(const llcpp::fuchsia::sysmem2::ColorSpace::Builder& a,
                         const llcpp::fuchsia::sysmem2::ColorSpace& b);

  fit::result<llcpp::fuchsia::sysmem2::BufferCollectionInfo, zx_status_t> Allocate();

  fit::result<zx::vmo> AllocateVmo(
      MemoryAllocator* allocator,
      const llcpp::fuchsia::sysmem2::SingleBufferSettings::Builder& settings, uint32_t index);

  int32_t CompareImageFormatConstraintsTieBreaker(
      const llcpp::fuchsia::sysmem2::ImageFormatConstraints& a,
      const llcpp::fuchsia::sysmem2::ImageFormatConstraints& b);

  int32_t CompareImageFormatConstraintsByIndex(uint32_t index_a, uint32_t index_b);
  void CreationTimedOut(async_dispatcher_t* dispatcher, async::TaskBase* task, zx_status_t status);

  Device* parent_device_ = nullptr;

  FidlAllocator allocator_;

  using TokenMap = std::map<BufferCollectionToken*, std::unique_ptr<BufferCollectionToken>>;
  TokenMap token_views_;

  CollectionMap collection_views_;

  using ConstraintsList = std::list<Constraints>;
  ConstraintsList constraints_list_;

  bool is_allocate_attempted_ = false;

  std::optional<llcpp::fuchsia::sysmem2::BufferCollectionConstraints::Builder> constraints_;

  // Iff true, initial allocation has been attempted and has succeeded or
  // failed.  Both allocation_result_status_ and allocation_result_info_ are
  // not meaningful until has_allocation_result_ is true.
  bool has_allocation_result_ = false;
  zx_status_t allocation_result_status_ = ZX_OK;
  std::optional<llcpp::fuchsia::sysmem2::BufferCollectionInfo> allocation_result_info_;

  MemoryAllocator* memory_allocator_ = nullptr;
  std::optional<std::pair<uint32_t /*priority*/, std::string>> name_;

  // Information about the current client - only valid while aggregating state for a particular
  // client.
  ClientInfo* current_client_info_ = nullptr;

  // We keep LogicalBufferCollection alive as long as there are child VMOs
  // outstanding (no revoking of child VMOs for now).
  //
  // This tracking is for the benefit of MemoryAllocator sub-classes that need
  // a Delete() call, such as to clean up a slab allocation and/or to inform
  // an external allocator of delete.
  class TrackedParentVmo {
   public:
    using DoDelete = fit::callback<void(TrackedParentVmo* parent)>;
    // The do_delete callback will be invoked upon the sooner of (A) the client
    // code causing ~ParentVmo, or (B) ZX_VMO_ZERO_CHILDREN occurring async
    // after StartWait() is called.
    TrackedParentVmo(fbl::RefPtr<LogicalBufferCollection> buffer_collection, zx::vmo vmo,
                     DoDelete do_delete);
    ~TrackedParentVmo();

    // This should only be called after client code has created a child VMO, and
    // will begin the wait for ZX_VMO_ZERO_CHILDREN.
    zx_status_t StartWait(async_dispatcher_t* dispatcher);

    // Cancel the wait. This should only be used by LogicalBufferCollection
    zx_status_t CancelWait();

    zx::vmo TakeVmo();
    [[nodiscard]] const zx::vmo& vmo() const;

    void set_child_koid(zx_koid_t koid) { child_koid_ = koid; }

    TrackedParentVmo(const TrackedParentVmo&) = delete;
    TrackedParentVmo(TrackedParentVmo&&) = delete;
    TrackedParentVmo& operator=(const TrackedParentVmo&) = delete;
    TrackedParentVmo& operator=(TrackedParentVmo&&) = delete;

   private:
    void OnZeroChildren(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                        const zx_packet_signal_t* signal);
    fbl::RefPtr<LogicalBufferCollection> buffer_collection_;
    zx::vmo vmo_;
    zx_koid_t child_koid_{};
    DoDelete do_delete_;
    async::WaitMethod<TrackedParentVmo, &TrackedParentVmo::OnZeroChildren> zero_children_wait_;
    // Only for asserts:
    bool waiting_ = {};
  };
  using ParentVmoMap = std::map<zx_handle_t, std::unique_ptr<TrackedParentVmo>>;
  ParentVmoMap parent_vmos_;
  async::TaskMethod<LogicalBufferCollection, &LogicalBufferCollection::CreationTimedOut>
      creation_timer_{this};

  inspect::Node node_;
  inspect::StringProperty name_property_;
  inspect::UintProperty vmo_count_property_;
  inspect::ValueList vmo_properties_;
};

}  // namespace sysmem_driver

#endif  // SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_LOGICAL_BUFFER_COLLECTION_H_
