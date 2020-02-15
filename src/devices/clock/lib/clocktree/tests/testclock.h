// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef ZIRCON_SYSTEM_ULIB_CLOCKTREE_TEST_TESTCLOCK_H_
#define ZIRCON_SYSTEM_ULIB_CLOCKTREE_TEST_TESTCLOCK_H_

#include "baseclock.h"

namespace clk {

// Trivial Clock implementation that doesn't support any operations.
class TestNullClock : public BaseClock {
 public:
  TestNullClock(const char* name, uint32_t id, uint32_t parent)
      : BaseClock(name, id), parent_(parent) {}

  zx_status_t Enable() override { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t Disable() override { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t IsEnabled(bool* out) override { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t SetRate(const Hertz rate, const Hertz parent_rate) override {
    return ZX_ERR_NOT_SUPPORTED;
  }
  zx_status_t QuerySupportedRate(const Hertz max, const Hertz parent_rate, Hertz* out) override {
    return ZX_ERR_NOT_SUPPORTED;
  }
  zx_status_t GetRate(const Hertz parent_rate, Hertz* out) override { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t SetInput(const uint32_t index) override { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t GetNumInputs(uint32_t* out) override { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t GetInput(uint32_t* out) override { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t GetInputId(const uint32_t index, uint32_t* id) override {
    return ZX_ERR_NOT_SUPPORTED;
  }

  uint32_t ParentId() override final { return parent_; }

 private:
  const uint32_t parent_;
};

class TestGateClock : public BaseClock {
 public:
  TestGateClock(const char* name, uint32_t id, uint32_t parent, bool enabled = false)
      : BaseClock(name, id),
        parent_(parent),
        enabled_(enabled),
        enable_count_(0),
        disable_count_(0) {}
  zx_status_t Enable() override final {
    enable_count_++;
    enabled_ = true;
    return ZX_OK;
  }
  zx_status_t Disable() override final {
    disable_count_++;
    enabled_ = false;
    return ZX_OK;
  }
  zx_status_t IsEnabled(bool* out) override final {
    *out = enabled_;
    return ZX_OK;
  }

  zx_status_t SetRate(const Hertz rate, const Hertz parent_rate) override {
    return ZX_ERR_NOT_SUPPORTED;
  }
  zx_status_t QuerySupportedRate(const Hertz max, const Hertz parent_rate, Hertz* out) override {
    return ZX_ERR_NOT_SUPPORTED;
  }
  zx_status_t GetRate(const Hertz parent_rate, Hertz* out) override { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t SetInput(const uint32_t index) override { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t GetNumInputs(uint32_t* out) override { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t GetInput(uint32_t* out) override { return ZX_ERR_NOT_SUPPORTED; }
  zx_status_t GetInputId(const uint32_t index, uint32_t* id) override {
    return ZX_ERR_NOT_SUPPORTED;
  }

  uint32_t ParentId() override final { return parent_; }

  uint32_t EnableCount() const { return enable_count_; }
  uint32_t DisableCount() const { return disable_count_; }

 private:
  const uint32_t parent_;
  bool enabled_;
  uint32_t enable_count_;
  uint32_t disable_count_;
};

// Simple clock implementation that returns ZX_ERR_INTERNAL for each call.
class TestFailClock : public BaseClock {
 public:
  TestFailClock(const char* name, uint32_t id, uint32_t parent)
      : BaseClock(name, id), parent_(parent) {}

  zx_status_t Enable() override { return ZX_ERR_INTERNAL; }
  zx_status_t Disable() override { return ZX_ERR_INTERNAL; }
  zx_status_t IsEnabled(bool* out) override { return ZX_ERR_INTERNAL; }

  zx_status_t SetRate(const Hertz rate, const Hertz parent_rate) override {
    return ZX_ERR_INTERNAL;
  }
  zx_status_t QuerySupportedRate(const Hertz max, const Hertz parent_rate, Hertz* out) override {
    return ZX_ERR_INTERNAL;
  }
  zx_status_t GetRate(const Hertz parent_rate, Hertz* out) override { return ZX_ERR_INTERNAL; }

  zx_status_t SetInput(const uint32_t index) override { return ZX_ERR_INTERNAL; }
  zx_status_t GetNumInputs(uint32_t* out) override { return ZX_ERR_INTERNAL; }
  zx_status_t GetInput(uint32_t* out) override { return ZX_ERR_INTERNAL; }
  zx_status_t GetInputId(const uint32_t index, uint32_t* id) override { return ZX_ERR_INTERNAL; }

  uint32_t ParentId() override final { return parent_; }

 private:
  const uint32_t parent_;
};

}  // namespace clk

#endif  // ZIRCON_SYSTEM_ULIB_CLOCKTREE_TEST_TESTCLOCK_H_
