// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/fake_ddk/fidl-helper.h"

namespace fake_ddk {
namespace {
// We are using lowest bit of transaction as a flag. The static assert assures us that this bit will
// always be 0 due to alignment.
static_assert(alignof(fidl::Transaction) > 1);
constexpr uintptr_t kTransactionIsBoxed = 0x1;

zx_status_t DdkReply(fidl_txn_t* txn, const fidl_outgoing_msg_t* msg) {
  ZX_ASSERT(msg->type == FIDL_OUTGOING_MSG_TYPE_BYTE);
  fidl::OutgoingByteMessage message(reinterpret_cast<uint8_t*>(msg->byte.bytes),
                                    msg->byte.num_bytes, msg->byte.num_bytes, msg->byte.handles,
                                    msg->byte.num_handles, msg->byte.num_handles);
  // If FromDdkInternalTransaction returns a unique_ptr variant, it will be destroyed when exiting
  // this scope.
  auto fidl_txn = FromDdkInternalTransaction(ddk::internal::Transaction::FromTxn(txn));
  std::visit([&](auto&& arg) { arg->Reply(&message); }, fidl_txn);
  return ZX_OK;
}

}  // namespace

ddk::internal::Transaction MakeDdkInternalTransaction(fidl::Transaction* txn) {
  device_fidl_txn_t fidl_txn = {};
  fidl_txn.txn = {
      .reply = DdkReply,
  };
  fidl_txn.driver_host_context = reinterpret_cast<uintptr_t>(txn);
  return ddk::internal::Transaction(fidl_txn);
}

ddk::internal::Transaction MakeDdkInternalTransaction(std::unique_ptr<fidl::Transaction> txn) {
  device_fidl_txn_t fidl_txn = {};
  fidl_txn.txn = {
      .reply = DdkReply,
  };
  fidl_txn.driver_host_context = reinterpret_cast<uintptr_t>(txn.release()) | kTransactionIsBoxed;
  return ddk::internal::Transaction(fidl_txn);
}

std::variant<::fidl::Transaction*, std::unique_ptr<::fidl::Transaction>> FromDdkInternalTransaction(
    ddk::internal::Transaction* txn) {
  uintptr_t raw = txn->DriverHostCtx();
  ZX_ASSERT_MSG(raw != 0, "Reused a fidl_txn_t!\n");

  // Invalidate the source transaction
  txn->DeviceFidlTxn()->driver_host_context = 0;

  auto ptr = reinterpret_cast<fidl::Transaction*>(raw & ~kTransactionIsBoxed);
  if (raw & kTransactionIsBoxed) {
    return std::unique_ptr<fidl::Transaction>(ptr);
  }
  return ptr;
}

::fidl::DispatchResult FidlMessenger::Dispatch(fidl_incoming_msg_t* msg, ::fidl::Transaction* txn) {
  auto ddk_txn = MakeDdkInternalTransaction(txn);
  auto status = message_op_(op_ctx_, msg, ddk_txn.Txn());
  const bool found = status == ZX_OK || status == ZX_ERR_ASYNC;
  if (!found) {
    FidlHandleInfoCloseMany(msg->handles, msg->num_handles);
    txn->Close(status);
  }
  return found ? ::fidl::DispatchResult::kFound : ::fidl::DispatchResult::kNotFound;
}

zx_status_t FidlMessenger::SetMessageOp(void* op_ctx, MessageOp* op,
                                        std::optional<zx::channel> optional_remote) {
  zx_status_t status;

  if (message_op_) {
    // Message op was already set
    return ZX_ERR_BAD_STATE;
  }
  message_op_ = op;
  op_ctx_ = op_ctx;

  // If the caller provided a remote endpoint, we use it below and assume they kept the local
  // endpoint. Otherwise, create a new channel and store the local endpoint.
  zx::channel remote;
  if (optional_remote) {
    remote = std::move(optional_remote.value());
  } else {
    if ((status = zx::channel::create(0, &local_, &remote)) < 0) {
      return status;
    }
  }

  if ((status = loop_.StartThread("fake_ddk_fidl")) < 0) {
    return status;
  }

  auto res = fidl::BindServer(loop_.dispatcher(), std::move(remote), this);
  if (res.is_error()) {
    return res.error();
  }

  binding_ = std::make_unique<fidl::ServerBindingRef<FidlMessenger>>(std::move(res.value()));
  return ZX_OK;
}

}  // namespace fake_ddk
