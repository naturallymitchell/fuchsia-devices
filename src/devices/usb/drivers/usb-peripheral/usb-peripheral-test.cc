// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "usb-peripheral.h"

#include <lib/fake_ddk/fake_ddk.h>
#include <lib/zx/clock.h>
#include <lib/zx/interrupt.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>

#include <cstring>
#include <list>
#include <memory>

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/metadata.h>
#include <ddk/protocol/platform/device.h>
#include <ddk/protocol/usb/dci.h>
#include <ddktl/protocol/usb/dci.h>
#include <fake-mmio-reg/fake-mmio-reg.h>
#include <zxtest/zxtest.h>

#include "usb-function.h"

struct zx_device : std::enable_shared_from_this<zx_device> {
  std::list<std::shared_ptr<zx_device>> devices;
  std::weak_ptr<zx_device> parent;
  std::vector<zx_device_prop_t> props;
  void* proto_ops;
  uint32_t proto_id;
  void* ctx;
  zx_protocol_device_t dev_ops;
  virtual ~zx_device() = default;
};

class FakeDevice : public ddk::UsbDciProtocol<FakeDevice, ddk::base_protocol> {
 public:
  FakeDevice() : proto_({&usb_dci_protocol_ops_, this}) {}

  // USB DCI protocol implementation.
  void UsbDciRequestQueue(usb_request_t* req, const usb_request_complete_t* cb) {}

  zx_status_t UsbDciSetInterface(const usb_dci_interface_protocol_t* interface) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t UsbDciConfigEp(const usb_endpoint_descriptor_t* ep_desc,
                             const usb_ss_ep_comp_descriptor_t* ss_comp_desc) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  zx_status_t UsbDciDisableEp(uint8_t ep_address) { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t UsbDciEpSetStall(uint8_t ep_address) { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t UsbDciEpClearStall(uint8_t ep_address) { return ZX_ERR_NOT_SUPPORTED; }
  size_t UsbDciGetRequestSize() { return sizeof(usb_request_t); }

  zx_status_t UsbDciCancelAll(uint8_t ep_address) { return ZX_OK; }

  usb_dci_protocol_t* proto() { return &proto_; }

 private:
  usb_dci_protocol_t proto_;
};

static void DestroyDevices(zx_device_t* node) {
  for (auto& dev : node->devices) {
    DestroyDevices(dev.get());
    if (dev->dev_ops.unbind) {
      dev->dev_ops.unbind(dev->ctx);
    }
    dev->dev_ops.release(dev->ctx);
  }
}

class Ddk : public fake_ddk::Bind {
 public:
  zx_status_t DeviceGetMetadata(zx_device_t* dev, uint32_t type, void* data, size_t length,
                                size_t* actual) override {
    return ZX_ERR_NOT_FOUND;
  }
  zx_status_t DeviceGetProtocol(const zx_device_t* device, uint32_t proto_id,
                                void* protocol) override {
    if (device->proto_id != proto_id) {
      return ZX_ERR_NOT_SUPPORTED;
    }
    memcpy(protocol, device->proto_ops, 16);
    return ZX_OK;
  }
  zx_status_t DeviceAdd(zx_driver_t* drv, zx_device_t* parent, device_add_args_t* args,
                        zx_device_t** out) override {
    auto dev = std::make_shared<zx_device>();
    dev->ctx = args->ctx;
    dev->proto_ops = args->proto_ops;
    if (args->props) {
      dev->props.resize(args->prop_count);
      memcpy(dev->props.data(), args->props, args->prop_count * sizeof(zx_device_prop_t));
    }
    dev->dev_ops = *args->ops;
    dev->parent = parent->weak_from_this();
    dev->proto_id = args->proto_id;
    parent->devices.push_back(dev);
    *out = dev.get();
    return ZX_OK;
  }

  zx_status_t DeviceRemove(zx_device_t* device) override {
    DestroyDevices(device);
    return ZX_OK;
  }
};

TEST(UsbPeripheral, DoesNotCrash) {
  Ddk ddk;
  auto dci = std::make_unique<FakeDevice>();
  auto root_device = std::make_shared<zx_device_t>();
  root_device->proto_ops = dci->proto();
  root_device->ctx = dci.get();
  root_device->proto_id = ZX_PROTOCOL_USB_DCI;
  zx::interrupt irq;
  ASSERT_OK(zx::interrupt::create(zx::resource(), 0, ZX_INTERRUPT_VIRTUAL, &irq));
  ASSERT_OK(usb_peripheral::UsbPeripheral::Create(nullptr, root_device.get()));
  DestroyDevices(root_device.get());
}
