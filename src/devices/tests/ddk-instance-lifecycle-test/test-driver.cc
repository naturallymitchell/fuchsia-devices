// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/device/instancelifecycle/test/llcpp/fidl.h>
#include <lib/zx/channel.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <ddktl/device.h>
#include <ddktl/fidl.h>

#include "test-driver-child.h"
#include "zircon/errors.h"

namespace {

using llcpp::fuchsia::device::instancelifecycle::test::TestDevice;

class TestLifecycleDriver;
using DeviceType = ddk::Device<TestLifecycleDriver, ddk::Unbindable, ddk::Messageable>;

class TestLifecycleDriver : public DeviceType, public TestDevice::Interface {
 public:
  explicit TestLifecycleDriver(zx_device_t* parent) : DeviceType(parent) {}
  ~TestLifecycleDriver() {}

  // Device protocol implementation.
  void DdkUnbind(ddk::UnbindTxn txn) { txn.Reply(); }
  void DdkRelease() { delete this; }

  // Device message ops implementation.
  void CreateDevice(zx::channel lifecycle_client, zx::channel instance_client,
                    CreateDeviceCompleter::Sync& completer) override;

  zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
    DdkTransaction transaction(txn);
    TestDevice::Dispatch(this, msg, &transaction);
    return transaction.Status();
  }
};

void TestLifecycleDriver::CreateDevice(zx::channel lifecycle_client, zx::channel instance_client,
                                       CreateDeviceCompleter::Sync& completer) {
  zx_status_t status = TestLifecycleDriverChild::Create(zxdev(), std::move(lifecycle_client),
                                                        std::move(instance_client));
  if (status != ZX_OK) {
    completer.ReplyError(status);
  } else {
    completer.ReplySuccess();
  }
}

zx_status_t TestLifecycleBind(void* ctx, zx_device_t* device) {
  auto dev = std::make_unique<TestLifecycleDriver>(device);
  auto status = dev->DdkAdd("instance-test");
  if (status == ZX_OK) {
    // devmgr is now in charge of the memory for dev
    __UNUSED auto ptr = dev.release();
  }
  return status;
}

zx_driver_ops_t driver_ops = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = TestLifecycleBind;
  return ops;
}();

}  // namespace

// clang-format off
ZIRCON_DRIVER_BEGIN(TestLifecycle, driver_ops, "zircon", "0.1", 2)
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_TEST),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_PID, PDEV_PID_INSTANCE_LIFECYCLE_TEST),
ZIRCON_DRIVER_END(TestLifecycle)
    // clang-format on
