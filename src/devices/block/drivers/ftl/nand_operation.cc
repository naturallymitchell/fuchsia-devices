// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "nand_operation.h"

#include <lib/ddk/debug.h>
#include <lib/ddk/driver.h>
#include <zircon/errors.h>
#include <zircon/process.h>
#include <zircon/status.h>

#include "lib/zx/status.h"

namespace ftl {

zx_status_t NandOperation::SetDataVmo(size_t num_bytes) {
  nand_operation_t* operation = GetOperation();
  if (!operation) {
    return ZX_ERR_NO_MEMORY;
  }
  zx_status_t status = GetVmo(num_bytes);
  if (status != ZX_OK) {
    return status;
  }

  operation->rw.data_vmo = mapper_.vmo().get();
  return ZX_OK;
}

zx_status_t NandOperation::SetOobVmo(size_t num_bytes) {
  nand_operation_t* operation = GetOperation();
  if (!operation) {
    return ZX_ERR_NO_MEMORY;
  }
  zx_status_t status = GetVmo(num_bytes);
  if (status != ZX_OK) {
    return status;
  }

  operation->rw.oob_vmo = mapper_.vmo().get();
  return ZX_OK;
}

nand_operation_t* NandOperation::GetOperation() {
  if (!raw_buffer_) {
    CreateOperation();
  }
  return reinterpret_cast<nand_operation_t*>(raw_buffer_.get());
}

zx_status_t NandOperation::Execute(OobDoubler* parent) {
  parent->Queue(GetOperation(), OnCompletion, this);
  zx_status_t status = sync_completion_wait(&event_, ZX_SEC(60));
  sync_completion_reset(&event_);
  if (status != ZX_OK) {
    return status;
  }
  return status_;
}

// Static.
void NandOperation::OnCompletion(void* cookie, zx_status_t status, nand_operation_t* op) {
  NandOperation* operation = reinterpret_cast<NandOperation*>(cookie);
  operation->status_ = status;
  sync_completion_signal(&operation->event_);
}

zx_status_t NandOperation::GetVmo(size_t num_bytes) {
  if (mapper_.start()) {
    return ZX_OK;
  }

  return mapper_.CreateAndMap(num_bytes, "");
}

std::vector<zx::status<>> NandOperation::ExecuteBatch(
    OobDoubler* parent, fbl::Span<std::unique_ptr<NandOperation>> operations) {
  std::vector<zx::status<>> results(operations.size(), zx::ok());
  for (auto& operation : operations) {
    parent->Queue(operation->GetOperation(), &OnCompletion, static_cast<void*>(operation.get()));
  }

  for (size_t i = 0; i < operations.size(); ++i) {
    zx_status_t status = sync_completion_wait(&operations[i]->event_, zx::sec(60).get());
    results[i] = status == ZX_OK ? zx::status<>(zx::ok()) : zx::status<>(zx::error(status));
    if (results[i].is_ok()) {
      results[i] = operations[i]->status_ == ZX_OK
                       ? zx::status<>(zx::ok())
                       : zx::status<>(zx::error(operations[i]->status_));
    }
  }

  return results;
}

void NandOperation::CreateOperation() {
  ZX_DEBUG_ASSERT(op_size_ >= sizeof(nand_operation_t));
  raw_buffer_.reset(new char[op_size_]);

  memset(raw_buffer_.get(), 0, op_size_);
}

}  // namespace ftl.
