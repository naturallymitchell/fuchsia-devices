// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_RAM_DRIVERS_AML_RAM_AML_RAM_H_
#define SRC_DEVICES_RAM_DRIVERS_AML_RAM_AML_RAM_H_

#include <fuchsia/device/llcpp/fidl.h>
#include <fuchsia/hardware/ram/metrics/llcpp/fidl.h>
#include <lib/mmio/mmio.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/interrupt.h>
#include <lib/zx/port.h>

#include <deque>
#include <thread>

#include <ddktl/device.h>
#include <fbl/mutex.h>

namespace ram_metrics = ::llcpp::fuchsia::hardware::ram::metrics;

namespace amlogic_ram {

// The AmlRam device provides FIDL services directly to applications
// to query performance counters. For example effective DDR bandwith.
//
// There are 4 monitoring channels and each one can agregate up to 64
// hardware memory ports. NOTE: the word channel and port in this file
// refer to hardware, not to zircon objects.
constexpr size_t MEMBW_MAX_CHANNELS = 4u;

// Controls start,stop and if polling or interrupt mode.
constexpr uint32_t MEMBW_PORTS_CTRL = (0x0020 << 2);
constexpr uint32_t DMC_QOS_ENABLE_CTRL = (0x01 << 31);
constexpr uint32_t DMC_QOS_CLEAR_CTRL = (0x01 << 30);

// Returns all granted cycles.
constexpr uint32_t MEMBW_ALL_GRANT_CNT = (0x2a << 2);

// Returns the granted cycles per channel.
constexpr uint32_t MEMBW_C0_GRANT_CNT = (0x2b << 2);
constexpr uint32_t MEMBW_C1_GRANT_CNT = (0x2c << 2);
constexpr uint32_t MEMBW_C2_GRANT_CNT = (0x2d << 2);
constexpr uint32_t MEMBW_C3_GRANT_CNT = (0x2e << 2);

// Controls how long to measure cycles for.
constexpr uint32_t MEMBW_TIMER = (0x002f << 2);

// Controls which ports are assigned to each channel.
constexpr uint32_t MEMBW_RP[MEMBW_MAX_CHANNELS] = {(0x0021 << 2), (0x0023 << 2), (0x0025 << 2),
                                                   (0x0027 << 2)};

// Controls wich subports are assinged to each channel.
constexpr uint32_t MEMBW_SP[MEMBW_MAX_CHANNELS] = {(0x0022 << 2), (0x0024 << 2), (0x0026 << 2),
                                                   (0x0028 << 2)};

// Contains the DDR frequency.
// TODO(reveman): Understand why we use 0x0300 instead of 0x0000.
constexpr uint32_t MEMBW_PLL_CNTL = (0x0300 << 2);

constexpr uint64_t kMinimumCycleCount = 1024 * 512;
constexpr uint64_t kMaximumCycleCount = 0xffffffff;

class AmlRam;
using DeviceType = ddk::Device<AmlRam, ddk::Suspendable, ddk::Messageable>;

class AmlRam : public DeviceType, private ram_metrics::Device::Interface {
 public:
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AmlRam);

  static zx_status_t Create(void* context, zx_device_t* parent);

  AmlRam(zx_device_t* parent, ddk::MmioBuffer mmio, zx::interrupt irq, zx::port port,
         bool all_grant_broken);
  ~AmlRam();
  void DdkRelease();
  void DdkSuspend(ddk::SuspendTxn txn);

  // Implements ddk::Messageable
  zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn);

 private:
  struct Job {
    ram_metrics::BandwidthMeasurementConfig config;
    MeasureBandwidthCompleter::Async completer;
    zx_time_t start_time = 0;
    Job() = delete;
    Job(ram_metrics::BandwidthMeasurementConfig config, MeasureBandwidthCompleter::Async completer)
        : config(std::move(config)), completer(std::move(completer)) {}
  };

  // Implementation of ram_metrics::Device::Interface FIDL service.
  void MeasureBandwidth(ram_metrics::BandwidthMeasurementConfig config,
                        MeasureBandwidthCompleter::Sync completer) override;

  void StartReadBandwithCounters(Job* job);
  void FinishReadBandwithCounters(ram_metrics::BandwidthInfo* bpi, zx_time_t start_time);
  void CancelReadBandwithCounters();

  zx_status_t Bind();
  void ReadLoop();
  void RevertJobs(std::deque<AmlRam::Job>* source);
  void AcceptJobs(std::deque<AmlRam::Job>* source);
  void Shutdown();
  uint64_t ReadFrequency() const;

  ddk::MmioBuffer mmio_;
  zx::interrupt irq_;
  zx::port port_;
  std::thread thread_;
  fbl::Mutex lock_;
  std::deque<Job> requests_ TA_GUARDED(lock_);
  bool shutdown_ TA_GUARDED(lock_) = false;
  const bool all_grant_broken_;
};

}  // namespace amlogic_ram

#endif  // SRC_DEVICES_RAM_DRIVERS_AML_RAM_AML_RAM_H_
