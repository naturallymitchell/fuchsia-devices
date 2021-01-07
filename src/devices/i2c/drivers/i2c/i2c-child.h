// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_I2C_DRIVERS_I2C_I2C_CHILD_H_
#define SRC_DEVICES_I2C_DRIVERS_I2C_I2C_CHILD_H_

#include <fuchsia/hardware/i2c/cpp/banjo.h>
#include <fuchsia/hardware/i2c/llcpp/fidl.h>
#include <fuchsia/hardware/i2cimpl/cpp/banjo.h>

#include <ddk/metadata/i2c.h>
#include <ddk/platform-defs.h>
#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <fbl/ref_ptr.h>
#include <fbl/vector.h>

#include "i2c-bus.h"

namespace i2c {

class I2cChild;

using I2cChildType = ddk::Device<I2cChild, ddk::Unbindable, ddk::Messageable>;

namespace fidl_i2c = llcpp::fuchsia::hardware::i2c;

class I2cChild : public I2cChildType,
                 public ddk::I2cProtocol<I2cChild, ddk::base_protocol>,
                 public fidl_i2c::Device2::Interface {
 public:
  I2cChild(zx_device_t* parent, fbl::RefPtr<I2cBus> bus, uint16_t address)
      : I2cChildType(parent), bus_(bus), address_(address) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();
  zx_status_t DdkMessage(fidl_incoming_msg_t* msg, fidl_txn_t* txn) {
    DdkTransaction transaction(txn);
    llcpp::fuchsia::hardware::i2c::Device2::Dispatch(this, msg, &transaction);
    return transaction.Status();
  }

  // FIDL methods.
  void Transfer(fidl::VectorView<bool> input_segments,
                fidl::VectorView<fidl::VectorView<uint8_t>> input_segments_data,
                fidl::VectorView<uint8_t> output_segments_length,
                TransferCompleter::Sync& completer) override;

  void I2cTransact(const i2c_op_t* op_list, size_t op_count, i2c_transact_callback callback,
                   void* cookie);
  zx_status_t I2cGetMaxTransferSize(size_t* out_size);
  zx_status_t I2cGetInterrupt(uint32_t flags, zx::interrupt* out_irq);

 private:
  fbl::RefPtr<I2cBus> bus_;
  const uint16_t address_;
};

}  // namespace i2c

#endif  // SRC_DEVICES_I2C_DRIVERS_I2C_I2C_CHILD_H_
