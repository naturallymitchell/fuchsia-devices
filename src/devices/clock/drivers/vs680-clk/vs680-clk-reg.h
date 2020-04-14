// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_CLOCK_DRIVERS_VS680_CLK_VS680_CLK_REG_H_
#define SRC_DEVICES_CLOCK_DRIVERS_VS680_CLK_VS680_CLK_REG_H_

#include <hwreg/bitfields.h>

namespace clk {

class PllCtrlA : public hwreg::RegisterBase<PllCtrlA, uint32_t> {
 public:
  static auto Get() { return hwreg::RegisterAddr<PllCtrlA>(0x00); }

  DEF_FIELD(5, 3, range);  // PLL filter range
  DEF_BIT(1, bypass);
  DEF_BIT(0, reset);
};

class PllCtrlC : public hwreg::RegisterBase<PllCtrlC, uint32_t> {
 public:
  static auto Get() { return hwreg::RegisterAddr<PllCtrlC>(0x08); }

  DEF_FIELD(8, 0, divr);  // Reference divider
};

class PllCtrlD : public hwreg::RegisterBase<PllCtrlD, uint32_t> {
 public:
  static auto Get() { return hwreg::RegisterAddr<PllCtrlD>(0x0c); }

  DEF_FIELD(8, 0, divfi);  // Integer divider
};

class PllCtrlE : public hwreg::RegisterBase<PllCtrlE, uint32_t> {
 public:
  static auto Get() { return hwreg::RegisterAddr<PllCtrlE>(0x10); }

  DEF_FIELD(23, 0, divff);  // Fractional divider
};

class PllCtrlF : public hwreg::RegisterBase<PllCtrlF, uint32_t> {
 public:
  static auto Get() { return hwreg::RegisterAddr<PllCtrlF>(0x14); }

  DEF_FIELD(4, 0, divq);  // Output divider for PLLOUT
};

class PllCtrlG : public hwreg::RegisterBase<PllCtrlG, uint32_t> {
 public:
  static auto Get() { return hwreg::RegisterAddr<PllCtrlG>(0x18); }

  DEF_FIELD(2, 0, divqf);  // Output divider for PLLOUT1
};

class PllStatus : public hwreg::RegisterBase<PllStatus, uint32_t> {
 public:
  static auto Get() { return hwreg::RegisterAddr<PllStatus>(0x1c); }

  DEF_BIT(0, lock);  // Output lock detection
};

}  // namespace clk

#endif  // SRC_DEVICES_CLOCK_DRIVERS_VS680_CLK_VS680_CLK_REG_H_
