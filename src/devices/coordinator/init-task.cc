// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "init-task.h"

#include "coordinator.h"
#include "log.h"

namespace devmgr {

InitTask::InitTask(fbl::RefPtr<Device> device, Completion completion)
    : Task(device->coordinator->dispatcher(), std::move(completion)), device_(std::move(device)) {}

InitTask::~InitTask() = default;

fbl::RefPtr<InitTask> InitTask::Create(fbl::RefPtr<Device> device, Completion completion) {
  return fbl::MakeRefCounted<InitTask>(std::move(device), std::move(completion));
}

void InitTask::Run() {
  log(TRACE, "running init task for %s\n", device_->name().data());

  // If the init task exists for a device, it should always run before
  // other tasks for a device.
  ZX_ASSERT(device_->state() == Device::State::kInitializing);

  // Composite and proxy devices do not implement init hooks or use init tasks.
  // If the parent is a composite device, we do not need to wait on any init task,
  // as composite devices are not created until all its component devices have finished
  // initializing.
  // If the parent is a proxy device, it is sufficient to wait on the init task of the
  // stored real parent (parent of the proxy device).
  ZX_ASSERT(!device_->composite());
  ZX_ASSERT(!(device_->flags & DEV_CTX_PROXY));

  if (device_->parent() && device_->parent()->state() == Device::State::kInitializing) {
    AddDependency(device_->parent()->GetActiveInit());
    return;
  }

  auto completion = [this](zx_status_t status) {
    // Only update the device state if we are not being forcibly removed.
    if (device_->state() != Device::State::kDead) {
      device_->set_state(Device::State::kActive);
    }
    if (status == ZX_OK) {
      if (!device_->wait_make_visible()) {
        status = device_->coordinator->MakeVisible(device_);
      }
    } else if (device_->state() != Device::State::kDead) {
      log(ERROR, "%s: init task failed, err: %d, scheduling removal of device\n",
          device_->name().data(), status);
      device_->coordinator->ScheduleDevhostRequestedRemove(device_, true /* do_unbind */);
    }
    // We still want other tasks to run even if init failed, so do not propagate errors.
    // If a driver adds multiple devices, it is possible that init tasks are scheduled
    // for both a parent and child. We will still run the child init task if the parent
    // init task fails, as drivers expect init to always run before unbind.
    // TODO(jocelyndang): consider providing the parent init failure to the child init hook.
    Complete(ZX_OK);
  };

  zx_status_t status = ZX_OK;
  if (device_->host() != nullptr) {
    status = device_->SendInit(std::move(completion));
    if (status == ZX_OK) {
      // Sent the init request, the devhost will call our completion when ready.
      return;
    }
  }
  // No init request sent, need to call the completion now.
  completion(status);
  // Since the device didn't successfully send an CompleteInit request, it will not
  // drop our init task reference. We need to drop it now.
  device_->DropInitTask();
}

}  // namespace devmgr
