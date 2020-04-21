// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "driver_host.h"

#include <lib/async-loop/default.h>

#include <zxtest/zxtest.h>

#include "driver_host_context.h"
#include "zx_device.h"

namespace {

TEST(DriverHostTest, MkDevpath) {
  DriverHostContext ctx(&kAsyncLoopConfigNoAttachToCurrentThread);

  fbl::RefPtr<zx_device> dev;
  ASSERT_OK(zx_device::Create(&ctx, &dev));

  constexpr char device_name[] = "device-name";
  strlcpy(dev->name, device_name, sizeof(dev->name));

  const char* result = mkdevpath(nullptr, nullptr, 0);
  EXPECT_STR_EQ("", result);

  result = mkdevpath(dev, nullptr, 0);
  EXPECT_STR_EQ("", result);

  std::vector<char> buf;
  result = mkdevpath(dev, buf.data(), buf.size());
  EXPECT_STR_EQ("", result);

  buf.resize(sizeof(device_name));
  result = mkdevpath(dev, buf.data(), buf.size());
  EXPECT_STR_EQ(device_name, result);

  buf.resize(sizeof(device_name) * 2);
  result = mkdevpath(dev, buf.data(), buf.size());
  EXPECT_STR_EQ(device_name, result);

  buf.resize(sizeof(device_name) / 2);
  result = mkdevpath(dev, buf.data(), buf.size());
  EXPECT_STR_EQ("...", result);
}

}  // namespace
