// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "devhost-context.h"

#include <stdio.h>

#include <fbl/auto_lock.h>

#include "log.h"

namespace devmgr {

void DevhostContext::PushWorkItem(const fbl::RefPtr<zx_device_t>& dev, Callback callback) {
  auto work_item = std::make_unique<WorkItem>(dev, std::move(callback));

  fbl::AutoLock al(&lock_);
  work_items_.push_back(std::move(work_item));

  // TODO(surajmalhotra): Only signal if not being run in main devhost thread as a slight
  // optimization (assuming we will run work items before going back to waiting on the port).
  if (!event_waiter_->signaled()) {
    event_waiter_->signal();
  }
}

void DevhostContext::InternalRunWorkItems(size_t how_many_to_run) {
  {
    fbl::AutoLock al(&lock_);
    if (event_waiter_->signaled()) {
      event_waiter_->designal();
    }
  }

  size_t work_items_run = 0;
  auto keep_running = [&]() { return work_items_run < how_many_to_run || how_many_to_run == 0; };
  do {
    fbl::DoublyLinkedList<std::unique_ptr<WorkItem>> work_items;
    {
      fbl::AutoLock al(&lock_);
      work_items = std::move(work_items_);
    }

    if (work_items.is_empty()) {
      return;
    }

    std::unique_ptr<WorkItem> work_item;
    while (keep_running() && (work_item = work_items.pop_front())) {
      work_item->callback();
      work_items_run++;
    }

    if (!work_items.is_empty()) {
      fbl::AutoLock al(&lock_);
      work_items_.splice(work_items_.begin(), work_items);
    }
  } while (keep_running());

  fbl::AutoLock al(&lock_);
  if (!work_items_.is_empty() && !event_waiter_->signaled()) {
    event_waiter_->signal();
  }
}

void DevhostContext::RunWorkItems(size_t how_many_to_run) {
  std::unique_ptr<EventWaiter> event_waiter;
  {
    fbl::AutoLock al(&lock_);
    ZX_DEBUG_ASSERT(event_waiter_ != nullptr);
    if (work_items_.is_empty()) {
      return;
    }
    event_waiter = event_waiter_->Cancel();
  }

  InternalRunWorkItems(how_many_to_run);
  EventWaiter::BeginWait(std::move(event_waiter), loop_.dispatcher());
}

zx_status_t DevhostContext::SetupEventWaiter() {
  zx::event event;
  if (zx_status_t status = zx::event::create(0, &event); status != ZX_OK) {
    return status;
  }
  // TODO(surajmalhotra): Tune this value.
  constexpr uint32_t kBatchSize = 5;
  auto event_waiter = std::make_unique<EventWaiter>(std::move(event),
                                                    [this]() { InternalRunWorkItems(kBatchSize); });
  {
    fbl::AutoLock al(&lock_);
    event_waiter_ = event_waiter.get();
  }

  return EventWaiter::BeginWait(std::move(event_waiter), loop_.dispatcher());
}

void DevhostContext::EventWaiter::HandleEvent(std::unique_ptr<EventWaiter> event_waiter,
                                              async_dispatcher_t* dispatcher, async::WaitBase* wait,
                                              zx_status_t status,
                                              const zx_packet_signal_t* signal) {
  if (status != ZX_OK) {
    log(ERROR, "driver_host: event waiter error: %d\n", status);
    return;
  }

  if (signal->observed & ZX_USER_SIGNAL_0) {
    event_waiter->InvokeCallback();
    BeginWait(std::move(event_waiter), dispatcher);
  } else {
    printf("%s: invalid signals %x\n", __func__, signal->observed);
    abort();
  }
}

}  // namespace devmgr
