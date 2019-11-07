// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zx-device.h"

#include <stdio.h>

#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>

#include "composite-device.h"
#include "devhost.h"

zx_status_t zx_device::Create(fbl::RefPtr<zx_device>* out_dev) {
  *out_dev = fbl::AdoptRef(new zx_device());
  return ZX_OK;
}

void zx_device::set_bind_conn(const fs::FidlConnection& conn) {
  fbl::AutoLock<fbl::Mutex> lock(&bind_conn_lock_);
  bind_conn_ = conn;
}

void zx_device::set_rebind_conn(const fs::FidlConnection& conn) {
  fbl::AutoLock<fbl::Mutex> lock(&rebind_conn_lock_);
  rebind_conn_ = conn;
}

bool zx_device::take_rebind_conn_and_clear(fs::FidlConnection* conn) {
  fbl::AutoLock<fbl::Mutex> lock(&rebind_conn_lock_);
  if (!rebind_conn_.has_value()) {
    return false;
  }
  *conn = rebind_conn_.value();
  rebind_conn_.reset();
  return true;
}

void zx_device::set_rebind_drv_name(const char* drv_name) {
  rebind_drv_name_ = std::string(drv_name);
}

bool zx_device::get_bind_conn_and_clear(fs::FidlConnection* conn) {
  fbl::AutoLock<fbl::Mutex> lock(&bind_conn_lock_);
  if (!bind_conn_.has_value()) {
    return false;
  }
  *conn = bind_conn_.value();
  bind_conn_.reset();
  return true;
}

void zx_device::PushTestCompatibilityConn(const fs::FidlConnection& conn) {
  fbl::AutoLock<fbl::Mutex> lock(&test_compatibility_conn_lock_);
  test_compatibility_conn_.push_back(conn);
}

bool zx_device::PopTestCompatibilityConn(fs::FidlConnection* conn) {
  fbl::AutoLock<fbl::Mutex> lock(&test_compatibility_conn_lock_);
  if (test_compatibility_conn_.is_empty()) {
    return false;
  }
  *conn = test_compatibility_conn_[0];
  test_compatibility_conn_.erase(0);
  return true;
}

const std::array<fuchsia_device_DevicePerformanceStateInfo,
                 fuchsia_device_MAX_DEVICE_PERFORMANCE_STATES>&
zx_device::GetPerformanceStates() const {
  return performance_states_;
}

const std::array<fuchsia_device_DevicePowerStateInfo, fuchsia_device_MAX_DEVICE_POWER_STATES>&
zx_device::GetPowerStates() const {
  return power_states_;
}

const std::array<fuchsia_device_SystemPowerStateInfo,
                 fuchsia_device_manager_MAX_SYSTEM_POWER_STATES>&
zx_device::GetSystemPowerStateMapping() const {
  return system_power_states_mapping_;
}

zx_status_t zx_device::SetPowerStates(const device_power_state_info_t* power_states,
                                      uint8_t count) {
  if (count < fuchsia_device_MIN_DEVICE_POWER_STATES ||
      count > fuchsia_device_MAX_DEVICE_POWER_STATES) {
    return ZX_ERR_INVALID_ARGS;
  }
  bool visited[fuchsia_device_MAX_DEVICE_POWER_STATES] = {false};
  for (uint8_t i = 0; i < count; i++) {
    const auto& info = power_states[i];
    if (info.state_id >= fbl::count_of(visited)) {
      return ZX_ERR_INVALID_ARGS;
    }
    if (visited[info.state_id]) {
      return ZX_ERR_INVALID_ARGS;
    }
    fuchsia_device_DevicePowerStateInfo* state = &(power_states_[info.state_id]);
    state->state_id = info.state_id;
    state->is_supported = true;
    state->restore_latency = info.restore_latency;
    state->wakeup_capable = info.wakeup_capable;
    state->system_wake_state = info.system_wake_state;
    visited[info.state_id] = true;
  }
  if (!(power_states_[fuchsia_device_DevicePowerState_DEVICE_POWER_STATE_D0].is_supported) ||
      !(power_states_[fuchsia_device_DevicePowerState_DEVICE_POWER_STATE_D3COLD].is_supported)) {
    return ZX_ERR_INVALID_ARGS;
  }
  return ZX_OK;
}
zx_status_t zx_device::SetPerformanceStates(
    const device_performance_state_info_t* performance_states, uint8_t count) {
  if (count < fuchsia_device_MIN_DEVICE_PERFORMANCE_STATES ||
      count > fuchsia_device_MAX_DEVICE_PERFORMANCE_STATES) {
    return ZX_ERR_INVALID_ARGS;
  }
  bool visited[fuchsia_device_MAX_DEVICE_PERFORMANCE_STATES] = {false};
  for (uint8_t i = 0; i < count; i++) {
    const auto& info = performance_states[i];
    if (info.state_id >= fbl::count_of(visited)) {
      return ZX_ERR_INVALID_ARGS;
    }
    if (visited[info.state_id]) {
      return ZX_ERR_INVALID_ARGS;
    }
    fuchsia_device_DevicePerformanceStateInfo* state =
        &(performance_states_[info.state_id]);
    state->state_id = info.state_id;
    state->is_supported = true;
    state->restore_latency = info.restore_latency;
    visited[info.state_id] = true;
  }
  if (!(performance_states_[fuchsia_device_DEVICE_PERFORMANCE_STATE_P0].is_supported)) {
    return ZX_ERR_INVALID_ARGS;
  }
  return ZX_OK;
}

zx_status_t zx_device::SetSystemPowerStateMapping(
    const std::array<fuchsia_device_SystemPowerStateInfo,
                     fuchsia_device_manager_MAX_SYSTEM_POWER_STATES>& mapping) {
  for (size_t i = 0; i < fuchsia_device_manager_MAX_SYSTEM_POWER_STATES; i++) {
    const fuchsia_device_SystemPowerStateInfo* info = &mapping[i];
    if (!(power_states_[info->dev_state].is_supported)) {
      return ZX_ERR_INVALID_ARGS;
    }
    if (info->wakeup_enable && !power_states_[info->dev_state].wakeup_capable) {
      return ZX_ERR_INVALID_ARGS;
    }
    // TODO(ravoorir): Validate whether the system can wake up from that state,
    // when power states make more sense. Currently we cannot compare the
    // system sleep power states.
    system_power_states_mapping_[i] = mapping[i];
  }
  return ZX_OK;
}

// We must disable thread-safety analysis due to not being able to statically
// guarantee the lock holding invariant.  Instead, we acquire the lock if
// it's not already being held by the current thread.
void zx_device::fbl_recycle() TA_NO_THREAD_SAFETY_ANALYSIS {
  bool acq_lock = !devmgr::DM_LOCK_HELD();
  if (acq_lock) {
    devmgr::DM_LOCK();
  }
  auto unlock = fbl::MakeAutoCall([acq_lock]() TA_NO_THREAD_SAFETY_ANALYSIS {
    if (acq_lock) {
      devmgr::DM_UNLOCK();
    }
  });

  if (this->flags & DEV_FLAG_INSTANCE) {
    // these don't get removed, so mark dead state here
    this->flags |= DEV_FLAG_DEAD;
  }
  if (this->flags & DEV_FLAG_BUSY) {
    // this can happen if creation fails
    // the caller to device_add() will free it
    printf("device: %p(%s): ref=0, busy, not releasing\n", this, this->name);
    return;
  }
#if TRACE_ADD_REMOVE
  printf("device: %p(%s): ref=0. releasing.\n", this, this->name);
#endif

  if (!(this->flags & DEV_FLAG_DEAD)) {
    printf("device: %p(%s): not yet dead (this is bad)\n", this, this->name);
  }
  if (!this->children.is_empty()) {
    printf("device: %p(%s): still has children! not good.\n", this, this->name);
  }

  composite_.reset();
  this->event.reset();
  this->local_event.reset();

  // Put on the defered work list for finalization
  devmgr::defer_device_list.push_back(this);

  // Immediately finalize if there's not an active enumerator
  if (devmgr::devhost_enumerators == 0) {
    devmgr::devhost_finalize();
  }
}

static fbl::Mutex local_id_map_lock_;
static fbl::WAVLTree<uint64_t, fbl::RefPtr<zx_device>, zx_device::LocalIdKeyTraits,
                     zx_device::LocalIdNode>
    local_id_map_ TA_GUARDED(local_id_map_lock_);

void zx_device::set_local_id(uint64_t id) {
  // If this is the last reference, we want it to go away outside of the lock
  fbl::RefPtr<zx_device> old_entry;

  fbl::AutoLock guard(&local_id_map_lock_);
  if (local_id_ != 0) {
    old_entry = local_id_map_.erase(*this);
    ZX_ASSERT(old_entry.get() == this);
  }

  local_id_ = id;
  if (id != 0) {
    local_id_map_.insert(fbl::RefPtr(this));
  }
}

fbl::RefPtr<zx_device> zx_device::GetDeviceFromLocalId(uint64_t local_id) {
  fbl::AutoLock guard(&local_id_map_lock_);
  auto itr = local_id_map_.find(local_id);
  if (itr == local_id_map_.end()) {
    return nullptr;
  }
  return fbl::RefPtr(&*itr);
}

bool zx_device::has_composite() { return !!composite_; }

fbl::RefPtr<devmgr::CompositeDevice> zx_device::take_composite() { return std::move(composite_); }

void zx_device::set_composite(fbl::RefPtr<devmgr::CompositeDevice> composite) {
  composite_ = std::move(composite);
}
