// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DRIVER_FRAMEWORK_DEVCOORDINATOR_SUSPEND_TASK_H_
#define SRC_DRIVER_FRAMEWORK_DEVCOORDINATOR_SUSPEND_TASK_H_

#include "device.h"
#include "task.h"

namespace devmgr {

class SuspendTask final : public Task {
 public:
  static fbl::RefPtr<SuspendTask> Create(fbl::RefPtr<Device> device, uint32_t flags,
                                         Completion completion = nullptr);

  // Don/t invoke this, use Create
  SuspendTask(fbl::RefPtr<Device> device, uint32_t flags, Completion completion);

  uint32_t suspend_flags() { return flags_; }

  ~SuspendTask() final;

  const Device& device() const { return *device_; }

 private:
  void Run() final;

  // The device being suspended
  fbl::RefPtr<Device> device_;
  // The target suspend flags
  uint32_t flags_;
};

}  // namespace devmgr

#endif  // SRC_DRIVER_FRAMEWORK_DEVCOORDINATOR_SUSPEND_TASK_H_
