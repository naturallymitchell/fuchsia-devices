// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_ALLOCATOR_H_
#define SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_ALLOCATOR_H_

#include <fuchsia/sysmem/llcpp/fidl.h>
#include <lib/zx/channel.h>

#include "device.h"
#include "logging.h"
#include "logical_buffer_collection.h"

namespace sysmem_driver {

// An instance of this class serves an Allocator connection.  The lifetime of
// the instance is 1:1 with the Allocator channel.
//
// Because Allocator is essentially self-contained and handling the server end
// of a channel, most of Allocator is private.
class Allocator : public fuchsia_sysmem::Allocator::RawChannelInterface, public LoggingMixin {
 public:
  // Public for std::unique_ptr<Allocator>:
  ~Allocator();

  static void CreateChannelOwned(zx::channel request, Device* device);

 private:
  Allocator(Device* parent_device);

  void AllocateNonSharedCollection(zx::channel buffer_collection_request,
                                   AllocateNonSharedCollectionCompleter::Sync& completer) override;
  void AllocateSharedCollection(zx::channel token_request,
                                AllocateSharedCollectionCompleter::Sync& completer) override;
  void BindSharedCollection(zx::channel token, zx::channel buffer_collection,
                            BindSharedCollectionCompleter::Sync& completer) override;
  void ValidateBufferCollectionToken(
      zx_koid_t token_server_koid,
      ValidateBufferCollectionTokenCompleter::Sync& completer) override;
  void SetDebugClientInfo(fidl::StringView name, uint64_t id,
                          SetDebugClientInfoCompleter::Sync& completer) override;

  Device* parent_device_ = nullptr;

  std::optional<LogicalBufferCollection::ClientInfo> client_info_;
};

}  // namespace sysmem_driver

#endif  // SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_ALLOCATOR_H_
