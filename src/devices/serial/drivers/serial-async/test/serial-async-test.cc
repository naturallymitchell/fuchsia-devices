// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fake_ddk/fake_ddk.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/zircon-internal/thread_annotations.h>

#include <memory>
#include <optional>
#include <vector>

#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <zxtest/zxtest.h>

#include "serial.h"

namespace {

constexpr size_t kBufferLength = 16;

constexpr zx_signals_t kEventWrittenSignal = ZX_USER_SIGNAL_0;

// Fake for the SerialImpl protocol.
class FakeSerialImpl : public ddk::SerialImplAsyncProtocol<FakeSerialImpl, ddk::base_protocol> {
 public:
  FakeSerialImpl() : proto_({&serial_impl_async_protocol_ops_, this}), total_written_bytes_(0) {
    zx::event::create(0, &write_event_);
  }

  // Getters.
  const serial_impl_async_protocol_t* proto() const { return &proto_; }
  bool enabled() { return enabled_; }

  char* read_buffer() { return read_buffer_; }
  const char* write_buffer() { return write_buffer_; }
  size_t write_buffer_length() { return write_buffer_length_; }

  size_t total_written_bytes() { return total_written_bytes_; }

  // Test utility methods.
  void set_state_and_notify(serial_state_t state) {
    fbl::AutoLock al(&cb_lock_);

    state_ = state;
  }

  zx_status_t wait_for_write(zx::time deadline, zx_signals_t* pending) {
    return write_event_.wait_one(kEventWrittenSignal, deadline, pending);
  }

  // Raw nand protocol:
  zx_status_t SerialImplAsyncGetInfo(serial_port_info_t* info) { return ZX_OK; }

  zx_status_t SerialImplAsyncConfig(uint32_t baud_rate, uint32_t flags) { return ZX_OK; }

  zx_status_t SerialImplAsyncEnable(bool enable) {
    enabled_ = enable;
    return ZX_OK;
  }

  void SerialImplAsyncReadAsync(serial_impl_async_read_async_callback callback, void* cookie) {
    if (!(state_ & SERIAL_STATE_READABLE)) {
      callback(cookie, ZX_ERR_SHOULD_WAIT, nullptr, 0);
      return;
    }
    char buf[kBufferLength];
    char* buffer = static_cast<char*>(buf);
    size_t i;

    for (i = 0; i < kBufferLength && read_buffer_[i]; ++i) {
      buffer[i] = read_buffer_[i];
    }
    size_t out_actual = i;

    if (i == kBufferLength || !read_buffer_[i]) {
      // Simply reset the state, no advanced state machine.
      set_state_and_notify(0);
    }
    callback(cookie, ZX_OK, buf, out_actual);
  }

  void SerialImplAsyncCancelAll() {
    // Not needed for this test driver
  }

  void SerialImplAsyncWriteAsync(const void* buf_buffer, size_t buf_size,
                                 serial_impl_async_write_async_callback callback, void* cookie) {
    if (!(state_ & SERIAL_STATE_WRITABLE)) {
      callback(cookie, ZX_ERR_SHOULD_WAIT);
      return;
    }

    const char* buffer = static_cast<const char*>(buf_buffer);
    size_t i;

    for (i = 0; i < kBufferLength; ++i) {
      write_buffer_[i] = buffer[i];
    }

    // Signal that the write_buffer has been written to.
    if (i > 0) {
      write_buffer_length_ = i;
      total_written_bytes_ += i;
      write_event_.signal(0, kEventWrittenSignal);
    }

    callback(cookie, ZX_OK);
  }

 private:
  serial_impl_async_protocol_t proto_;
  bool enabled_;

  fbl::Mutex cb_lock_;
  serial_state_t state_;

  char read_buffer_[kBufferLength];
  char write_buffer_[kBufferLength];
  size_t write_buffer_length_;
  size_t total_written_bytes_;

  zx::event write_event_;
};

class SerialTester {
 public:
  SerialTester() {
    fbl::Array<fake_ddk::ProtocolEntry> protocols(new fake_ddk::ProtocolEntry[1], 1);
    protocols[0] = {ZX_PROTOCOL_SERIAL_IMPL_ASYNC,
                    *reinterpret_cast<const fake_ddk::Protocol*>(serial_impl_.proto())};
    ddk_.SetProtocols(std::move(protocols));
  }

  fake_ddk::Bind& ddk() { return ddk_; }
  FakeSerialImpl& serial_impl() { return serial_impl_; }

 private:
  fake_ddk::Bind ddk_;
  FakeSerialImpl serial_impl_;
};

TEST(SerialTest, InitNoProtocolParent) {
  // SerialTester is intentionally not defined in this scope as it would
  // define the ZX_PROTOCOL_SERIAL_IMPL protocol.
  serial::SerialDevice device(fake_ddk::kFakeParent);
  ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, device.Init());
}

TEST(SerialTest, Init) {
  SerialTester tester;
  serial::SerialDevice device(fake_ddk::kFakeParent);
  ASSERT_EQ(ZX_OK, device.Init());
}

TEST(SerialTest, DdkLifetime) {
  SerialTester tester;
  serial::SerialDevice* device(new serial::SerialDevice(fake_ddk::kFakeParent));

  ASSERT_EQ(ZX_OK, device->Init());
  ASSERT_EQ(ZX_OK, device->Bind());
  device->DdkAsyncRemove();
  EXPECT_TRUE(tester.ddk().Ok());

  // Delete the object.
  device->DdkRelease();
  ASSERT_FALSE(tester.serial_impl().enabled());
}

// Provides control primitives for tests that issue IO requests to the device.
class SerialDeviceTest : public zxtest::Test {
 public:
  SerialDeviceTest();
  ~SerialDeviceTest();
  llcpp::fuchsia::hardware::serial::NewDevice::SyncClient& fidl() {
    if (!fidl_.has_value()) {
      // Connect
      auto connection = llcpp::fuchsia::hardware::serial::NewDeviceProxy::SyncClient(
          std::move(tester_.ddk().FidlClient()));
      zx::channel remote;
      zx::channel local;
      zx::channel::create(0, &remote, &local);
      connection.GetChannel(std::move(remote));
      fidl_ = llcpp::fuchsia::hardware::serial::NewDevice::SyncClient(std::move(local));
    }
    return *fidl_;
  }
  serial::SerialDevice* device() { return device_; }
  FakeSerialImpl& serial_impl() { return tester_.serial_impl(); }

  // DISALLOW_COPY_ASSIGN_AND_MOVE(SerialDeviceTest);

 private:
  std::optional<llcpp::fuchsia::hardware::serial::NewDevice::SyncClient> fidl_;
  SerialTester tester_;
  serial::SerialDevice* device_;
};

SerialDeviceTest::SerialDeviceTest() {
  device_ = new serial::SerialDevice(fake_ddk::kFakeParent);

  if (ZX_OK != device_->Init()) {
    delete device_;
    device_ = nullptr;
  }
}

SerialDeviceTest::~SerialDeviceTest() { device_->DdkRelease(); }

static zx_status_t SerialWrite(llcpp::fuchsia::hardware::serial::NewDevice::SyncClient* interface,
                               std::vector<uint8_t>* data) {
  zx_status_t status = interface->Write(fidl::unowned_vec(*data)).status();
  if (status != ZX_OK) {
    return status;
  }
  return ZX_OK;
}

static zx_status_t Read(llcpp::fuchsia::hardware::serial::NewDevice::SyncClient* interface,
                        std::vector<uint8_t>* data) {
  auto result = interface->Read();
  if (result.status() != ZX_OK) {
    return result.status();
  }
  auto& view = result->result.response().data;
  data->resize(view.count());
  memcpy(data->data(), view.data(), view.count());
  return ZX_OK;
}

TEST_F(SerialDeviceTest, AsyncRead) {
  const char* expected = "test";
  std::vector<uint8_t> buffer;
  // Test set up.
  strcpy(serial_impl().read_buffer(), expected);
  serial_impl().set_state_and_notify(SERIAL_STATE_READABLE);
  ASSERT_OK(device()->Bind());
  fbl::AutoCall unbind([=]() { device()->DdkAsyncRemove(); });
  // Test.
  ASSERT_EQ(ZX_OK, Read(&fidl(), &buffer));
  ASSERT_EQ(4, buffer.size());
  ASSERT_EQ(0, memcmp(expected, buffer.data(), buffer.size()));
}

TEST_F(SerialDeviceTest, AsyncWrite) {
  const char* data = "test";
  const size_t kDataLen = 5;
  std::vector<uint8_t> data_buffer;
  data_buffer.resize(kDataLen);
  memcpy(data_buffer.data(), data, kDataLen);

  // Test set up.
  ASSERT_OK(device()->Bind());
  fbl::AutoCall unbind([=]() { device()->DdkAsyncRemove(); });
  serial_impl().set_state_and_notify(SERIAL_STATE_WRITABLE);

  // Test.
  ASSERT_EQ(ZX_OK, SerialWrite(&fidl(), &data_buffer));
  ASSERT_EQ(0, memcmp(data, serial_impl().write_buffer(), kDataLen));
}

}  // namespace
