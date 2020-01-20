// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_COORDINATOR_INIT_TASK_H_
#define SRC_DEVICES_COORDINATOR_INIT_TASK_H_

#include "device.h"
#include "task.h"

namespace devmgr {

// This is used for sending |Init| requests.
class InitTask final : public Task {
 public:
  static fbl::RefPtr<InitTask> Create(fbl::RefPtr<Device> device, Completion completion = nullptr);

  // Don't invoke this, use Create
  InitTask(fbl::RefPtr<Device> device, Completion completion);

  ~InitTask() final;

 private:
  void Run() final;

  fbl::RefPtr<Device> device_;
};

}  // namespace devmgr

#endif  // SRC_DEVICES_COORDINATOR_INIT_TASK_H_
