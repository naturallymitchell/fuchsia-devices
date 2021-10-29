// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_RUNTIME_CHANNEL_H_
#define SRC_DEVICES_BIN_DRIVER_RUNTIME_CHANNEL_H_

#include <lib/fdf/channel.h>
#include <lib/fdf/channel_read.h>

#include <optional>

#include <fbl/auto_lock.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>

#include "src/devices/bin/driver_runtime/callback_request.h"
#include "src/devices/bin/driver_runtime/message_packet.h"
#include "src/devices/bin/driver_runtime/object.h"

namespace driver_runtime {

// State shared between a pair of channels.
class FdfChannelSharedState : public fbl::RefCounted<FdfChannelSharedState> {
 public:
  FdfChannelSharedState() = default;
  ~FdfChannelSharedState() = default;

  fbl::Mutex* get_lock() { return &lock_; }

 private:
  fbl::Mutex lock_;
};

struct Channel : public Object {
 public:
  // fdf_channel_t implementation
  static fdf_status_t Create(uint32_t options, fdf_handle_t* out0, fdf_handle_t* out1);
  fdf_status_t Write(uint32_t options, fdf_arena_t* arena, void* data, uint32_t num_bytes,
                     zx_handle_t* handles, uint32_t num_handles);
  fdf_status_t Read(uint32_t options, fdf_arena_t** out_arena, void** out_data,
                    uint32_t* out_num_bytes, zx_handle_t** out_handles, uint32_t* out_num_handles);
  fdf_status_t WaitAsync(struct fdf_dispatcher* dispatcher, fdf_channel_read_t* channel_read,
                         uint32_t options);
  void Close();

 private:
  explicit Channel(fbl::RefPtr<FdfChannelSharedState> shared_state)
      : shared_state_(std::move(shared_state)),
        callback_request_(std::make_unique<driver_runtime::CallbackRequest>()),
        unowned_callback_request_(callback_request_.get()) {}

  // Stores a reference to |peer|. This reference will be cleared in |Close|.
  void Init(const fbl::RefPtr<Channel>& peer);

  // Takes ownership of the transferred |msg| and adds it to the |msg_queue|.
  // Queues a callback request with the dispatcher if not already queued.
  fdf_status_t WriteSelf(MessagePacketOwner msg);

  // Called when the other end of the channel is being closed.
  void OnPeerClosed();

  // Returns the callback request that can be queued with the dispatcher.
  // This will assert if the callback request is not available.
  // Use |IsCallbackRequestQueuedLocked| to check whether the callback
  // request has already been queued with the dispatcher.
  // |callback_reason| is the reason for queueing the callback request.
  std::unique_ptr<driver_runtime::CallbackRequest> TakeCallbackRequestLocked(
      fdf_status_t callback_reason) __TA_REQUIRES(get_lock());

  // Handles the callback from the dispatcher. Takes ownership of |callback_request|.
  void DispatcherCallback(std::unique_ptr<driver_runtime::CallbackRequest> callback_request,
                          fdf_status_t status);

  // Returns whether a read wait async request has been registered via |WaitAsync|,
  // and not yet completed i.e. the read callback has not completed yet.
  bool HasIncompleteWaitAsync() {
    fbl::AutoLock lock(get_lock());
    return IsWaitAsyncRegisteredLocked() || IsCallbackRequestQueuedLocked() || IsInCallbackLocked();
  }

  // Returns whether the callback request has been queued with the dispatcher.
  bool IsCallbackRequestQueuedLocked() __TA_REQUIRES(get_lock()) { return !callback_request_; }
  // Returns whether a read wait async request has been registered via |WaitAsync|.
  bool IsWaitAsyncRegisteredLocked() __TA_REQUIRES(get_lock()) { return !!dispatcher_; }
  // Whether the channel is currently calling a read callback.
  bool IsInCallbackLocked() __TA_REQUIRES(get_lock()) { return num_pending_callbacks_ > 0; }

  // Returns the lock shared between the channels.
  fbl::Mutex* get_lock() { return shared_state_->get_lock(); }

  // This cannot be locked as it holds the shared lock.
  const fbl::RefPtr<FdfChannelSharedState> shared_state_;
  // The other end of the channel.
  fbl::RefPtr<Channel> peer_ __TA_GUARDED(get_lock());

  // Callback request that can be queued with the dispatcher.
  // Only one pending callback per end of the channel is supported at a time.
  std::unique_ptr<driver_runtime::CallbackRequest> callback_request_ __TA_GUARDED(get_lock());
  // Used for canceling a queued callback request.
  CallbackRequest* const unowned_callback_request_ __TA_GUARDED(get_lock());

  // This could be potentially be greater than 1 if the user registers a new callback from within
  // a callback, and a new callback is called from a different thread.
  uint32_t num_pending_callbacks_ __TA_GUARDED(get_lock()) = 0;

  // Messages written to this end of the channel.
  fbl::DoublyLinkedList<MessagePacketOwner> msg_queue_ __TA_GUARDED(get_lock());

  // Dispatcher and channel_read registered via |WaitAsync|.
  // These are cleared before calling a read callback.
  fdf_dispatcher_t* dispatcher_ __TA_GUARDED(get_lock()) = nullptr;
  fdf_channel_read_t* channel_read_ __TA_GUARDED(get_lock()) = nullptr;
};

}  // namespace driver_runtime

#endif  //  SRC_DEVICES_BIN_DRIVER_RUNTIME_CHANNEL_H_