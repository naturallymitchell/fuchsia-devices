// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "allocator.h"

#include <lib/fidl/internal.h>
#include <lib/zx/channel.h>
#include <lib/zx/event.h>
#include <zircon/fidl.h>

#include <ddk/trace/event.h>

#include "logical_buffer_collection.h"

namespace sysmem_driver {

Allocator::Allocator(Device* parent_device)
    : LoggingMixin("allocator"), parent_device_(parent_device) {
  // nothing else to do here
}

Allocator::~Allocator() { LogInfo("~Allocator"); }

// static
void Allocator::CreateChannelOwned(zx::channel request, Device* device) {
  auto allocator = std::unique_ptr<Allocator>(new Allocator(device));
  // Ignore the result - allocator will be destroyed and the channel will be closed on error.
  fidl::BindServer(device->dispatcher(), std::move(request), std::move(allocator));
}

void Allocator::AllocateNonSharedCollection(zx::channel buffer_collection_request,
                                            AllocateNonSharedCollectionCompleter::Sync& completer) {
  TRACE_DURATION("gfx", "Allocator::AllocateNonSharedCollection");

  // The AllocateCollection() message skips past the token stage because the
  // client is also the only participant (probably a temp/test client).  Real
  // clients are encouraged to use AllocateSharedCollection() instead, so that
  // the client can share the LogicalBufferCollection with other participants.
  //
  // Because this is a degenerate way to use sysmem, we implement this method
  // in terms of the non-degenerate way.
  //
  // This code is essentially the same as what a client would do if a client
  // wanted to skip the BufferCollectionToken stage without using
  // AllocateCollection().  Essentially, this code is here just so clients
  // that don't need to share their collection don't have to write this code,
  // and can share this code instead.

  // Create a local token.
  zx::channel token_client;
  zx::channel token_server;
  zx_status_t status = zx::channel::create(0, &token_client, &token_server);
  if (status != ZX_OK) {
    LogError(
        "Allocator::AllocateCollection() zx::channel::create() failed "
        "- status: %d",
        status);
    // ~buffer_collection_request
    //
    // Returning an error here causes the sysmem connection to drop also,
    // which seems like a good idea (more likely to recover overall) given
    // the nature of the error.
    completer.Close(status);
    return;
  }

  // The server end of the local token goes to Create(), and the client end
  // goes to BindSharedCollection().  The BindSharedCollection() will figure
  // out which token we're talking about based on the koid(s), as usual.
  LogicalBufferCollection::Create(std::move(token_server), parent_device_);
  LogicalBufferCollection::BindSharedCollection(parent_device_, std::move(token_client),
                                                std::move(buffer_collection_request),
                                                client_info_ ? &*client_info_ : nullptr);

  // Now the client can SetConstraints() on the BufferCollection, etc.  The
  // client didn't have to hassle with the BufferCollectionToken, which is the
  // sole upside of the client using this message over
  // AllocateSharedCollection().
}

void Allocator::AllocateSharedCollection(zx::channel token_request,
                                         AllocateSharedCollectionCompleter::Sync& completer) {
  TRACE_DURATION("gfx", "Allocator::AllocateSharedCollection");

  // The LogicalBufferCollection is self-owned / owned by all the channels it
  // serves.
  //
  // There's no channel served directly by the LogicalBufferCollection.
  // Instead LogicalBufferCollection owns all the FidlServer instances that
  // each own a channel.
  //
  // Initially there's only a channel to the first BufferCollectionToken.  We
  // go ahead and allocate the LogicalBufferCollection here since the
  // LogicalBufferCollection associates all the BufferCollectionToken and
  // BufferCollection bindings to the same LogicalBufferCollection.
  LogicalBufferCollection::Create(std::move(token_request), parent_device_);
}

void Allocator::BindSharedCollection(zx::channel token, zx::channel buffer_collection_request,
                                     BindSharedCollectionCompleter::Sync& completer) {
  TRACE_DURATION("gfx", "Allocator::BindSharedCollection");

  // The BindSharedCollection() message is about a supposed-to-be-pre-existing
  // logical BufferCollection, but the only association we have to that
  // BufferCollection is the client end of a BufferCollectionToken channel
  // being handed in via token_param.  To find any associated BufferCollection
  // we have to look it up by koid.  The koid table is held by
  // BufferCollection, so delegate over to BufferCollection for this request.
  LogicalBufferCollection::BindSharedCollection(parent_device_, std::move(token),
                                                std::move(buffer_collection_request),
                                                client_info_ ? &*client_info_ : nullptr);
}

void Allocator::ValidateBufferCollectionToken(
    zx_koid_t token_server_koid, ValidateBufferCollectionTokenCompleter::Sync& completer) {
  zx_status_t status =
      LogicalBufferCollection::ValidateBufferCollectionToken(parent_device_, token_server_koid);
  ZX_DEBUG_ASSERT(status == ZX_OK || status == ZX_ERR_NOT_FOUND);
  completer.Reply(status == ZX_OK);
}

void Allocator::SetDebugClientInfo(fidl::StringView name, uint64_t id,
                                   SetDebugClientInfoCompleter::Sync& completer) {
  client_info_.emplace();
  client_info_->name = std::string(name.begin(), name.end());
  client_info_->id = id;
}

}  // namespace sysmem_driver
