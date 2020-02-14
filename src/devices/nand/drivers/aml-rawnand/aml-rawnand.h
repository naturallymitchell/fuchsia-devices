// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_NAND_DRIVERS_AML_RAWNAND_AML_RAWNAND_H_
#define SRC_STORAGE_NAND_DRIVERS_AML_RAWNAND_AML_RAWNAND_H_

#include <lib/device-protocol/pdev.h>
#include <lib/device-protocol/platform-device.h>
#include <lib/mmio/mmio.h>
#include <lib/sync/completion.h>
#include <lib/zx/time.h>
#include <string.h>
#include <unistd.h>
#include <zircon/threads.h>
#include <zircon/types.h>

#include <memory>
#include <utility>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/io-buffer.h>
#include <ddk/mmio-buffer.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform/device.h>
#include <ddktl/device.h>
#include <ddktl/protocol/rawnand.h>
#include <fbl/bitfield.h>
#include <hw/reg.h>
#include <soc/aml-common/aml-rawnand.h>

#include "onfi.h"

namespace amlrawnand {

struct AmlController {
  int ecc_strength;
  int user_mode;
  int rand_mode;
  int options;
  int bch_mode;
};

// In the case where user_mode == 2 (2 OOB bytes per ECC page),
// the controller adds one of these structs *per* ECC page in
// the info_buf.
struct AmlInfoFormat {
  uint16_t info_bytes;
  uint8_t zero_bits; /* bit0~5 is valid */
  union ecc_sta {
    uint8_t raw_value;
    fbl::BitFieldMember<uint8_t, 0, 6> eccerr_cnt;
    fbl::BitFieldMember<uint8_t, 7, 1> completed;
  } ecc;
  uint32_t reserved;

  // BitFieldMember is not trivially copyable so neither are we, have to copy
  // each member over manually (copy assignment is needed by tests).
  AmlInfoFormat& operator=(const AmlInfoFormat& other) {
    info_bytes = other.info_bytes;
    zero_bits = other.zero_bits;
    ecc.raw_value = other.ecc.raw_value;
    reserved = other.reserved;
    return *this;
  }
};

// gcc doesn't let us use __PACKED with fbl::BitFieldMember<>, but it shouldn't
// make a difference practically in how the AmlInfoFormat struct is laid out
// and this assertion will double-check that we don't need it.
static_assert(sizeof(AmlInfoFormat) == 8, "sizeof(AmlInfoFormat) must be exactly 8 bytes");

// This should always be the case, but we also need an array of AmlInfoFormats
// to have no padding between the items.
static_assert(sizeof(AmlInfoFormat[2]) == 16, "AmlInfoFormat has unexpected padding");

class AmlRawNand;
using DeviceType = ddk::Device<AmlRawNand, ddk::UnbindableNew>;

class AmlRawNand : public DeviceType, public ddk::RawNandProtocol<AmlRawNand, ddk::base_protocol> {
 public:
  explicit AmlRawNand(zx_device_t* parent, ddk::MmioBuffer mmio_nandreg,
                      ddk::MmioBuffer mmio_clockreg, zx::bti bti, zx::interrupt irq,
                      std::unique_ptr<Onfi> onfi)
      : DeviceType(parent),
        onfi_(std::move(onfi)),
        mmio_nandreg_(std::move(mmio_nandreg)),
        mmio_clockreg_(std::move(mmio_clockreg)),
        bti_(std::move(bti)),
        irq_(std::move(irq)) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  virtual ~AmlRawNand() = default;

  void DdkRelease();
  void DdkUnbindNew(ddk::UnbindTxn txn);

  zx_status_t Bind();
  zx_status_t Init();
  zx_status_t RawNandReadPageHwecc(uint32_t nand_page, void* data, size_t data_size,
                                   size_t* data_actual, void* oob, size_t oob_size,
                                   size_t* oob_actual, uint32_t* ecc_correct);
  zx_status_t RawNandWritePageHwecc(const void* data, size_t data_size, const void* oob,
                                    size_t oob_size, uint32_t nand_page);
  zx_status_t RawNandEraseBlock(uint32_t nand_page);
  zx_status_t RawNandGetNandInfo(fuchsia_hardware_nand_Info* nand_info);

 protected:
  // These functions require complicated hardware interaction so need to be
  // overridden or called differently in tests.

  // Waits until a read completes.
  virtual zx_status_t AmlQueueRB();

  // Waits until DMA has transfered data into or out of the NAND buffers.
  virtual zx_status_t AmlWaitDmaFinish();

  // Reads a single status byte from a NAND register. Used during initialization
  // to query the chip information and settings.
  virtual uint8_t AmlReadByte();

  // Normally called when the driver is unregistered but is not automatically
  // called on destruction, so needs to be called manually by tests before
  // destroying this object.
  void CleanUpIrq();

  // Tests can fake page read/writes by copying bytes to/from these buffers.
  const ddk::IoBuffer& data_buffer() { return data_buffer_; }
  const ddk::IoBuffer& info_buffer() { return info_buffer_; }

 private:
  std::unique_ptr<Onfi> onfi_;
  void *info_buf_, *data_buf_;
  zx_paddr_t info_buf_paddr_, data_buf_paddr_;

  ddk::MmioBuffer mmio_nandreg_;
  ddk::MmioBuffer mmio_clockreg_;

  zx::bti bti_;
  zx::interrupt irq_;

  thrd_t irq_thread_;
  ddk::IoBuffer data_buffer_;
  ddk::IoBuffer info_buffer_;
  sync_completion_t req_completion_;

  AmlController controller_params_;
  uint32_t chip_select_ = 0;  // Default to 0.
  int chip_delay_ = 100;      // Conservative default before we query chip to find better value.
  uint32_t writesize_;        /* NAND pagesize - bytes */
  uint32_t erasesize_;        /* size of erase block - bytes */
  uint32_t erasesize_pages_;
  uint32_t oobsize_;    /* oob bytes per NAND page - bytes */
  uint32_t bus_width_;  /* 16bit or 8bit ? */
  uint64_t chipsize_;   /* MiB */
  uint32_t page_shift_; /* NAND page shift */
  struct {
    uint64_t ecc_corrected;
    uint64_t failed;
  } stats;

  polling_timings_t polling_timings_ = {};

  void AmlCmdCtrl(int32_t cmd, uint32_t ctrl);
  void NandctrlSetCfg(uint32_t val);
  void NandctrlSetTimingAsync(int bus_tim, int bus_cyc);
  void NandctrlSendCmd(uint32_t cmd);
  void AmlCmdIdle(uint32_t time);
  zx_status_t AmlWaitCmdFinish(zx::duration timeout, zx::duration first_interval,
                               zx::duration polling_interval);
  void AmlCmdSeed(uint32_t seed);
  void AmlCmdN2M(uint32_t ecc_pages, uint32_t ecc_pagesize);
  void AmlCmdM2N(uint32_t ecc_pages, uint32_t ecc_pagesize);
  void AmlCmdM2NPage0();
  void AmlCmdN2MPage0();
  // Returns the AmlInfoFormat struct corresponding to the i'th
  // ECC page. THIS ASSUMES user_mode == 2 (2 OOB bytes per ECC page).
  void* AmlInfoPtr(int i);
  zx_status_t AmlGetOOBByte(uint8_t* oob_buf, size_t* oob_actual);
  zx_status_t AmlSetOOBByte(const uint8_t* oob_buf, size_t oob_size, uint32_t ecc_pages);
  // Returns the maximum bitflips corrected on this NAND page
  // (the maximum bitflips across all of the ECC pages in this page).
  zx_status_t AmlGetECCCorrections(int ecc_pages, uint32_t nand_page, uint32_t* ecc_corrected);
  zx_status_t AmlCheckECCPages(int ecc_pages);
  void AmlSetClockRate(uint32_t clk_freq);
  void AmlClockInit();
  void AmlAdjustTimings(uint32_t tRC_min, uint32_t tREA_max, uint32_t RHOH_min);
  zx_status_t AmlGetFlashType();
  int IrqThread();
  void AmlSetEncryption();
  zx_status_t AmlReadPage0(void* data, size_t data_size, void* oob, size_t oob_size,
                           uint32_t nand_page, uint32_t* ecc_correct, int retries);
  // Reads one of the page0 pages, and use the result to init
  // ECC algorithm and rand-mode.
  zx_status_t AmlNandInitFromPage0();
  zx_status_t AmlRawNandAllocBufs();
  zx_status_t AmlNandInit();
};

}  // namespace amlrawnand

#endif  // SRC_STORAGE_NAND_DRIVERS_AML_RAWNAND_AML_RAWNAND_H_
