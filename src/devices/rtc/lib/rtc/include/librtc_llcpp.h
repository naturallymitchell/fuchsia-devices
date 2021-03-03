// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_RTC_LIB_RTC_INCLUDE_LIBRTC_LLCPP_H_
#define SRC_DEVICES_RTC_LIB_RTC_INCLUDE_LIBRTC_LLCPP_H_

#include <fuchsia/hardware/rtc/llcpp/fidl.h>

namespace rtc {

namespace FidlRtc = fuchsia_hardware_rtc;

enum Month {
  JANUARY = 1,  // 31 days
  FEBRUARY,     // 28 or 29
  MARCH,        // 31
  APRIL,        // 30
  MAY,          // 31
  JUNE,         // 30
  JULY,         // 31
  AUGUST,       // 31
  SEPTEMBER,    // 30
  OCTOBER,      // 31
  NOVEMBER,     // 30
  DECEMBER      // 31
};

bool IsRtcValid(FidlRtc::wire::Time rtc);

// Convert |seconds| to RTC. If |seconds| is before the local epoch time, then
// the default RTC value is returned instead.
FidlRtc::wire::Time SecondsToRtc(uint64_t seconds);

uint64_t SecondsSinceEpoch(FidlRtc::wire::Time rtc);

// Validate that |rtc| is set to a valid time and is later than the default year
// and environment backstop time. If it is, then return |rtc|.  Otherwise, return
// the backstop time. If the backstop time isn't available, return the default rtc.
FidlRtc::wire::Time SanitizeRtc(FidlRtc::wire::Time rtc);

}  // namespace rtc

#endif  // SRC_DEVICES_RTC_LIB_RTC_INCLUDE_LIBRTC_LLCPP_H_
