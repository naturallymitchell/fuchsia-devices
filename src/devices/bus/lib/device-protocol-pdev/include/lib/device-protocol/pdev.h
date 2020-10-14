// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BUS_LIB_DEVICE_PROTOCOL_PDEV_INCLUDE_LIB_DEVICE_PROTOCOL_PDEV_H_
#define SRC_DEVICES_BUS_LIB_DEVICE_PROTOCOL_PDEV_INCLUDE_LIB_DEVICE_PROTOCOL_PDEV_H_

#include <lib/zx/bti.h>
#include <lib/zx/interrupt.h>
#include <zircon/types.h>

#include <optional>

#include <ddktl/protocol/platform/device.h>

namespace ddk {

class MmioBuffer;

class PDev : public PDevProtocolClient {
 public:
  PDev() {}

  // TODO(andresoportus): pass protocol by value/const& so there is no question on lifecycle.
  PDev(pdev_protocol_t* proto) : PDevProtocolClient(proto) {}

  PDev(zx_device_t* parent) : PDevProtocolClient(parent) {}

  ~PDev() = default;

  // Prints out information about the platform device.
  void ShowInfo();

  zx_status_t MapMmio(uint32_t index, std::optional<MmioBuffer>* mmio,
                      uint32_t cache_policy = ZX_CACHE_POLICY_UNCACHED_DEVICE);

  zx_status_t GetInterrupt(uint32_t index, zx::interrupt* out) {
    return PDevProtocolClient::GetInterrupt(index, 0, out);
  }

  zx_status_t GetInterrupt(uint32_t index, uint32_t flags, zx::interrupt* out) {
    return PDevProtocolClient::GetInterrupt(index, flags, out);
  }

  zx_status_t GetBti(uint32_t index, zx::bti* out) {
    return PDevProtocolClient::GetBti(index, out);
  }
};

// This helper is marked weak because it is sometimes necessary to provide a
// test-only version that allows for MMIO fakes or mocks to reach the driver under test.
// For example say you have a fake Protocol device that needs to smuggle a fake MMIO:
//
//  class FakePDev : public ddk::PDevProtocol<FakePDev, ddk::base_protocol> {
//    .....
//    zx_status_t PDevGetMmio(uint32_t index, pdev_mmio_t* out_mmio) {
//      out_mmio->offset = reinterpret_cast<size_t>(this);
//      return ZX_OK;
//    }
//    .....
//  };
//
// The actual implementation expects a real {size, offset, VMO} and therefore it will
// fail. But works if a replacement PDevMakeMmioBufferWeak in the test is provided:
//
//  zx_status_t PDevMakeMmioBufferWeak(
//      const pdev_mmio_t& pdev_mmio, std::optional<MmioBuffer>* mmio) {
//    auto* test_harness = reinterpret_cast<FakePDev*>(pdev_mmio.offset);
//    mmio->emplace(test_harness->fake_mmio());
//    return ZX_OK;
//  }
//
zx_status_t PDevMakeMmioBufferWeak(const pdev_mmio_t& pdev_mmio, std::optional<MmioBuffer>* mmio,
                                   uint32_t cache_policy);

}  // namespace ddk

#endif  // SRC_DEVICES_BUS_LIB_DEVICE_PROTOCOL_PDEV_INCLUDE_LIB_DEVICE_PROTOCOL_PDEV_H_
