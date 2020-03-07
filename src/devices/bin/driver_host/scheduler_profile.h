// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_HOST_SCHEDULER_PROFILE_H_
#define SRC_DEVICES_BIN_DRIVER_HOST_SCHEDULER_PROFILE_H_

#include <zircon/types.h>

namespace internal {
zx_status_t connect_scheduler_profile_provider();
zx_status_t get_scheduler_profile(uint32_t priority, const char* name, zx_handle_t* profile);

zx_status_t get_scheduler_deadline_profile(uint64_t capacity, uint64_t deadline, uint64_t period,
                                           const char* name, zx_handle_t* profile);
}  // namespace internal

#endif  // SRC_DEVICES_BIN_DRIVER_HOST_SCHEDULER_PROFILE_H_
