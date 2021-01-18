// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/driver-integration-test/fixture.h>
#include <lib/fdio/directory.h>
#include <zircon/syscalls.h>

#include <unordered_set>

#include <ddk/platform-defs.h>
#include <zxtest/zxtest.h>

using driver_integration_test::IsolatedDevmgr;

class FallbackTest : public zxtest::Test {
 public:
  ~FallbackTest() override = default;
  // Set up and launch the devmgr.
  void LaunchDevmgr(IsolatedDevmgr::Args args) {
    board_test::DeviceEntry dev = {};
    dev.vid = PDEV_VID_TEST;
    dev.pid = PDEV_PID_FALLBACK_TEST;
    dev.did = 0;
    args.device_list.push_back(dev);
    // We disable searching for drivers so that "TestFallbackBoundWhenAlone" works as expected.
    args.driver_search_paths.push_back("/do-not-search");
    // Explicitly whitelist drivers that are necessary for the integration test to come up.
    args.load_drivers.push_back("/boot/driver/platform-bus.so");
    args.load_drivers.push_back("/boot/driver/platform-bus.proxy.so");
    args.load_drivers.push_back("/boot/driver/integration-test.so");

    zx_status_t status = IsolatedDevmgr::Create(&args, &devmgr_);
    ASSERT_OK(status);
  }

  // Check that the correct driver was bound. `fallback` indicates if we expect the fallback or
  // not-fallback driver to have bound.
  void CheckDriverBound(bool fallback) {
    fbl::unique_fd fd;
    fbl::String path = fbl::StringPrintf("sys/platform/11:16:0/ddk-%s-test",
                                         fallback ? "fallback" : "not-fallback");
    ASSERT_OK(
        devmgr_integration_test::RecursiveWaitForFile(devmgr_.devfs_root(), path.c_str(), &fd));
    ASSERT_GT(fd.get(), 0);
    ASSERT_OK(fdio_get_service_handle(fd.release(), chan_.reset_and_get_address()));
    ASSERT_NE(chan_.get(), ZX_HANDLE_INVALID);
  }

 protected:
  zx::channel chan_;
  IsolatedDevmgr devmgr_;
};

TEST_F(FallbackTest, TestNotFallbackTakesPriority) {
  IsolatedDevmgr::Args args;
  args.load_drivers.push_back("/boot/driver/ddk-fallback-test.so");
  args.load_drivers.push_back("/boot/driver/ddk-not-fallback-test.so");
  ASSERT_NO_FATAL_FAILURES(LaunchDevmgr(std::move(args)));
  ASSERT_NO_FATAL_FAILURES(CheckDriverBound(false));
}

TEST_F(FallbackTest, TestFallbackBoundWhenAlone) {
  IsolatedDevmgr::Args args;
  args.load_drivers.push_back("/boot/driver/ddk-fallback-test.so");
  ASSERT_NO_FATAL_FAILURES(LaunchDevmgr(std::move(args)));
  ASSERT_NO_FATAL_FAILURES(CheckDriverBound(true));
}

TEST_F(FallbackTest, TestFallbackBoundWhenEager) {
  IsolatedDevmgr::Args args;
  args.boot_args.emplace(std::make_pair("devmgr.bind-eager", "ddk_fallback_test"));
  args.load_drivers.push_back("/boot/driver/ddk-fallback-test.so");
  args.load_drivers.push_back("/boot/driver/ddk-not-fallback-test.so");
  ASSERT_NO_FATAL_FAILURES(LaunchDevmgr(std::move(args)));
  ASSERT_NO_FATAL_FAILURES(CheckDriverBound(true));
}
