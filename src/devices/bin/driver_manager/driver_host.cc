// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "driver_host.h"

#include <lib/fdio/spawn-actions.h>
#include <lib/fdio/spawn.h>
#include <zircon/status.h>

#include <vector>

#include "coordinator.h"
#include "fdio.h"
#include "src/devices/lib/log/log.h"
#include "src/lib/storage/vfs/cpp/remote_dir.h"

DriverHost::DriverHost(Coordinator* coordinator, zx::channel rpc, zx::channel diagnostics,
                       zx::process proc)
    : coordinator_(coordinator), hrpc_(std::move(rpc)), proc_(std::move(proc)) {
  // cache the process's koid
  zx_info_handle_basic_t info;
  if (proc_.is_valid() &&
      proc_.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr) == ZX_OK) {
    koid_ = info.koid;
  }

  coordinator_->RegisterDriverHost(this);
  driver_host_dir_ = coordinator_->inspect_manager().driver_host_dir();
  if (diagnostics.is_valid()) {
    driver_host_dir_->AddEntry(std::to_string(koid_),
                               fbl::MakeRefCounted<fs::RemoteDir>(std::move(diagnostics)));
  }
}

DriverHost::~DriverHost() {
  coordinator_->UnregisterDriverHost(this);
  driver_host_dir_->RemoveEntry(std::to_string(koid_));
  proc_.kill();
  LOGF(INFO, "Destroyed driver_host %p", this);
}

zx_status_t DriverHost::Launch(const DriverHostConfig& config, fbl::RefPtr<DriverHost>* out) {
  zx::channel hrpc, dh_hrpc;
  zx_status_t status = zx::channel::create(0, &hrpc, &dh_hrpc);
  if (status != ZX_OK) {
    return status;
  }

  zx::channel diagnostics_client, diagnostics_server;
  status = zx::channel::create(0, &diagnostics_client, &diagnostics_server);
  if (status != ZX_OK) {
    return status;
  }

  // Give driver_hosts the root resource if we have it (in tests, we may not)
  // TODO: limit root resource to root driver_host only
  zx::resource resource;
  if (config.root_resource->is_valid()) {
    zx_status_t status = config.root_resource->duplicate(ZX_RIGHT_SAME_RIGHTS, &resource);
    if (status != ZX_OK) {
      LOGF(ERROR, "Failed to duplicate root resource: %s", zx_status_get_string(status));
    }
  }

  FdioSpawnActions fdio_spawn_actions;
  fdio_spawn_actions.AddAction(
      fdio_spawn_action_t{.action = FDIO_SPAWN_ACTION_SET_NAME, .name = {.data = config.name}});

  auto fs_object = config.fs_provider->CloneFs("driver_host_svc");
  fdio_spawn_actions.AddActionWithNamespace(
      fdio_spawn_action_t{
          .action = FDIO_SPAWN_ACTION_ADD_NS_ENTRY,
          .ns = {.prefix = "/svc"},
      },
      std::move(fs_object));

  fdio_spawn_actions.AddActionWithHandle(
      fdio_spawn_action_t{
          .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
          .h = {.id = PA_HND(PA_USER0, 0)},
      },
      std::move(hrpc));

  if (resource.is_valid()) {
    fdio_spawn_actions.AddActionWithHandle(
        fdio_spawn_action_t{
            .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
            .h = {.id = PA_HND(PA_RESOURCE, 0)},
        },
        std::move(resource));
  }

  uint32_t flags = FDIO_SPAWN_CLONE_ENVIRON | FDIO_SPAWN_CLONE_STDIO | FDIO_SPAWN_CLONE_UTC_CLOCK;
  if (!*config.loader_service_connector) {
    flags |= FDIO_SPAWN_DEFAULT_LDSVC;
  } else {
    zx::channel loader_service_client;
    status = (*config.loader_service_connector)(&loader_service_client);
    if (status != ZX_OK) {
      LOGF(ERROR, "Failed connect to loader service: %s", zx_status_get_string(status));
      return status;
    }

    fdio_spawn_actions.AddActionWithHandle(
        fdio_spawn_action_t{
            .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
            .h = {.id = PA_HND(PA_LDSVC_LOADER, 0)},
        },
        std::move(loader_service_client));
  }

  fdio_spawn_actions.AddActionWithHandle(
      fdio_spawn_action_t{
          .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
          .h = {.id = PA_DIRECTORY_REQUEST},
      },
      std::move(diagnostics_server));

  zx::process proc;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
  const char* const argv[] = {config.binary, nullptr};
  std::vector<fdio_spawn_action_t> actions = fdio_spawn_actions.GetActions();
  status = fdio_spawn_etc(config.job->get(), flags, argv[0], argv, config.env, actions.size(),
                          actions.data(), proc.reset_and_get_address(), err_msg);
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to launch driver_host '%s': %s", config.name, err_msg);
    return status;
  }

  auto host = fbl::MakeRefCounted<DriverHost>(config.coordinator, std::move(dh_hrpc),
                                              std::move(diagnostics_client), std::move(proc));
  LOGF(INFO, "Launching driver_host '%s' (pid %zu)", config.name, host->koid());
  *out = std::move(host);
  return ZX_OK;
}
