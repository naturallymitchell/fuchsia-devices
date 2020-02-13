// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_LOGGING_H_
#define SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_LOGGING_H_

#include <stdarg.h>

namespace sysmem_driver {

void vLog(bool is_error, const char* prefix1, const char* prefix2, const char* format,
          va_list args);

}  // namespace sysmem_driver

#endif  // SRC_DEVICES_SYSMEM_DRIVERS_SYSMEM_LOGGING_H_
