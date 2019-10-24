// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "multiple-device-test.h"

// Reads a CreateDevice from remote, checks expectations, and sends a ZX_OK
// response.
void MultipleDeviceTestCase::CheckCreateDeviceReceived(const zx::channel& remote,
                                                       const char* expected_driver,
                                                       zx::channel* device_remote) {
  // Read the CreateDevice request.
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_bytes;
  uint32_t actual_handles;
  zx_status_t status = remote.read(0, bytes, handles, sizeof(bytes), fbl::count_of(handles),
                                   &actual_bytes, &actual_handles);
  ASSERT_OK(status);
  ASSERT_LT(0, actual_bytes);
  ASSERT_EQ(3, actual_handles);
  *device_remote = zx::channel(handles[0]);
  status = zx_handle_close(handles[1]);
  ASSERT_OK(status);

  // Validate the CreateDevice request.
  auto hdr = reinterpret_cast<fidl_message_header_t*>(bytes);
  ASSERT_EQ(fuchsia_device_manager_DevhostControllerCreateDeviceOrdinal, hdr->ordinal);
  status = fidl_decode(&fuchsia_device_manager_DevhostControllerCreateDeviceRequestTable, bytes,
                       actual_bytes, handles, actual_handles, nullptr);
  ASSERT_OK(status);
  auto req = reinterpret_cast<fuchsia_device_manager_DevhostControllerCreateDeviceRequest*>(bytes);
  ASSERT_EQ(req->driver_path.size, strlen(expected_driver));
  ASSERT_BYTES_EQ(reinterpret_cast<const uint8_t*>(expected_driver),
                  reinterpret_cast<const uint8_t*>(req->driver_path.data), req->driver_path.size,
                  "");
}

// Reads a Suspend request from remote and checks that it is for the expected
// flags, without sending a response. |SendSuspendReply| can be used to send the desired response.
void MultipleDeviceTestCase::CheckSuspendReceived(const zx::channel& remote,
                                                  uint32_t expected_flags) {
  // Read the Suspend request.
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_bytes;
  uint32_t actual_handles;
  zx_status_t status = remote.read(0, bytes, handles, sizeof(bytes), fbl::count_of(handles),
                                   &actual_bytes, &actual_handles);
  ASSERT_OK(status);
  ASSERT_LT(0, actual_bytes);
  ASSERT_EQ(0, actual_handles);

  // Validate the Suspend request.
  auto hdr = reinterpret_cast<fidl_message_header_t*>(bytes);
  ASSERT_EQ(fuchsia_device_manager_DeviceControllerSuspendOrdinal, hdr->ordinal);
  status = fidl_decode(&fuchsia_device_manager_DeviceControllerSuspendRequestTable, bytes,
                       actual_bytes, handles, actual_handles, nullptr);
  ASSERT_OK(status);
  auto req = reinterpret_cast<fuchsia_device_manager_DeviceControllerSuspendRequest*>(bytes);
  ASSERT_EQ(req->flags, expected_flags);
}

// Sends a response with the given return_status. This can be used to reply to a
// request received by |CheckSuspendReceived|.
void MultipleDeviceTestCase::SendSuspendReply(const zx::channel& remote,
                                              zx_status_t return_status) {
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_handles;

  // Write the Suspend response.
  memset(bytes, 0, sizeof(bytes));
  auto resp = reinterpret_cast<fuchsia_device_manager_DeviceControllerSuspendResponse*>(bytes);
  fidl_init_txn_header(&resp->hdr, 0, fuchsia_device_manager_DeviceControllerSuspendOrdinal);
  resp->status = return_status;
  zx_status_t status =
      fidl_encode(&fuchsia_device_manager_DeviceControllerSuspendResponseTable, bytes,
                  sizeof(*resp), handles, fbl::count_of(handles), &actual_handles, nullptr);
  ASSERT_OK(status);
  ASSERT_EQ(0, actual_handles);
  status = remote.write(0, bytes, sizeof(*resp), nullptr, 0);
  ASSERT_OK(status);
}

// Reads a Suspend request from remote, checks that it is for the expected
// flags, and then sends the given response.
void MultipleDeviceTestCase::CheckSuspendReceived(const zx::channel& remote,
                                                  uint32_t expected_flags,
                                                  zx_status_t return_status) {
  CheckSuspendReceived(remote, expected_flags);
  SendSuspendReply(remote, return_status);
}

void MultipleDeviceTestCase::SetUp() {
  ASSERT_NO_FATAL_FAILURES(InitializeCoordinator(&coordinator_));

  // refcount starts at zero, so bump it up to keep us from being cleaned up
  devhost_.AddRef();
  {
    zx::channel local;
    zx_status_t status = zx::channel::create(0, &local, &devhost_remote_);
    ASSERT_OK(status);
    devhost_.set_hrpc(local.release());
  }

  // Set up the sys device proxy, inside of the devhost
  ASSERT_OK(coordinator_.PrepareProxy(coordinator_.sys_device(), &devhost_));
  coordinator_loop_.RunUntilIdle();
  ASSERT_NO_FATAL_FAILURES(
      CheckCreateDeviceReceived(devhost_remote_, kSystemDriverPath, &sys_proxy_remote_));
  coordinator_loop_.RunUntilIdle();

  // Create a child of the sys_device (an equivalent of the platform bus)
  {
    zx::channel local;
    zx_status_t status = zx::channel::create(0, &local, &platform_bus_.remote);
    ASSERT_OK(status);
    status = coordinator_.AddDevice(
        coordinator_.sys_device()->proxy(), std::move(local), nullptr /* props_data */,
        0 /* props_count */, "platform-bus", 0, nullptr /* driver_path */, nullptr /* args */,
        false /* invisible */, zx::channel() /* client_remote */, &platform_bus_.device);
    ASSERT_OK(status);
    coordinator_loop_.RunUntilIdle();
  }
}

void MultipleDeviceTestCase::TearDown() {
  if (!coordinator_loop_thread_running_) {
    coordinator_loop_.RunUntilIdle();
  }
  // Remove the devices in the opposite order that we added them
  while (!devices_.is_empty()) {
    devices_.pop_back();
    if (!coordinator_loop_thread_running_) {
      coordinator_loop_.RunUntilIdle();
    }
  }
  platform_bus_.device.reset();
  if (!coordinator_loop_thread_running_) {
    coordinator_loop_.RunUntilIdle();
  }

  devhost_.devices().clear();
  // We no longer need the async loop.
  // If we do not shutdown here, the destructor
  // could be cleaning up the vfs, before the loop clears the
  // connections.
  coordinator_loop_.Shutdown();
}

void MultipleDeviceTestCase::AddDevice(const fbl::RefPtr<devmgr::Device>& parent, const char* name,
                                       uint32_t protocol_id, fbl::String driver, size_t* index) {
  DeviceState state;

  zx::channel local;
  zx_status_t status = zx::channel::create(0, &local, &state.remote);
  ASSERT_OK(status);
  status = coordinator_.AddDevice(
      parent, std::move(local), nullptr /* props_data */, 0 /* props_count */, name, protocol_id,
      driver.data() /* driver_path */, nullptr /* args */, false /* invisible */,
      zx::channel() /* client_remote */, &state.device);
  state.device->flags |= DEV_CTX_ALLOW_MULTI_COMPOSITE;
  ASSERT_OK(status);
  coordinator_loop_.RunUntilIdle();

  devices_.push_back(std::move(state));
  *index = devices_.size() - 1;
}

void MultipleDeviceTestCase::RemoveDevice(size_t device_index) {
  auto& state = devices_[device_index];
  ASSERT_OK(coordinator_.RemoveDevice(state.device, false));
  state.device.reset();
  state.remote.reset();
  coordinator_loop_.RunUntilIdle();
}

bool MultipleDeviceTestCase::DeviceHasPendingMessages(const zx::channel& remote) {
  return remote.wait_one(ZX_CHANNEL_READABLE, zx::time(0), nullptr) == ZX_OK;
}
bool MultipleDeviceTestCase::DeviceHasPendingMessages(size_t device_index) {
  return DeviceHasPendingMessages(devices_[device_index].remote);
}

void MultipleDeviceTestCase::DoSuspend(uint32_t flags,
                                       fit::function<void(uint32_t flags)> suspend_cb) {
  const bool vfs_exit_expected = (flags != DEVICE_SUSPEND_FLAG_SUSPEND_RAM);
  if (vfs_exit_expected) {
    zx::unowned_event event(coordinator()->fshost_event());
    auto thrd_func = [](void* ctx) -> int {
      zx::unowned_event event(*static_cast<zx::unowned_event*>(ctx));
      if (event->wait_one(FSHOST_SIGNAL_EXIT, zx::time::infinite(), nullptr) != ZX_OK) {
        return false;
      }
      if (event->signal(0, FSHOST_SIGNAL_EXIT_DONE) != ZX_OK) {
        return false;
      }
      return true;
    };

    thrd_t fshost_thrd;
    ASSERT_EQ(thrd_create(&fshost_thrd, thrd_func, &event), thrd_success);

    suspend_cb(flags);
    if (!coordinator_loop_thread_running()) {
      coordinator_loop()->RunUntilIdle();
    }
    int thread_status;
    ASSERT_EQ(thrd_join(fshost_thrd, &thread_status), thrd_success);
    ASSERT_TRUE(thread_status);

    // Make sure that vfs_exit() happened.
    ASSERT_OK(
        coordinator()->fshost_event().wait_one(FSHOST_SIGNAL_EXIT_DONE, zx::time(0), nullptr));
  } else {
    suspend_cb(flags);
    if (!coordinator_loop_thread_running()) {
      coordinator_loop()->RunUntilIdle();
    }

    // Make sure that vfs_exit() didn't happen.
    ASSERT_EQ(coordinator()->fshost_event().wait_one(FSHOST_SIGNAL_EXIT | FSHOST_SIGNAL_EXIT_DONE,
                                                     zx::time(0), nullptr),
              ZX_ERR_TIMED_OUT);
  }
}

void MultipleDeviceTestCase::DoSuspend(uint32_t flags) {
  DoSuspend(flags, [this](uint32_t flags) { coordinator()->Suspend(flags); });
}

// Reads the request from |remote| and verifies whether it matches the expected Unbind request.
// |SendUnbindReply| can be used to send the desired response.
void MultipleDeviceTestCase::CheckUnbindReceived(const zx::channel& remote) {
  // Read the unbind request.
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_bytes;
  uint32_t actual_handles;
  zx_status_t status = remote.read(0, bytes, handles, sizeof(bytes), fbl::count_of(handles),
                                   &actual_bytes, &actual_handles);
  ASSERT_OK(status);
  ASSERT_LT(0, actual_bytes);
  ASSERT_EQ(0, actual_handles);

  // Validate the unbind request.
  auto hdr = reinterpret_cast<fidl_message_header_t*>(bytes);
  ASSERT_EQ(fuchsia_device_manager_DeviceControllerUnbindOrdinal, hdr->ordinal);
  status = fidl_decode(&fuchsia_device_manager_DeviceControllerUnbindRequestTable, bytes,
                       actual_bytes, handles, actual_handles, nullptr);
  ASSERT_OK(status);
}

// Sends a response with the given return_status. This can be used to reply to a
// request received by |CheckUnbindReceived|.
void MultipleDeviceTestCase::SendUnbindReply(const zx::channel& remote) {
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_handles;
  // Write the UnbindDone message.
  memset(bytes, 0, sizeof(bytes));
  auto req = reinterpret_cast<fuchsia_device_manager_CoordinatorUnbindDoneRequest*>(bytes);
  fidl_init_txn_header(&req->hdr, 1, fuchsia_device_manager_CoordinatorUnbindDoneOrdinal);
  zx_status_t status =
      fidl_encode(&fuchsia_device_manager_CoordinatorUnbindDoneRequestTable, bytes, sizeof(*req),
                  handles, fbl::count_of(handles), &actual_handles, nullptr);
  ASSERT_OK(status);
  ASSERT_EQ(0, actual_handles);
  status = remote.write(0, bytes, sizeof(*req), nullptr, 0);
  ASSERT_OK(status);

  coordinator_loop()->RunUntilIdle();

  // Verify the UnbindDone response.
  uint32_t actual_bytes;
  status = remote.read(0, bytes, handles, sizeof(bytes), fbl::count_of(handles), &actual_bytes,
                       &actual_handles);
  ASSERT_OK(status);
  ASSERT_LT(0, actual_bytes);
  ASSERT_EQ(0, actual_handles);

  fidl::EncodedMessage<::llcpp::fuchsia::device::manager::Coordinator::UnbindDoneResponse> encoded(
      fidl::BytePart(bytes, actual_bytes, actual_bytes));
  auto decode_result = fidl::Decode(std::move(encoded));
  ASSERT_OK(decode_result.status);

  const ::llcpp::fuchsia::device::manager::Coordinator::UnbindDoneResponse& msg =
      *decode_result.message.message();
  ASSERT_FALSE(msg.result.is_err());
}

void MultipleDeviceTestCase::CheckUnbindReceivedAndReply(const zx::channel& remote) {
  CheckUnbindReceived(remote);
  SendUnbindReply(remote);
}

// Reads the request from |remote| and verifies whether it matches the expected
// CompleteRemoval request.
// |SendRemoveReply| can be used to send the desired response.
void MultipleDeviceTestCase::CheckRemoveReceived(const zx::channel& remote) {
  // Read the remove request.
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_bytes;
  uint32_t actual_handles;
  zx_status_t status = remote.read(0, bytes, handles, sizeof(bytes), fbl::count_of(handles),
                                   &actual_bytes, &actual_handles);
  ASSERT_OK(status);
  ASSERT_LT(0, actual_bytes);
  ASSERT_EQ(0, actual_handles);

  // Validate the remove request.
  auto hdr = reinterpret_cast<fidl_message_header_t*>(bytes);
  ASSERT_EQ(fuchsia_device_manager_DeviceControllerCompleteRemovalOrdinal, hdr->ordinal);
  status = fidl_decode(&fuchsia_device_manager_DeviceControllerCompleteRemovalRequestTable, bytes,
                       actual_bytes, handles, actual_handles, nullptr);
  ASSERT_OK(status);
}

// Sends a response with the given return_status. This can be used to reply to a
// request received by |CheckRemoveReceived|.
void MultipleDeviceTestCase::SendRemoveReply(const zx::channel& remote) {
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_handles;
  // Write the RemoveDone message.
  memset(bytes, 0, sizeof(bytes));
  auto req = reinterpret_cast<fuchsia_device_manager_CoordinatorRemoveDoneRequest*>(bytes);
  zx_txid_t txid = 1;
  fidl_init_txn_header(&req->hdr, txid, fuchsia_device_manager_CoordinatorRemoveDoneOrdinal);
  zx_status_t status =
      fidl_encode(&fuchsia_device_manager_CoordinatorRemoveDoneRequestTable, bytes, sizeof(*req),
                  handles, fbl::count_of(handles), &actual_handles, nullptr);
  ASSERT_OK(status);
  ASSERT_EQ(0, actual_handles);
  status = remote.write(0, bytes, sizeof(*req), nullptr, 0);
  ASSERT_OK(status);

  coordinator_loop()->RunUntilIdle();

  // Verify the RemoveDone response.
  uint32_t actual_bytes;
  status = remote.read(0, bytes, handles, sizeof(bytes), fbl::count_of(handles), &actual_bytes,
                       &actual_handles);
  ASSERT_OK(status);
  ASSERT_LT(0, actual_bytes);
  ASSERT_EQ(0, actual_handles);

  fidl::EncodedMessage<::llcpp::fuchsia::device::manager::Coordinator::RemoveDoneResponse> encoded(
      fidl::BytePart(bytes, actual_bytes, actual_bytes));
  auto decode_result = fidl::Decode(std::move(encoded));
  ASSERT_OK(decode_result.status);

  const ::llcpp::fuchsia::device::manager::Coordinator::RemoveDoneResponse& msg =
      *decode_result.message.message();
  ASSERT_FALSE(msg.result.is_err());
}

void MultipleDeviceTestCase::CheckRemoveReceivedAndReply(const zx::channel& remote) {
  CheckRemoveReceived(remote);
  SendRemoveReply(remote);
}

// Reads a Resume request from remote and checks that it is for the expected
// target state, without sending a response. |SendResumeReply| can be used to send the desired
// response.
void MultipleDeviceTestCase::CheckResumeReceived(const zx::channel& remote,
                                                 SystemPowerState target_state) {
  // Read the Resume request.
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_bytes;
  uint32_t actual_handles;
  zx_status_t status = remote.read(0, bytes, handles, sizeof(bytes), fbl::count_of(handles),
                                   &actual_bytes, &actual_handles);
  ASSERT_OK(status);
  ASSERT_LT(0, actual_bytes);
  ASSERT_EQ(0, actual_handles);

  // Validate the Resume request.
  auto hdr = reinterpret_cast<fidl_message_header_t*>(bytes);
  ASSERT_EQ(fuchsia_device_manager_DeviceControllerResumeOrdinal, hdr->ordinal);
  status = fidl_decode(&fuchsia_device_manager_DeviceControllerResumeRequestTable, bytes,
                       actual_bytes, handles, actual_handles, nullptr);
  ASSERT_OK(status);
  auto req = reinterpret_cast<fuchsia_device_manager_DeviceControllerResumeRequest*>(bytes);
  ASSERT_EQ(static_cast<SystemPowerState>(req->target_system_state), target_state);
}

// Sends a response with the given return_status. This can be used to reply to a
// request received by |CheckResumeReceived|.
void MultipleDeviceTestCase::SendResumeReply(const zx::channel& remote, zx_status_t return_status) {
  FIDL_ALIGNDECL uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t actual_handles;

  // Write the Resume response.
  memset(bytes, 0, sizeof(bytes));
  auto resp = reinterpret_cast<fuchsia_device_manager_DeviceControllerResumeResponse*>(bytes);
  fidl_init_txn_header(&resp->hdr, 0, fuchsia_device_manager_DeviceControllerResumeOrdinal);
  resp->status = return_status;
  zx_status_t status =
      fidl_encode(&fuchsia_device_manager_DeviceControllerResumeResponseTable, bytes, sizeof(*resp),
                  handles, fbl::count_of(handles), &actual_handles, nullptr);
  ASSERT_OK(status);
  ASSERT_EQ(0, actual_handles);
  status = remote.write(0, bytes, sizeof(*resp), nullptr, 0);
  ASSERT_OK(status);
}

// Reads a Resume request from remote, checks that it is for the expected
// target state, and then sends the given response.
void MultipleDeviceTestCase::CheckResumeReceived(const zx::channel& remote,
                                                 SystemPowerState target_state,
                                                 zx_status_t return_status) {
  CheckResumeReceived(remote, target_state);
  SendResumeReply(remote, return_status);
}

void MultipleDeviceTestCase::DoResume(
    SystemPowerState target_state, fit::function<void(SystemPowerState target_state)> resume_cb) {
  resume_cb(target_state);
  if (!coordinator_loop_thread_running()) {
    coordinator_loop()->RunUntilIdle();
  }
}

void MultipleDeviceTestCase::DoResume(SystemPowerState target_state) {
  DoResume(target_state,
           [this](SystemPowerState target_state) { coordinator()->Resume(target_state); });
}
