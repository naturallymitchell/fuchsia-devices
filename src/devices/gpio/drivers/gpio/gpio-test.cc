// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpio.h"

#include <lib/async-loop/default.h>
#include <lib/fake_ddk/fake_ddk.h>
#include <lib/fidl-async/cpp/bind.h>

#include <ddk/debug.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_call.h>
#include <mock/ddktl/protocol/gpioimpl.h>

namespace gpio {

class FakeGpio : public GpioDevice {
 public:
  static std::unique_ptr<FakeGpio> Create(const gpio_impl_protocol_t* proto) {
    fbl::AllocChecker ac;
    auto device = fbl::make_unique_checked<FakeGpio>(&ac, proto);
    if (!ac.check()) {
      zxlogf(ERROR, "FakeGpio::Create: device object alloc failed\n");
      return nullptr;
    }

    return device;
  }

  zx_status_t Connect(async_dispatcher_t* dispatcher, zx::channel request) {
    return fidl::Bind(dispatcher, std::move(request), this);
  }

  explicit FakeGpio(const gpio_impl_protocol_t* gpio_impl)
      : GpioDevice(nullptr, const_cast<gpio_impl_protocol_t*>(gpio_impl), 0) {}
};

class GpioTest : public zxtest::Test {
 public:
  void SetUp() override {
    gpio_ = FakeGpio::Create(gpio_impl_.GetProto());
    ASSERT_NOT_NULL(gpio_);
    loop_ = std::make_unique<async::Loop>(&kAsyncLoopConfigAttachToCurrentThread);

    zx::channel server;
    ASSERT_OK(zx::channel::create(0, &client_, &server));
    ASSERT_OK(loop_->StartThread("gpio-test-loop"));
    ASSERT_OK(gpio_->Connect(loop_->dispatcher(), std::move(server)));
  }

  void TearDown() override {
    gpio_impl_.VerifyAndClear();

    loop_->Shutdown();
  }

 protected:
  std::unique_ptr<FakeGpio> gpio_;
  ddk::MockGpioImpl gpio_impl_;
  std::unique_ptr<async::Loop> loop_;
  zx::channel client_;
};

TEST_F(GpioTest, TestAll) {
  ::llcpp::fuchsia::hardware::gpio::Gpio::SyncClient client(std::move(client_));

  gpio_impl_.ExpectRead(ZX_OK, 0, 20);
  auto result_read = client.Read();
  EXPECT_OK(result_read.status());
  EXPECT_EQ(result_read->result.response().value, 20);

  gpio_impl_.ExpectWrite(ZX_OK, 0, 11);
  auto result_write = client.Write(11);
  EXPECT_OK(result_write.status());

  gpio_impl_.ExpectConfigIn(ZX_OK, 0, 0);
  auto result_in = client.ConfigIn(0);
  EXPECT_OK(result_in.status());

  gpio_impl_.ExpectConfigOut(ZX_OK, 0, 5);
  auto result_out = client.ConfigOut(5);
  EXPECT_OK(result_out.status());
}

}  // namespace gpio
