// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_NAND_DRIVERS_CADENCE_HPNFC_CADENCE_HPNFC_H_
#define SRC_STORAGE_NAND_DRIVERS_CADENCE_HPNFC_CADENCE_HPNFC_H_

#include <lib/mmio/mmio.h>
#include <lib/sync/completion.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <lib/zx/interrupt.h>
#include <threads.h>

#include <ddktl/device.h>
#include <ddktl/protocol/rawnand.h>
#include <fbl/mutex.h>

namespace rawnand {

class CadenceHpnfc;
using DeviceType = ddk::Device<CadenceHpnfc, ddk::UnbindableNew>;

class CadenceHpnfc : public DeviceType,
                     public ddk::RawNandProtocol<CadenceHpnfc, ddk::base_protocol> {
 public:
  static zx_status_t Create(void* ctx, zx_device_t* parent);

  CadenceHpnfc(zx_device_t* parent, ddk::MmioBuffer mmio, ddk::MmioBuffer fifo_mmio,
               zx::interrupt interrupt)
      : DeviceType(parent),
        mmio_(std::move(mmio)),
        fifo_mmio_(std::move(fifo_mmio)),
        interrupt_(std::move(interrupt)) {}

  void DdkUnbindNew(ddk::UnbindTxn txn);
  void DdkRelease();

  zx_status_t RawNandReadPageHwecc(uint32_t nandpage, void* out_data_buffer, size_t data_size,
                                   size_t* out_data_actual, void* out_oob_buffer, size_t oob_size,
                                   size_t* out_oob_actual, uint32_t* out_ecc_correct);
  zx_status_t RawNandWritePageHwecc(const void* data_buffer, size_t data_size,
                                    const void* oob_buffer, size_t oob_size, uint32_t nandpage);
  zx_status_t RawNandEraseBlock(uint32_t nandpage);
  zx_status_t RawNandGetNandInfo(nand_info_t* out_info);

  // Visible for testing.
  zx_status_t Bind();
  zx_status_t StartInterruptThread();

 private:
  zx_status_t Init();

  zx_status_t PopulateNandInfoJedec();
  zx_status_t PopulateNandInfoOnfi();

  zx_status_t DoGenericCommand(uint32_t instruction, uint8_t* out_data, uint32_t size);

  // Copy data to or from the FIFO. size is the total number of bytes expected. CopyFromFifo returns
  // the number of bytes read into buffer, which may be zero if buffer is null.
  size_t CopyFromFifo(void* buffer, size_t size);
  void CopyToFifo(const void* buffer, size_t size);

  bool WaitForRBn();
  bool WaitForThread();
  zx_status_t WaitForSdmaTrigger();
  bool WaitForCommandComplete();

  void StopInterruptThread();
  int InterruptThread();

  ddk::MmioBuffer mmio_;
  ddk::MmioBuffer fifo_mmio_;
  zx::interrupt interrupt_;
  nand_info_t nand_info_;

  fbl::Mutex lock_;
  thrd_t interrupt_thread_;
  sync_completion_t completion_;
  bool thread_started_ TA_GUARDED(lock_) = false;
  zx_status_t sdma_status_ TA_GUARDED(lock_) = ZX_ERR_BAD_STATE;
  bool cmd_complete_ TA_GUARDED(lock_) = false;
};

}  // namespace rawnand

#endif  // SRC_STORAGE_NAND_DRIVERS_CADENCE_HPNFC_CADENCE_HPNFC_H_
