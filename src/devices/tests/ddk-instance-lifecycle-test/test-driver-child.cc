// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test-driver-child.h"

#include <ddk/debug.h>
#include <ddktl/device.h>
#include <fbl/alloc_checker.h>
#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>

using llcpp::fuchsia::device::instancelifecycle::test::Lifecycle;

void TestLifecycleDriverChild::DdkRelease() {
  zx_status_t status = lifecycle_.OnRelease();
  ZX_ASSERT(status == ZX_OK);
  delete this;
}

zx_status_t TestLifecycleDriverChild::Create(zx_device_t* parent, zx::channel lifecycle_client,
                                             zx::channel instance_client) {
  auto device = std::make_unique<TestLifecycleDriverChild>(parent, std::move(lifecycle_client));

  zx_status_t status =
      device->DdkAdd(ddk::DeviceAddArgs("child").set_client_remote(std::move(instance_client)));
  if (status == ZX_OK) {
    // devmgr is now in charge of the memory for dev
    __UNUSED auto ptr = device.release();
  }
  return status;
}

void TestLifecycleDriverChild::DdkUnbind(ddk::UnbindTxn txn) {
  zx_status_t status = lifecycle_.OnUnbind();
  ZX_ASSERT(status == ZX_OK);
  txn.Reply();
}

zx_status_t TestLifecycleDriverChild::DdkOpen(zx_device_t** out, uint32_t flags) {
  zx_status_t status = lifecycle_.OnOpen();
  ZX_ASSERT(status == ZX_OK);

  auto device = std::make_unique<TestLifecycleDriverChildInstance>(zxdev(), this);
  status = device->DdkAdd("child-instance", DEVICE_ADD_INSTANCE);
  if (status != ZX_OK) {
    return status;
  }

  *out = device->zxdev();
  // devmgr is now in charge of the memory for dev
  __UNUSED auto ptr = device.release();
  return ZX_OK;
}

// Implementation of the instance devices

zx_status_t TestLifecycleDriverChildInstance::DdkClose(uint32_t flags) {
  if (lifecycle_.is_valid()) {
    zx_status_t status = lifecycle_.OnClose();
    ZX_ASSERT(status == ZX_OK);
  }
  return ZX_OK;
}

void TestLifecycleDriverChildInstance::DdkRelease() {
  if (lifecycle_.is_valid()) {
    zx_status_t status = lifecycle_.OnRelease();
    ZX_ASSERT(status == ZX_OK);
  }
  delete this;
}

void TestLifecycleDriverChildInstance::RemoveDevice(RemoveDeviceCompleter::Sync& completer) {
  parent_ctx_->DdkAsyncRemove();
}

void TestLifecycleDriverChildInstance::SubscribeToLifecycle(
    zx::channel client, SubscribeToLifecycleCompleter::Sync& completer) {
  // Currently we only care about supporting one client.
  if (lifecycle_.is_valid()) {
    completer.ReplyError(ZX_ERR_ALREADY_BOUND);
  } else {
    lifecycle_ = Lifecycle::EventSender(std::move(client));
    completer.ReplySuccess();
  }
}
