// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "driver_host.h"

#include <lib/fdio/spawn.h>
#include <zircon/status.h>

#include "coordinator.h"
#include "src/devices/lib/log/log.h"

DriverHost::DriverHost(Coordinator* coordinator, zx::channel rpc, zx::process proc)
    : coordinator_(coordinator), hrpc_(std::move(rpc)), proc_(std::move(proc)) {
  // cache the process's koid
  zx_info_handle_basic_t info;
  if (proc_.is_valid() &&
      proc_.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr) == ZX_OK) {
    koid_ = info.koid;
  }

  coordinator_->RegisterDriverHost(this);
}

DriverHost::~DriverHost() {
  coordinator_->UnregisterDriverHost(this);
  proc_.kill();
  LOGF(INFO, "Destroyed driver_host %p", this);
}

zx_status_t DriverHost::Launch(Coordinator* coordinator,
                               const LoaderServiceConnector& loader_connector,
                               const char* driver_host_bin, const char* proc_name,
                               const char* const* proc_env, const zx::resource& root_resource,
                               zx::unowned_job driver_host_job, FsProvider* fs_provider,
                               fbl::RefPtr<DriverHost>* out) {
  zx::channel hrpc, dh_hrpc;
  zx_status_t status = zx::channel::create(0, &hrpc, &dh_hrpc);
  if (status != ZX_OK) {
    return status;
  }

  // Give driver_hosts the root resource if we have it (in tests, we may not)
  // TODO: limit root resource to root driver_host only
  zx::resource resource;
  if (root_resource.is_valid()) {
    zx_status_t status = root_resource.duplicate(ZX_RIGHT_SAME_RIGHTS, &resource);
    if (status != ZX_OK) {
      LOGF(ERROR, "Failed to duplicate root resource: %s", zx_status_get_string(status));
    }
  }

  zx::channel loader_connection;
  status = loader_connector(&loader_connection);
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to get driver_host loader connection: %s", zx_status_get_string(status));
    return status;
  }

  constexpr size_t kMaxActions = 5;
  fdio_spawn_action_t actions[kMaxActions];
  size_t actions_count = 0;
  actions[actions_count++] =
      fdio_spawn_action_t{.action = FDIO_SPAWN_ACTION_SET_NAME, .name = {.data = proc_name}};
  // TODO: constrain to /svc/device
  actions[actions_count++] = fdio_spawn_action_t{
      .action = FDIO_SPAWN_ACTION_ADD_NS_ENTRY,
      .ns = {.prefix = "/svc", .handle = fs_provider->CloneFs("svc").release()},
  };
  actions[actions_count++] = fdio_spawn_action_t{
      .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
      .h = {.id = PA_HND(PA_USER0, 0), .handle = hrpc.release()},
  };
  if (resource.is_valid()) {
    actions[actions_count++] = fdio_spawn_action_t{
        .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
        .h = {.id = PA_HND(PA_RESOURCE, 0), .handle = resource.release()},
    };
  }

  actions[actions_count++] = fdio_spawn_action_t{
      .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
      .h = {.id = PA_HND(PA_LDSVC_LOADER, 0), .handle = loader_connection.release()},
  };
  ZX_ASSERT(actions_count <= kMaxActions);

  zx::process proc;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
  // Inherit devmgr's environment (including kernel cmdline)
  const char* const argv[] = {
      driver_host_bin,
      nullptr,
  };
  const auto flags = FDIO_SPAWN_CLONE_ENVIRON | FDIO_SPAWN_CLONE_STDIO;
  status = fdio_spawn_etc(driver_host_job->get(), flags, argv[0], argv, proc_env, actions_count,
                          actions, proc.reset_and_get_address(), err_msg);
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to launch driver_host '%s': %s", proc_name, err_msg);
    return status;
  }

  auto host = fbl::MakeRefCounted<DriverHost>(coordinator, std::move(dh_hrpc), std::move(proc));
  LOGF(INFO, "Launching driver_host '%s' (pid %zu)", proc_name, host->koid());
  *out = std::move(host);
  return ZX_OK;
}
