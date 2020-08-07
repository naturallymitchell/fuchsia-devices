// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_LIB_AMLOGIC_INCLUDE_SOC_AML_COMMON_AML_CPU_METADATA_H_
#define SRC_DEVICES_LIB_AMLOGIC_INCLUDE_SOC_AML_COMMON_AML_CPU_METADATA_H_

#include <zircon/types.h>
#include <ddk/metadata.h>

namespace amlogic_cpu {

#define DEVICE_METADATA_AML_PERF_DOMAINS (0x50524600 | DEVICE_METADATA_PRIVATE)  // PRF
#define DEVICE_METADATA_AML_OP_POINTS    (0x4f505000 | DEVICE_METADATA_PRIVATE)  // OPP

using PerfDomainId = uint32_t;
constexpr size_t kMaxPerformanceDomainNameLength = 32;

typedef struct perf_domain {
  // A unique identifier that maps this performance domain to its
  // operating points.
  PerfDomainId id;

  // An integer in the range [0-255] that defines the relative performance
  // of this domain compared to others in the system.
  uint8_t relative_performance;

  // A friendly name for this performance domain.
  char name[kMaxPerformanceDomainNameLength];
} perf_domain_t;

typedef struct operating_point {
  uint32_t freq_hz;
  uint32_t volt_uv;
  PerfDomainId pd_id;
} operating_point_t;

}  // namespace amlogic_cpu

#endif  // SRC_DEVICES_LIB_AMLOGIC_INCLUDE_SOC_AML_COMMON_AML_CPU_METADATA_H_
