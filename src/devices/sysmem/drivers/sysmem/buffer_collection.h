// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_BUFFER_COLLECTION_H_
#define SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_BUFFER_COLLECTION_H_

#include <fuchsia/sysmem/c/fidl.h>
#include <fuchsia/sysmem/llcpp/fidl.h>
#include <fuchsia/sysmem2/llcpp/fidl.h>
#include <lib/fidl/internal.h>
#include <lib/fidl/llcpp/heap_allocator.h>

#include <list>

#include "logging.h"
#include "logical_buffer_collection.h"
#include "node.h"
#include "src/devices/sysmem/drivers/sysmem/device.h"

namespace sysmem_driver {

class BufferCollection : public Node, public fuchsia_sysmem::BufferCollection::Interface {
 public:
  // Use EmplaceInTree() instead of Create() (until we switch to llcpp when we can have a new
  // Create() that does what EmplaceInTree() currently does).  The returned reference is valid while
  // this Node is in the tree under root_.
  static BufferCollection& EmplaceInTree(
      fbl::RefPtr<LogicalBufferCollection> logical_buffer_collection, BufferCollectionToken* token);
  ~BufferCollection() override;

  void SetErrorHandler(fit::function<void(zx_status_t)> error_handler) {
    error_handler_ = std::move(error_handler);
  }
  void Bind(zx::channel channel);

  //
  // fuchsia.sysmem.BufferCollection interface methods
  //

  void SetEventSink(
      fidl::ClientEnd<fuchsia_sysmem::BufferCollectionEvents> buffer_collection_events_client,
      SetEventSinkCompleter::Sync& completer) override;
  void Sync(SyncCompleter::Sync& completer) override;
  void SetConstraints(bool has_constraints,
                      fuchsia_sysmem::wire::BufferCollectionConstraints constraints,
                      SetConstraintsCompleter::Sync& completer) override;
  void WaitForBuffersAllocated(WaitForBuffersAllocatedCompleter::Sync& completer) override;
  void CheckBuffersAllocated(CheckBuffersAllocatedCompleter::Sync& completer) override;
  void CloseSingleBuffer(uint64_t buffer_index,
                         CloseSingleBufferCompleter::Sync& completer) override;
  void AllocateSingleBuffer(uint64_t buffer_index,
                            AllocateSingleBufferCompleter::Sync& completer) override;
  void WaitForSingleBufferAllocated(
      uint64_t buffer_index, WaitForSingleBufferAllocatedCompleter::Sync& completer) override;
  void CheckSingleBufferAllocated(uint64_t buffer_index,
                                  CheckSingleBufferAllocatedCompleter::Sync& completer) override;
  void Close(CloseCompleter::Sync& completer) override;
  void SetName(uint32_t priority, fidl::StringView name,
               SetNameCompleter::Sync& completer) override;
  void SetDebugClientInfo(fidl::StringView name, uint64_t id,
                          SetDebugClientInfoCompleter::Sync& completer) override;
  void SetConstraintsAuxBuffers(
      fuchsia_sysmem::wire::BufferCollectionConstraintsAuxBuffers constraints_aux_buffers,
      SetConstraintsAuxBuffersCompleter::Sync& completer) override;
  void GetAuxBuffers(GetAuxBuffersCompleter::Sync& completer) override;
  void AttachToken(uint32_t rights_attenuation_mask,
                   fidl::ServerEnd<fuchsia_sysmem::BufferCollectionToken> token_request,
                   AttachTokenCompleter::Sync& completer) override;

  //
  // LogicalBufferCollection uses these:
  //

  bool is_set_constraints_seen() const { return is_set_constraints_seen_; }
  bool has_constraints();

  // has_constraints() must be true to call this.
  const fuchsia_sysmem2::wire::BufferCollectionConstraints& constraints();

  // has_constraints() must be true to call this, and will stay true after calling this.
  fuchsia_sysmem2::wire::BufferCollectionConstraints CloneConstraints();

  fbl::RefPtr<LogicalBufferCollection> logical_buffer_collection_shared();

  bool is_done() const;

  bool should_propagate_failure_to_parent_node() const;

  // Node interface
  bool ReadyForAllocation() override;
  void OnBuffersAllocated(const AllocationResult& allocation_result) override;
  void Fail(zx_status_t epitaph) override;
  BufferCollectionToken* buffer_collection_token() override;
  const BufferCollectionToken* buffer_collection_token() const override;
  BufferCollection* buffer_collection() override;
  const BufferCollection* buffer_collection() const override;
  OrphanedNode* orphaned_node() override;
  const OrphanedNode* orphaned_node() const override;
  bool is_connected() const override;

 private:
  using V1CBufferCollectionInfo = FidlStruct<fuchsia_sysmem_BufferCollectionInfo_2,
                                             fuchsia_sysmem::wire::BufferCollectionInfo_2>;

  friend class FidlServer;

  explicit BufferCollection(fbl::RefPtr<LogicalBufferCollection> logical_buffer_collection,
                            const BufferCollectionToken& token);

  void CloseChannel(zx_status_t epitaph);

  void SetDebugClientInfoInternal(std::string name, uint64_t id);

  // The rights attenuation mask driven by usage, so that read-only usage
  // doesn't get write, etc.
  uint32_t GetUsageBasedRightsAttenuation();

  uint32_t GetClientVmoRights();
  uint32_t GetClientAuxVmoRights();
  void MaybeCompleteWaitForBuffersAllocated();

  void FailAsync(Location location, zx_status_t status, const char* format, ...) __PRINTFLIKE(4, 5);
  // FailSync must be used instead of FailAsync if the current method has a completer that needs a
  // reply.
  template <typename Completer>
  void FailSync(Location location, Completer& completer, zx_status_t status, const char* format,
                ...) __PRINTFLIKE(5, 6);

  fit::result<fuchsia_sysmem2::wire::BufferCollectionInfo> CloneResultForSendingV2(
      const fuchsia_sysmem2::wire::BufferCollectionInfo& buffer_collection_info);

  fit::result<fuchsia_sysmem::wire::BufferCollectionInfo_2> CloneResultForSendingV1(
      const fuchsia_sysmem2::wire::BufferCollectionInfo& buffer_collection_info);
  fit::result<fuchsia_sysmem::wire::BufferCollectionInfo_2> CloneAuxBuffersResultForSendingV1(
      const fuchsia_sysmem2::wire::BufferCollectionInfo& buffer_collection_info);

  static const fuchsia_sysmem_BufferCollection_ops_t kOps;

  std::optional<zx_status_t> async_failure_result_;
  fit::function<void(zx_status_t)> error_handler_;

  // Cached from LogicalBufferCollection.
  TableSet& table_set_;

  // Client end of a BufferCollectionEvents channel, for the local server to
  // send events to the remote client.  All of the messages in this interface
  // are one-way with no response, so sending an event doesn't block the
  // server thread.
  //
  // This may remain non-set if SetEventSink() is never used by a client.  A
  // client may send SetEventSink() up to once.
  //
  std::optional<fuchsia_sysmem::BufferCollectionEvents::SyncClient> events_;

  // Temporarily holds fuchsia.sysmem.BufferCollectionConstraintsAuxBuffers until SetConstraints()
  // arrives.
  std::optional<TableHolder<fuchsia_sysmem::wire::BufferCollectionConstraintsAuxBuffers>>
      constraints_aux_buffers_;

  // FIDL protocol enforcement.
  bool is_set_constraints_seen_ = false;
  bool is_set_constraints_aux_buffers_seen_ = false;

  std::list<std::pair</*async_id*/ uint64_t, WaitForBuffersAllocatedCompleter::Async>>
      pending_wait_for_buffers_allocated_;

  bool is_done_ = false;

  std::optional<fidl::ServerBindingRef<fuchsia_sysmem::BufferCollection>> server_binding_;

  // Becomes set when OnBuffersAllocated() is called, and stays set after that.
  std::optional<AllocationResult> logical_allocation_result_;

  inspect::Node inspect_node_;
  inspect::UintProperty debug_id_property_;
  inspect::StringProperty debug_name_property_;
  inspect::ValueList properties_;
};

}  // namespace sysmem_driver

#endif  // SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_BUFFER_COLLECTION_H_
