// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "composite_device.h"

#include <algorithm>

#include <ddk/protocol/composite.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>

#include "driver_host.h"
#include "zx_device.h"

namespace {

class CompositeDeviceInstance {
 public:
  CompositeDeviceInstance(zx_device_t* zxdev, CompositeFragments&& fragments)
      : zxdev_(zxdev), fragments_(std::move(fragments)) {}

  static zx_status_t Create(fbl::RefPtr<zx_device> zxdev, CompositeFragments&& fragments,
                            std::unique_ptr<CompositeDeviceInstance>* device) {
    // Leak a reference to the zxdev here.  It will be cleaned up by the
    // device_unbind_reply() in Unbind().
    auto dev = std::make_unique<CompositeDeviceInstance>(fbl::ExportToRawPtr(&zxdev),
                                                         std::move(fragments));
    *device = std::move(dev);
    return ZX_OK;
  }

  uint32_t GetFragmentCount() { return static_cast<uint32_t>(fragments_.size()); }

  void GetFragments(zx_device_t** comp_list, size_t comp_count, size_t* comp_actual) {
    size_t actual = std::min(comp_count, fragments_.size());
    for (size_t i = 0; i < actual; ++i) {
      comp_list[i] = fragments_[i].get();
    }
    *comp_actual = actual;
  }

  void Release() { delete this; }

  void Unbind() {
    for (auto& fragment : fragments_) {
      // Drop the reference to the composite device.
      fragment->take_composite();
    }
    fragments_.reset();
    device_unbind_reply(zxdev_);
  }

  const CompositeFragments& fragments() { return fragments_; }

 private:
  zx_device_t* zxdev_;
  CompositeFragments fragments_;
};

}  // namespace

// Get the placeholder driver structure for the composite driver
fbl::RefPtr<zx_driver> GetCompositeDriver() {
  static fbl::Mutex lock;
  static fbl::RefPtr<zx_driver> composite TA_GUARDED(lock);

  fbl::AutoLock guard(&lock);
  if (composite == nullptr) {
    zx_status_t status = zx_driver::Create("<internal:composite>", &composite);
    if (status != ZX_OK) {
      return nullptr;
    }
    composite->set_name("internal:composite");
  }
  return composite;
}

zx_status_t InitializeCompositeDevice(const fbl::RefPtr<zx_device>& dev,
                                      CompositeFragments&& fragments) {
  static const zx_protocol_device_t composite_device_ops = []() {
    zx_protocol_device_t ops = {};
    ops.unbind = [](void* ctx) { static_cast<CompositeDeviceInstance*>(ctx)->Unbind(); };
    ops.release = [](void* ctx) { static_cast<CompositeDeviceInstance*>(ctx)->Release(); };
    return ops;
  }();
  static composite_protocol_ops_t composite_ops = []() {
    composite_protocol_ops_t ops = {};
    ops.get_fragment_count = [](void* ctx) {
      return static_cast<CompositeDeviceInstance*>(ctx)->GetFragmentCount();
    };
    ops.get_fragments = [](void* ctx, zx_device_t** comp_list, size_t comp_count,
                           size_t* comp_actual) {
      static_cast<CompositeDeviceInstance*>(ctx)->GetFragments(comp_list, comp_count, comp_actual);
    };
    ops.get_fragment_count = [](void* ctx) {
      return static_cast<CompositeDeviceInstance*>(ctx)->GetFragmentCount();
    };
    ops.get_fragments = [](void* ctx, zx_device_t** comp_list, size_t comp_count,
                           size_t* comp_actual) {
      static_cast<CompositeDeviceInstance*>(ctx)->GetFragments(comp_list, comp_count, comp_actual);
    };
    return ops;
  }();

  auto composite = fbl::MakeRefCounted<CompositeDevice>(dev);

  std::unique_ptr<CompositeDeviceInstance> new_device;
  zx_status_t status = CompositeDeviceInstance::Create(dev, std::move(fragments), &new_device);
  if (status != ZX_OK) {
    return status;
  }

  for (auto& fragment : new_device->fragments()) {
    fragment->set_composite(composite);
  }

  dev->set_protocol_id(ZX_PROTOCOL_COMPOSITE);
  dev->protocol_ops = &composite_ops;
  dev->set_ops(&composite_device_ops);
  dev->ctx = new_device.release();
  // Flag that when this is cleaned up, we should run its release hook.
  dev->set_flag(DEV_FLAG_ADDED);
  return ZX_OK;
}

CompositeDevice::~CompositeDevice() = default;
