// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/spawn.h>
#include <lib/zx/process.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>

#include <zxtest/zxtest.h>

TEST(lsusb, DoesNotCrashOrHang) {
  zx::process process;
  const char* kArgs[] = {"/pkg/bin/lsusb", "-debug", nullptr};
  ASSERT_OK(fdio_spawn(0, FDIO_SPAWN_CLONE_ALL, "/pkg/bin/lsusb", kArgs,
                       process.reset_and_get_address()));
  process.wait_one(ZX_PROCESS_TERMINATED, zx::time::infinite(), nullptr);
  zx_info_process_t info;
  size_t actual = sizeof(info);
  size_t avail = sizeof(info);
  ASSERT_OK(process.get_info(ZX_INFO_PROCESS, &info, sizeof(info), &actual, &avail));
  ASSERT_TRUE(info.exited);
  ASSERT_EQ(info.return_code, 0);
}
