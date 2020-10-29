// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_AML_SDMMC_AML_SDMMC_H_
#define SRC_DEVICES_BLOCK_DRIVERS_AML_SDMMC_AML_SDMMC_H_

#include <lib/mmio/mmio.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/interrupt.h>
#include <threads.h>

#include <ddk/phys-iter.h>
#include <ddktl/device.h>
#include <ddktl/protocol/gpio.h>
#include <ddktl/protocol/platform/device.h>
#include <ddktl/protocol/sdmmc.h>
#include <fbl/auto_lock.h>
#include <fbl/condition_variable.h>
#include <fbl/span.h>
#include <soc/aml-common/aml-sdmmc.h>

namespace sdmmc {

class AmlSdmmc;
using AmlSdmmcType = ddk::Device<AmlSdmmc, ddk::Suspendable, ddk::Unbindable>;

class AmlSdmmc : public AmlSdmmcType, public ddk::SdmmcProtocol<AmlSdmmc, ddk::base_protocol> {
 public:
  explicit AmlSdmmc(zx_device_t* parent, zx::bti bti, ddk::MmioBuffer mmio,
                    ddk::MmioPinnedBuffer pinned_mmio, aml_sdmmc_config_t config, zx::interrupt irq,
                    const ddk::GpioProtocolClient& gpio)
      : AmlSdmmcType(parent),
        mmio_(std::move(mmio)),
        bti_(std::move(bti)),
        pinned_mmio_(std::move(pinned_mmio)),
        reset_gpio_(gpio),
        irq_(std::move(irq)),
        board_config_(config),
        dead_(false),
        pending_txn_(false) {}

  virtual ~AmlSdmmc() {}
  static zx_status_t Create(void* ctx, zx_device_t* parent);

  // Device protocol implementation
  void DdkRelease();
  void DdkSuspend(ddk::SuspendTxn txn);
  void DdkUnbind(ddk::UnbindTxn txn);

  // Sdmmc Protocol implementation
  zx_status_t SdmmcHostInfo(sdmmc_host_info_t* out_info);
  zx_status_t SdmmcSetSignalVoltage(sdmmc_voltage_t voltage);
  zx_status_t SdmmcSetBusWidth(sdmmc_bus_width_t bus_width);
  zx_status_t SdmmcSetBusFreq(uint32_t bus_freq);
  zx_status_t SdmmcSetTiming(sdmmc_timing_t timing);
  void SdmmcHwReset();
  zx_status_t SdmmcPerformTuning(uint32_t cmd_idx);
  zx_status_t SdmmcRequest(sdmmc_req_t* req);
  zx_status_t SdmmcRegisterInBandInterrupt(const in_band_interrupt_protocol_t* interrupt_cb);
  zx_status_t SdmmcRegisterVmo(uint32_t vmo_id, uint8_t client_id, zx::vmo vmo, uint64_t offset,
                               uint64_t size);
  zx_status_t SdmmcUnregisterVmo(uint32_t vmo_id, uint8_t client_id, zx::vmo* out_vmo);
  zx_status_t SdmmcRequestNew(const sdmmc_req_new_t* req, uint32_t out_response[4]);

  // Visible for tests
  zx_status_t Init();
  void set_board_config(const aml_sdmmc_config_t& board_config) { board_config_ = board_config; }

 protected:
  // Visible for tests
  zx_status_t Bind();

  virtual zx_status_t WaitForInterruptImpl();
  virtual void WaitForBus() const;

  ddk::MmioBuffer mmio_;

 private:
  struct TuneWindow {
    uint32_t start = 0;
    uint32_t size = 0;

    uint32_t middle() const { return start + (size / 2); }
  };

  void DumpRegs();
  void DumpSdmmcStatus(uint32_t status) const;
  void DumpSdmmcCfg(uint32_t config) const;
  void DumpSdmmcClock(uint32_t clock) const;
  void DumpSdmmcCmdCfg(uint32_t cmd_desc) const;
  uint32_t GetClkFreq(uint32_t clk_src) const;
  zx_status_t TuningDoTransfer(uint8_t* tuning_res, size_t blk_pattern_size,
                               uint32_t tuning_cmd_idx);
  bool TuningTestSettings(fbl::Span<const uint8_t> tuning_blk, uint32_t tuning_cmd_idx);
  template <typename SetParamCallback>
  TuneWindow TuneDelayParam(fbl::Span<const uint8_t> tuning_blk, uint32_t tuning_cmd_idx,
                            uint32_t param_max, SetParamCallback& set_param);

  void SetAdjDelay(uint32_t adj_delay);
  void SetDelayLines(uint32_t delay);
  uint32_t max_delay() const;

  void ConfigureDefaultRegs();
  void SetupCmdDesc(sdmmc_req_t* req, aml_sdmmc_desc_t** out_desc);
  // Prepares the VMO and sets up the data descriptors
  zx_status_t SetupDataDescsDma(sdmmc_req_t* req, aml_sdmmc_desc_t* cur_desc,
                                aml_sdmmc_desc_t** last_desc);
  // Sets up the data descriptors using the ping/pong buffers
  zx_status_t SetupDataDescsPio(sdmmc_req_t* req, aml_sdmmc_desc_t* desc,
                                aml_sdmmc_desc_t** last_desc);
  zx_status_t SetupDataDescs(sdmmc_req_t* req, aml_sdmmc_desc_t* desc,
                             aml_sdmmc_desc_t** last_desc);
  zx_status_t FinishReq(sdmmc_req_t* req);
  void ClearStatus();
  zx_status_t WaitForInterrupt(sdmmc_req_t* req);

  void ShutDown();

  zx::bti bti_;

  ddk::MmioPinnedBuffer pinned_mmio_;
  const ddk::GpioProtocolClient reset_gpio_;
  zx::interrupt irq_;
  aml_sdmmc_config_t board_config_;

  sdmmc_host_info_t dev_info_;
  ddk::IoBuffer descs_buffer_;
  uint32_t max_freq_, min_freq_;

  fbl::Mutex mtx_;
  fbl::ConditionVariable txn_finished_ TA_GUARDED(mtx_);
  std::atomic<bool> dead_ TA_GUARDED(mtx_);
  std::atomic<bool> pending_txn_ TA_GUARDED(mtx_);
};

}  // namespace sdmmc

#endif  // SRC_DEVICES_BLOCK_DRIVERS_AML_SDMMC_AML_SDMMC_H_
