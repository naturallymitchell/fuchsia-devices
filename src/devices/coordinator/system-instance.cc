// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "system-instance.h"

#include <fcntl.h>
#include <fuchsia/hardware/virtioconsole/llcpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/namespace.h>
#include <lib/fdio/spawn.h>
#include <lib/fdio/unsafe.h>
#include <lib/fdio/watcher.h>
#include <lib/zx/debuglog.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zircon/boot/image.h>
#include <zircon/status.h>
#include <zircon/syscalls/log.h>
#include <zircon/syscalls/policy.h>

#include <fbl/string_printf.h>
#include <fbl/unique_fd.h>
#include <fbl/vector.h>

#include "devfs.h"
#include "fdio.h"
#include "log.h"

struct ConsoleStarterArgs {
  SystemInstance* instance;
  devmgr::BootArgs* boot_args;
};

// Wait for the requested file.  Its parent directory must exist.
zx_status_t wait_for_file(const char* path, zx::time deadline) {
  char path_copy[PATH_MAX];
  if (strlen(path) >= PATH_MAX) {
    return ZX_ERR_INVALID_ARGS;
  }
  strcpy(path_copy, path);

  char* last_slash = strrchr(path_copy, '/');
  // Waiting on the root of the fs or paths with no slashes is not supported by this function
  if (last_slash == path_copy || last_slash == nullptr) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  last_slash[0] = 0;
  char* dirname = path_copy;
  char* basename = last_slash + 1;

  auto watch_func = [](int dirfd, int event, const char* fn, void* cookie) -> zx_status_t {
    auto basename = static_cast<const char*>(cookie);
    if (event != WATCH_EVENT_ADD_FILE) {
      return ZX_OK;
    }
    if (!strcmp(fn, basename)) {
      return ZX_ERR_STOP;
    }
    return ZX_OK;
  };

  fbl::unique_fd dirfd(open(dirname, O_RDONLY));
  if (!dirfd.is_valid()) {
    return ZX_ERR_INVALID_ARGS;
  }
  zx_status_t status = fdio_watch_directory(dirfd.get(), watch_func, deadline.get(),
                                            reinterpret_cast<void*>(basename));
  if (status == ZX_ERR_STOP) {
    return ZX_OK;
  }
  return status;
}

// Get ramdisk from the boot items service.
zx_status_t get_ramdisk(zx::vmo* ramdisk_vmo) {
  zx::channel local, remote;
  zx_status_t status = zx::channel::create(0, &local, &remote);
  if (status != ZX_OK) {
    return status;
  }
  status = fdio_service_connect(kItemsPath, remote.release());
  if (status != ZX_OK) {
    return status;
  }
  uint32_t length;
  return fuchsia_boot_ItemsGet(local.get(), ZBI_TYPE_STORAGE_RAMDISK, 0,
                               ramdisk_vmo->reset_and_get_address(), &length);
}

SystemInstance::SystemInstance() : fshost_ldsvc(zx::channel()), launcher_(this) {}

zx_status_t SystemInstance::CreateSvcJob(const zx::job& root_job) {
  zx_status_t status = zx::job::create(root_job, 0u, &svc_job);
  if (status != ZX_OK) {
    fprintf(stderr, "devcoordinator: failed to create service job: %s\n",
            zx_status_get_string(status));
    return 1;
  }
  status = svc_job.set_property(ZX_PROP_NAME, "zircon-services", 16);
  if (status != ZX_OK) {
    fprintf(stderr, "devcoordinator: failed to set service job name: %s\n",
            zx_status_get_string(status));
    return 1;
  }

  return ZX_OK;
}

zx_status_t SystemInstance::CreateFuchsiaJob(const zx::job& root_job) {
  zx_status_t status = zx::job::create(root_job, 0u, &fuchsia_job);
  if (status != ZX_OK) {
    printf("devcoordinator: unable to create fuchsia job: %d (%s)\n", status,
           zx_status_get_string(status));
    return status;
  }

  fuchsia_job.set_property(ZX_PROP_NAME, "fuchsia", 7);

  const zx_policy_basic_t basic_policy[] = {
      // Lock down process creation. Child tasks must use fuchsia.process.Launcher.
      {.condition = ZX_POL_NEW_PROCESS, .policy = ZX_POL_ACTION_DENY}};

  status = fuchsia_job.set_policy(ZX_JOB_POL_RELATIVE, ZX_JOB_POL_BASIC, basic_policy,
                                  fbl::count_of(basic_policy));
  if (status != ZX_OK) {
    printf("devcoordinator: unable to set basic policy for fuchsia job: %d (%s)\n", status,
           zx_status_get_string(status));
    return status;
  }

  // Set the minimum timer slack amount and default mode. The amount should be large enough to
  // allow for some coalescing of timers, but small enough to ensure applications don't miss
  // deadlines.
  //
  // Why LATE and not CENTER or EARLY? Timers firing a little later than requested is not uncommon
  // in non-realtime systems. Programs are generally tolerant of some delays. However, timers
  // firing before their dealine can be unexpected and lead to bugs.
  const zx_policy_timer_slack_t timer_slack_policy{ZX_USEC(500), ZX_TIMER_SLACK_LATE};

  status =
      fuchsia_job.set_policy(ZX_JOB_POL_RELATIVE, ZX_JOB_POL_TIMER_SLACK, &timer_slack_policy, 1);
  if (status != ZX_OK) {
    printf("devcoordinator: unable to set timer slack policy for fuchsia job: %d (%s)\n", status,
           zx_status_get_string(status));
    return status;
  }

  return ZX_OK;
}

zx_status_t SystemInstance::PrepareChannels() {
  zx_status_t status;
  status = zx::channel::create(0, &appmgr_client, &appmgr_server);
  if (status != ZX_OK) {
    return status;
  }
  status = zx::channel::create(0, &miscsvc_client, &miscsvc_server);
  if (status != ZX_OK) {
    return status;
  }
  status = zx::channel::create(0, &device_name_provider_client, &device_name_provider_server);
  if (status != ZX_OK) {
    return status;
  }

  return ZX_OK;
}

zx_status_t SystemInstance::StartSvchost(const zx::job& root_job, bool require_system,
                                         devmgr::Coordinator* coordinator,
                                         zx::channel fshost_client) {
  zx::channel dir_request, svchost_local;
  zx_status_t status = zx::channel::create(0, &dir_request, &svchost_local);
  if (status != ZX_OK) {
    return status;
  }

  zx::debuglog logger;
  status = zx::debuglog::create(coordinator->root_resource(), 0, &logger);
  if (status != ZX_OK) {
    return status;
  }

  zx::channel appmgr_svc;
  {
    zx::channel appmgr_svc_req;
    status = zx::channel::create(0, &appmgr_svc_req, &appmgr_svc);
    if (status != ZX_OK) {
      return status;
    }

    status = fdio_service_connect_at(appmgr_client.get(), "svc", appmgr_svc_req.release());
    if (status != ZX_OK) {
      return status;
    }
  }

  zx::job root_job_copy;
  status =
      root_job.duplicate(ZX_RIGHTS_BASIC | ZX_RIGHTS_IO | ZX_RIGHTS_PROPERTY | ZX_RIGHT_ENUMERATE |
                             ZX_RIGHT_MANAGE_PROCESS | ZX_RIGHT_MANAGE_THREAD,
                         &root_job_copy);
  if (status != ZX_OK) {
    return status;
  }

  // TODO(ZX-3530): svchost needs the root resource to talk to
  // zx_debug_send_command. Remove this once zx_debug_send_command no longer
  // requires the root resource.
  zx::resource root_resource_copy;
  if (coordinator->root_resource().is_valid()) {
    status = coordinator->root_resource().duplicate(ZX_RIGHT_TRANSFER, &root_resource_copy);
    if (status != ZX_OK) {
      return status;
    }
  }

  zx::channel coordinator_client;
  {
    zx::channel root_server, root_client;
    status = zx::channel::create(0, &root_server, &root_client);
    if (status != ZX_OK) {
      return status;
    }

    status = coordinator->BindOutgoingServices(std::move(root_server));
    if (status != ZX_OK) {
      return status;
    }

    zx::channel coordinator_server;
    status = zx::channel::create(0, &coordinator_server, &coordinator_client);
    if (status != ZX_OK) {
      return status;
    }

    status = fdio_service_connect_at(root_client.get(), "svc", coordinator_server.release());
    if (status != ZX_OK) {
      return status;
    }
  }

  zx::channel virtcon_client;
  status = zx::channel::create(0, &virtcon_client, &virtcon_fidl);
  if (status != ZX_OK) {
    printf("Unable to create virtcon channel.\n");
    return status;
  }

  zx::channel miscsvc_svc;
  {
    zx::channel miscsvc_svc_req;
    status = zx::channel::create(0, &miscsvc_svc_req, &miscsvc_svc);
    if (status != ZX_OK) {
      return status;
    }

    status = fdio_service_connect_at(miscsvc_client.get(), "svc", miscsvc_svc_req.release());
    if (status != ZX_OK) {
      return status;
    }
  }

  zx::channel device_name_provider_svc;
  {
    zx::channel device_name_provider_svc_req;
    status = zx::channel::create(0, &device_name_provider_svc_req, &device_name_provider_svc);
    if (status != ZX_OK) {
      return status;
    }

    status = fdio_service_connect_at(device_name_provider_client.get(), "svc",
                                     device_name_provider_svc_req.release());
    if (status != ZX_OK) {
      return status;
    }
  }

  zx::channel devcoordinator_svc;
  {
    zx::channel devcoordinator_svc_req;
    status = zx::channel::create(0, &devcoordinator_svc_req, &devcoordinator_svc);
    if (status != ZX_OK) {
      return status;
    }

    // This connects to the /svc in devcoordinator's namespace.
    status = fdio_service_connect("/svc", devcoordinator_svc_req.release());
    if (status != ZX_OK) {
      return status;
    }
  }

  zx::job svc_job_copy;
  status = svc_job.duplicate(ZX_RIGHTS_BASIC | ZX_RIGHT_MANAGE_JOB | ZX_RIGHT_MANAGE_PROCESS,
                             &svc_job_copy);
  if (status != ZX_OK) {
    return status;
  }

  const char* name = "svchost";
  const char* argv[3] = {
      "/boot/bin/svchost",
      require_system ? "--require-system" : nullptr,
      nullptr,
  };

  fbl::Vector<fdio_spawn_action_t> actions;

  actions.push_back((fdio_spawn_action_t){
      .action = FDIO_SPAWN_ACTION_SET_NAME,
      .name = {.data = name},
  });

  actions.push_back((fdio_spawn_action_t){
      .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
      .h = {.id = PA_DIRECTORY_REQUEST, .handle = dir_request.release()},
  });
  actions.push_back((fdio_spawn_action_t){
      .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
      .h = {.id = PA_HND(PA_FD, FDIO_FLAG_USE_FOR_STDIO), .handle = logger.release()},
  });

  // Remove once svchost hosts the fuchsia.tracing.provider service itself.
  actions.push_back((fdio_spawn_action_t){
      .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
      .h = {.id = PA_HND(PA_USER0, 0), .handle = appmgr_svc.release()},
  });

  // Give svchost a restricted root job handle. svchost is already a privileged system service
  // as it controls system-wide process launching. With the root job it can consolidate a few
  // services such as crashsvc and the profile service.
  actions.push_back((fdio_spawn_action_t){
      .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
      .h = {.id = PA_HND(PA_USER0, 1), .handle = root_job_copy.release()},
  });

  // Also give svchost a restricted root resource handle, this allows it to run the kernel-debug
  // service.
  if (root_resource_copy.is_valid()) {
    actions.push_back((fdio_spawn_action_t){
        .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
        .h = {.id = PA_HND(PA_USER0, 2), .handle = root_resource_copy.release()},
    });
  }

  // TODO(smklein): Merge "coordinator_client" (proxying requests to devmgr) and
  // "fshost_client" (proxying requests to fshost) into one service provider
  // PseudoDirectory.

  // Add handle to channel to allow svchost to proxy fidl services to us.
  actions.push_back((fdio_spawn_action_t){
      .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
      .h = {.id = PA_HND(PA_USER0, 3), .handle = coordinator_client.release()},
  });

  // Add a handle to allow svchost to proxy services to fshost.
  actions.push_back((fdio_spawn_action_t){
      .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
      .h = {.id = PA_HND(PA_USER0, 4), .handle = fshost_client.release()},
  });
  if (!coordinator->boot_args().GetBool("virtcon.disable", false)) {
    // Add handle to channel to allow svchost to proxy fidl services to
    // virtcon.
    actions.push_back((fdio_spawn_action_t){
        .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
        .h = {.id = PA_HND(PA_USER0, 5), .handle = virtcon_client.release()},
    });
  }

  // Add handle to channel to allow svchost to talk to miscsvc.
  actions.push_back((fdio_spawn_action_t){
      .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
      .h = {.id = PA_HND(PA_USER0, 6), .handle = miscsvc_svc.release()},
  });

  // Add handle to channel to allow svchost to connect to services from devcoordinator's /svc, which
  // is hosted by component_manager and includes services routed from other components; see
  // "devcoordinator.cml".
  actions.push_back((fdio_spawn_action_t){
      .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
      .h = {.id = PA_HND(PA_USER0, 7), .handle = devcoordinator_svc.release()},
  });

  // Add handle to channel to allow svchost to talk to device_name_provider.
  actions.push_back((fdio_spawn_action_t){
      .action = FDIO_SPAWN_ACTION_ADD_HANDLE,
      .h = {.id = PA_HND(PA_USER0, 8), .handle = device_name_provider_svc.release()},
  });

  // Give svchost access to /dev/class/sysmem, to enable svchost to forward sysmem service
  // requests to the sysmem driver.  Create a namespace containing /dev/class/sysmem.
  zx::channel fs_handle = CloneFs("dev/class/sysmem");
  if (!fs_handle.is_valid()) {
    printf("devcoordinator: failed to clone /dev/class/sysmem\n");
    return ZX_ERR_BAD_STATE;
  }
  actions.push_back((fdio_spawn_action_t){
      .action = FDIO_SPAWN_ACTION_ADD_NS_ENTRY,
      .ns = {.prefix = "/sysmem", .handle = fs_handle.release()},
  });

  char errmsg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
  zx_handle_t proc = ZX_HANDLE_INVALID;
  status = fdio_spawn_etc(svc_job_copy.get(), FDIO_SPAWN_CLONE_JOB | FDIO_SPAWN_DEFAULT_LDSVC,
                          argv[0], argv, NULL, actions.size(), actions.data(), &proc, errmsg);
  if (status != ZX_OK) {
    printf("devcoordinator: launch %s (%s) failed: %s: %d\n", argv[0], name, errmsg, status);
    return status;
  } else {
    printf("devcoordinator: launch %s (%s) OK\n", argv[0], name);
  }

  zx::channel svchost_public_remote;
  status = zx::channel::create(0, &svchost_public_remote, &svchost_outgoing);
  if (status != ZX_OK) {
    return status;
  }

  return fdio_service_connect_at(svchost_local.get(), "svc", svchost_public_remote.release());
}

zx_status_t SystemInstance::ReuseExistingSvchost() {
  // This path is only used in integration tests that start an "isolated" devmgr/devcoordinator.
  // Rather than start another svchost process - which won't work for a couple reasons - we
  // clone the /svc in devcoordinator's namespace when devcoordinator launches other processes.
  // This may or may not work well, depending on the services those processes require and whether
  // they happen to be in the /svc exposed to this test instance of devcoordinator.
  // TODO(bryanhenry): This can go away once we move the processes devcoordinator spawns today out
  // into separate components.
  zx::channel dir_request;
  zx_status_t status = zx::channel::create(0, &dir_request, &svchost_outgoing);
  if (status != ZX_OK) {
    fprintf(stderr, "devcoordinator: failed to create svchost_outgoing channel\n");
    return 1;
  }
  status = fdio_service_connect("/svc", dir_request.release());
  if (status != ZX_OK) {
    fprintf(stderr, "devcoordinator: failed to connect to /svc\n");
    return 1;
  }

  return ZX_OK;
}

void SystemInstance::devmgr_vfs_init(devmgr::Coordinator* coordinator,
                                     const devmgr::DevmgrArgs& devmgr_args,
                                     zx::channel fshost_server) {
  fdio_ns_t* ns;
  zx_status_t r;
  r = fdio_ns_get_installed(&ns);
  ZX_ASSERT_MSG(r == ZX_OK, "devcoordinator: cannot get namespace: %s\n", zx_status_get_string(r));
  r = fdio_ns_bind(ns, "/dev", CloneFs("dev").release());
  ZX_ASSERT_MSG(r == ZX_OK, "devcoordinator: cannot bind /dev to namespace: %s\n",
                zx_status_get_string(r));

  // Start fshost before binding /system, since it publishes it.
  fshost_start(coordinator, devmgr_args, std::move(fshost_server));

  if ((r = fdio_ns_bind(ns, "/system", CloneFs("system").release())) != ZX_OK) {
    printf("devcoordinator: cannot bind /system to namespace: %d\n", r);
  }
}

// Thread entry point
int SystemInstance::pwrbtn_monitor_starter(void* arg) {
  auto args = std::unique_ptr<ServiceStarterArgs>(static_cast<ServiceStarterArgs*>(arg));
  return args->instance->PwrbtnMonitorStarter(args->coordinator);
}

int SystemInstance::PwrbtnMonitorStarter(devmgr::Coordinator* coordinator) {
  const char* name = "pwrbtn-monitor";
  const char* argv[] = {"/boot/bin/pwrbtn-monitor", nullptr};

  zx::job job_copy;
  zx_status_t status =
      svc_job.duplicate(ZX_RIGHTS_BASIC | ZX_RIGHT_READ | ZX_RIGHT_WRITE, &job_copy);
  if (status != ZX_OK) {
    printf("devcoordinator: svc_job.duplicate failed %s\n", zx_status_get_string(status));
    return 1;
  }

  zx::debuglog debuglog;
  if ((status = zx::debuglog::create(coordinator->root_resource(), 0, &debuglog) != ZX_OK)) {
    printf("devcoordinator: cannot create debuglog handle\n");
    return 1;
  }

  zx::channel input_handle = CloneFs("dev/class/input");
  if (!input_handle.is_valid()) {
    printf("devcoordinator: failed to clone /dev/input\n");
    return 1;
  }

  zx::channel svc_handle = CloneFs("svc");
  if (!svc_handle.is_valid()) {
    printf("devcoordinator: failed to clone /svc\n");
    return 1;
  }

  fdio_spawn_action_t actions[] = {
      {.action = FDIO_SPAWN_ACTION_SET_NAME, .name = {.data = name}},
      {.action = FDIO_SPAWN_ACTION_ADD_NS_ENTRY,
       .ns = {.prefix = "/input", .handle = input_handle.release()}},
      // Ideally we'd only expose /svc/fuchsia.device.manager.Administrator, but we do not
      // support exposing single services.
      {.action = FDIO_SPAWN_ACTION_ADD_NS_ENTRY,
       .ns = {.prefix = "/svc", .handle = svc_handle.release()}},
      {.action = FDIO_SPAWN_ACTION_ADD_HANDLE,
       .h = {.id = PA_HND(PA_FD, FDIO_FLAG_USE_FOR_STDIO | 0), .handle = debuglog.release()}},
  };

  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
  uint32_t spawn_flags = FDIO_SPAWN_CLONE_JOB | FDIO_SPAWN_DEFAULT_LDSVC;
  status = fdio_spawn_etc(job_copy.get(), spawn_flags, argv[0], argv, nullptr,
                          fbl::count_of(actions), actions, nullptr, err_msg);
  if (status != ZX_OK) {
    printf("devcoordinator: spawn %s (%s) failed: %s: %s\n", argv[0], name, err_msg,
           zx_status_get_string(status));
    return 1;
  }

  printf("devcoordinator: launch %s (%s) OK\n", argv[0], name);
  return 0;
}

// Thread trampoline for start_console_shell/ConsoleStarter
int console_starter(void* arg) {
  auto args = std::unique_ptr<ConsoleStarterArgs>(static_cast<ConsoleStarterArgs*>(arg));
  return args->instance->ConsoleStarter(args->boot_args);
}

void SystemInstance::start_console_shell(const devmgr::BootArgs& boot_args) {
  // Only start a shell on the kernel console if it isn't already running a shell.
  if (boot_args.GetBool("kernel.shell", false)) {
    return;
  }
  auto args = std::make_unique<ConsoleStarterArgs>();
  args->instance = this;
  args->boot_args = const_cast<devmgr::BootArgs*>(&boot_args);
  thrd_t t;
  int ret = thrd_create_with_name(&t, console_starter, args.release(), "console-starter");
  if (ret == thrd_success) {
    thrd_detach(t);
  }
}

int SystemInstance::ConsoleStarter(const devmgr::BootArgs* arg) {
  auto& boot_args = *arg;

  // If we got a TERM environment variable (aka a TERM=... argument on
  // the kernel command line), pass this down; otherwise pass TERM=uart.
  const char* term = boot_args.Get("TERM");
  if (term == nullptr) {
    term = "TERM=uart";
  } else {
    term -= sizeof("TERM=") - 1;
  }

  const char* device = boot_args.Get("console.path");
  if (device == nullptr) {
    device = "/svc/console";
  }

  const char* envp[] = {
      term,
      nullptr,
  };

  // Run thread forever, relaunching console shell on exit.
  for (;;) {
    zx_status_t status = wait_for_file(device, zx::time::infinite());
    if (status != ZX_OK) {
      printf("devcoordinator: failed to wait for console '%s' (%s)\n", device,
             zx_status_get_string(status));
      return 1;
    }
    fbl::unique_fd fd(open(device, O_RDWR));
    if (!fd.is_valid()) {
      printf("devcoordinator: failed to open console '%s'\n", device);
      return 1;
    }

    // TODO(ZX-3385): Clean this up once devhost stops speaking fuchsia.io.File
    // on behalf of drivers.  Once that happens, the virtio-console driver
    // should just speak that instead of this shim interface.
    if (boot_args.GetBool("console.is_virtio", false)) {
      // If the console is a virtio connection, then speak the
      // fuchsia.hardware.virtioconsole.Device interface to get the real
      // fuchsia.io.File connection
      zx::channel virtio_channel;
      status = fdio_get_service_handle(fd.release(), virtio_channel.reset_and_get_address());
      if (status != ZX_OK) {
        printf("devcoordinator: failed to get console handle '%s'\n", device);
        return 1;
      }

      zx::channel local, remote;
      status = zx::channel::create(0, &local, &remote);
      if (status != ZX_OK) {
        printf("devcoordinator: failed to create channel for console '%s'\n", device);
        return 1;
      }

      ::llcpp::fuchsia::hardware::virtioconsole::Device::SyncClient virtio_client(
          std::move(virtio_channel));
      virtio_client.GetChannel(std::move(remote));

      fdio_t* fdio;
      status = fdio_create(local.release(), &fdio);
      if (status != ZX_OK) {
        printf("devcoordinator: failed to setup fdio for console '%s'\n", device);
        return 1;
      }

      fd.reset(fdio_bind_to_fd(fdio, -1, 3));
      if (!fd.is_valid()) {
        fdio_unsafe_release(fdio);
        printf("devcoordinator: failed to transfer fdio for console '%s'\n", device);
        return 1;
      }
    }

    zx::channel ldsvc;
    status = clone_fshost_ldsvc(&ldsvc);
    if (status != ZX_OK) {
      fprintf(stderr, "devcoordinator: failed to clone fshost loader for console: %d\n", status);
      return 1;
    }

    const char* argv_sh[] = {"/boot/bin/sh", nullptr};
    zx::process proc;
    status = launcher_.LaunchWithLoader(svc_job, "sh:console", zx::vmo(), std::move(ldsvc), argv_sh,
                                        envp, fd.release(), nullptr, nullptr, 0, &proc, FS_ALL);
    if (status != ZX_OK) {
      printf("devcoordinator: failed to launch console shell (%s)\n", zx_status_get_string(status));
      return 1;
    }

    status = proc.wait_one(ZX_PROCESS_TERMINATED, zx::time::infinite(), nullptr);
    if (status != ZX_OK) {
      printf("devcoordinator: failed to wait for console shell termination (%s)\n",
             zx_status_get_string(status));
      return 1;
    }
    zx_info_process_t proc_info;
    status = proc.get_info(ZX_INFO_PROCESS, &proc_info, sizeof(proc_info), nullptr, nullptr);
    if (status != ZX_OK) {
      printf("devcoordinator: failed to determine console shell termination cause (%s)\n",
             zx_status_get_string(status));
      return 1;
    }
    printf(
        "devcoordinator: console shell exited (started=%d exited=%d, return_code=%ld), "
        "restarting\n",
        proc_info.started, proc_info.exited, proc_info.return_code);
  }
  /* NOTREACHED */
  return 0;
}

// Thread trampoline for ServiceStarter
int SystemInstance::service_starter(void* arg) {
  auto args = std::unique_ptr<ServiceStarterArgs>(static_cast<ServiceStarterArgs*>(arg));
  return args->instance->ServiceStarter(args->coordinator);
}

// Thread trampoline for FuchsiaStarter, which ServiceStarter spawns
int fuchsia_starter(void* arg) {
  auto args = std::unique_ptr<SystemInstance::ServiceStarterArgs>(
      static_cast<SystemInstance::ServiceStarterArgs*>(arg));
  return args->instance->FuchsiaStarter(args->coordinator);
}

int SystemInstance::ServiceStarter(devmgr::Coordinator* coordinator) {
  // Launch miscsvc binary with access to:
  // * /dev to talk to hardware
  // * /boot to dynamically load drivers (zxcrypt)
  // * /svc to call launch processes (minfs)
  // * /volume to mount (minfs)
  const zx_handle_t handles[] = {miscsvc_server.release()};
  const uint32_t types[] = {PA_DIRECTORY_REQUEST};
  const char* args[] = {"/boot/bin/miscsvc", nullptr};

  {
    // TODO(34633): miscsvc needs access to /boot/lib/asan when devcoordinator runs in isolated
    // devmgr mode.
    zx::channel ldsvc;
    zx_status_t status = clone_fshost_ldsvc(&ldsvc);
    if (status != ZX_OK) {
      fprintf(stderr, "devcoordinator: failed to clone loader for miscsvc: %d\n", status);
      return -1;
    }

    launcher_.LaunchWithLoader(svc_job, "miscsvc", zx::vmo(), std::move(ldsvc), args, nullptr, -1,
                               handles, types, countof(handles), nullptr,
                               FS_BOOT | FS_DEV | FS_SVC | FS_VOLUME);
  }

  bool netboot = false;
  bool vruncmd = false;
  fbl::String vcmd;
  const char* interface = coordinator->boot_args().Get("netsvc.interface");
  if (!(coordinator->boot_args().GetBool("netsvc.disable", false) ||
        coordinator->disable_netsvc())) {
    const char* args[] = {"/boot/bin/netsvc", nullptr, nullptr, nullptr, nullptr, nullptr};
    int argc = 1;

    if (coordinator->boot_args().GetBool("netsvc.netboot", false)) {
      args[argc++] = "--netboot";
      netboot = true;
      vruncmd = true;
    }

    if (coordinator->boot_args().GetBool("netsvc.advertise", true)) {
      args[argc++] = "--advertise";
    }

    if (interface != nullptr) {
      args[argc++] = "--interface";
      args[argc++] = interface;
    }

    zx::process proc;
    zx_status_t status =
        launcher_.Launch(svc_job, "netsvc", args, nullptr, -1, nullptr, nullptr, 0, &proc, FS_ALL);
    if (status == ZX_OK) {
      if (vruncmd) {
        zx_info_handle_basic_t info = {};
        proc.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
        proc.reset();
        vcmd = fbl::StringPrintf("dlog -f -t -p %zu", info.koid);
      }
    } else {
      vruncmd = false;
    }
    __UNUSED auto leaked_handle = proc.release();
  }

  {
    // Launch device-name-provider with access to /dev, to discover network interfaces.
    const zx_handle_t handles[] = {device_name_provider_server.release()};
    const uint32_t types[] = {PA_DIRECTORY_REQUEST};
    const char* nodename = coordinator->boot_args().Get("zircon.nodename");
    const char* args[] = {
        "/boot/bin/device-name-provider", nullptr, nullptr, nullptr, nullptr, nullptr};
    int argc = 1;

    if (interface != nullptr) {
      args[argc++] = "--interface";
      args[argc++] = interface;
    }

    if (nodename != nullptr) {
      args[argc++] = "--nodename";
      args[argc++] = nodename;
    }

    launcher_.Launch(svc_job, "device-name-provider", args, nullptr, -1, handles, types,
                     countof(handles), nullptr, FS_DEV);
  }

  if (!coordinator->boot_args().GetBool("virtcon.disable", false)) {
    // pass virtcon.* options along
    fbl::Vector<const char*> env;
    coordinator->boot_args().Collect("virtcon.", &env);
    env.push_back(nullptr);

    const char* num_shells = coordinator->require_system() && !netboot ? "0" : "3";
    size_t handle_count = 0;
    zx_handle_t handles[2];
    uint32_t types[2];

    handles[handle_count] = virtcon_fidl.release();
    types[handle_count] = PA_HND(PA_USER0, 0);
    ++handle_count;

    zx::debuglog debuglog;
    zx_status_t status =
        zx::debuglog::create(coordinator->root_resource(), ZX_LOG_FLAG_READABLE, &debuglog);
    if (status == ZX_OK) {
      handles[handle_count] = debuglog.release();
      types[handle_count] = PA_HND(PA_USER0, 1);
      ++handle_count;
    }

    const char* args[] = {
        "/boot/bin/virtual-console", "--shells", num_shells, nullptr, nullptr, nullptr};
    if (vruncmd) {
      args[3] = "--run";
      args[4] = vcmd.data();
    }
    launcher_.Launch(svc_job, "virtual-console", args, env.data(), -1, handles, types, handle_count,
                     nullptr, FS_ALL);
  }

  const char* backstop = coordinator->boot_args().Get("clock.backstop");
  if (backstop) {
    zx_time_t offset = ZX_SEC(atoi(backstop));
    printf("devcoordinator: setting UTC backstop: %ld\n", offset);
    zx_clock_adjust(coordinator->root_resource().get(), ZX_CLOCK_UTC, offset);
  }

  do_autorun("autorun:boot", coordinator->boot_args().Get("zircon.autorun.boot"));

  auto starter_args = std::make_unique<ServiceStarterArgs>();
  starter_args->instance = this;
  starter_args->coordinator = coordinator;
  thrd_t t;
  int ret = thrd_create_with_name(&t, fuchsia_starter, starter_args.release(), "fuchsia-starter");
  if (ret == thrd_success) {
    thrd_detach(t);
  }

  return 0;
}

int SystemInstance::FuchsiaStarter(devmgr::Coordinator* coordinator) {
  bool appmgr_started = false;
  bool autorun_started = false;
  bool drivers_loaded = false;

  size_t appmgr_timeout = 20;
  zx::time deadline = zx::deadline_after(zx::sec(appmgr_timeout));

  do {
    zx_status_t status =
        coordinator->fshost_event().wait_one(FSHOST_SIGNAL_READY, deadline, nullptr);
    if (status == ZX_ERR_TIMED_OUT) {
      if (appmgr_server.is_valid()) {
        if (coordinator->require_system()) {
          fprintf(stderr, "devcoordinator: appmgr not launched in %zus, closing appmgr handle\n",
                  appmgr_timeout);
        }
        appmgr_server.reset();
      }
      deadline = zx::time::infinite();
      continue;
    }
    if (status != ZX_OK) {
      fprintf(stderr, "devcoordinator: error waiting on fuchsia start event: %d\n", status);
      break;
    }
    status = coordinator->fshost_event().signal(FSHOST_SIGNAL_READY, 0);
    if (status != ZX_OK) {
      fprintf(stderr, "devcoordinator: error signaling fshost: %d\n", status);
    }

    if (!drivers_loaded) {
      // we're starting the appmgr because /system is present
      // so we also signal the device coordinator that those
      // drivers are now loadable
      coordinator->set_system_available(true);
      coordinator->ScanSystemDrivers();
      drivers_loaded = true;
    }

    const char* argv_appmgr[] = {
        "/system/bin/appmgr",
        nullptr,
    };

    struct stat s;
    if (!appmgr_started && stat(argv_appmgr[0], &s) == 0) {
      zx::channel ldsvc;
      zx_status_t status = clone_fshost_ldsvc(&ldsvc);
      if (status != ZX_OK) {
        fprintf(stderr, "devcoordinator: failed to clone fshost loader for appmgr: %d\n", status);
        continue;
      }

      unsigned int appmgr_hnd_count = 0;
      zx_handle_t appmgr_hnds[2] = {};
      uint32_t appmgr_ids[2] = {};
      if (appmgr_server.is_valid()) {
        ZX_ASSERT(appmgr_hnd_count < fbl::count_of(appmgr_hnds));
        appmgr_hnds[appmgr_hnd_count] = appmgr_server.release();
        appmgr_ids[appmgr_hnd_count] = PA_DIRECTORY_REQUEST;
        appmgr_hnd_count++;
      }
      launcher_.LaunchWithLoader(fuchsia_job, "appmgr", zx::vmo(), std::move(ldsvc), argv_appmgr,
                                 nullptr, -1, appmgr_hnds, appmgr_ids, appmgr_hnd_count, nullptr,
                                 FS_FOR_APPMGR);
      appmgr_started = true;
    }
    if (!autorun_started) {
      do_autorun("autorun:system", coordinator->boot_args().Get("zircon.autorun.system"));
      autorun_started = true;
    }
  } while (!appmgr_started);
  return 0;
}

// TODO(ZX-4860): DEPRECATED. Do not add new dependencies on the fshost loader service!
zx_status_t SystemInstance::clone_fshost_ldsvc(zx::channel* loader) {
  // Only valid to call this after fshost_ldsvc has been wired up.
  ZX_ASSERT_MSG(fshost_ldsvc.has_value(), "clone_fshost_ldsvc called too early");

  zx::channel remote;
  zx_status_t status = zx::channel::create(0, loader, &remote);
  if (status != ZX_OK) {
    return status;
  }
  auto result = fshost_ldsvc->Clone(std::move(remote));
  if (result.status() != ZX_OK) {
    return result.status();
  }
  return result.Unwrap()->rv;
}

void SystemInstance::do_autorun(const char* name, const char* cmd) {
  if (cmd != nullptr) {
    auto args = devmgr::ArgumentVector::FromCmdline(cmd);
    args.Print("autorun");

    zx::channel ldsvc;
    zx_status_t status = clone_fshost_ldsvc(&ldsvc);
    if (status != ZX_OK) {
      fprintf(stderr, "devcoordinator: failed to clone fshost loader for console: %d\n", status);
      return;
    }

    launcher_.LaunchWithLoader(svc_job, name, zx::vmo(), std::move(ldsvc), args.argv(), nullptr, -1,
                               nullptr, nullptr, 0, nullptr, FS_ALL);
  }
}

void SystemInstance::fshost_start(devmgr::Coordinator* coordinator,
                                  const devmgr::DevmgrArgs& devmgr_args,
                                  zx::channel fshost_server) {
  // assemble handles to pass down to fshost
  zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
  uint32_t types[fbl::count_of(handles)];
  size_t n = 0;

  // Pass server ends of fs_root and ldsvc handles to fshost.
  zx::channel fs_root_server;
  if (zx::channel::create(0, &fs_root, &fs_root_server) == ZX_OK) {
    handles[n] = fs_root_server.release();
    types[n++] = PA_HND(PA_USER0, 0);
  }
  zx::channel ldsvc_client, ldsvc_server;
  if (zx::channel::create(0, &ldsvc_client, &ldsvc_server) == ZX_OK) {
    handles[n] = ldsvc_server.release();
    types[n++] = PA_HND(PA_USER0, 2);
  }
  fshost_ldsvc.emplace(std::move(ldsvc_client));

  // The "public directory" of the fshost service.
  handles[n] = fshost_server.release();
  types[n++] = PA_HND(PA_USER0, 3);

  // pass fuchsia start event to fshost
  zx::event fshost_event_duplicate;
  if (coordinator->fshost_event().duplicate(ZX_RIGHT_SAME_RIGHTS, &fshost_event_duplicate) ==
      ZX_OK) {
    handles[n] = fshost_event_duplicate.release();
    types[n++] = PA_HND(PA_USER1, 0);
  }

  // pass VDSO VMOS to fshost
  for (uint32_t m = 0; n < fbl::count_of(handles); m++) {
    uint32_t type = PA_HND(PA_VMO_VDSO, m);
    handles[n] = zx_take_startup_handle(type);

    if (handles[n] != ZX_HANDLE_INVALID) {
      types[n++] = type;
    } else {
      break;
    }
  }

  // pass ramdisk to fshost
  zx::vmo ramdisk_vmo;
  zx_status_t status = get_ramdisk(&ramdisk_vmo);
  if (status == ZX_OK && ramdisk_vmo.is_valid()) {
    handles[n] = ramdisk_vmo.release();
    types[n++] = PA_HND(PA_VMO_BOOTDATA, 0);
  }

  // pass command line to the fshost
  fbl::Vector<const char*> args{"/boot/bin/fshost"};
  if (coordinator->boot_args().GetBool("netsvc.netboot", false) ||
      coordinator->boot_args().GetBool("zircon.system.disable-automount", false)) {
    args.push_back("--netboot");
  }
  if (devmgr_args.disable_block_watcher) {
    args.push_back("--disable-block-watcher");
  }
  args.push_back(nullptr);

  // pass zircon.system.* options to the fshost as environment variables
  fbl::Vector<const char*> env;
  coordinator->boot_args().Collect("zircon.system", &env);
  env.push_back(nullptr);

  launcher_.Launch(svc_job, "fshost", args.data(), env.data(), -1, handles, types, n, nullptr,
                   FS_BOOT | FS_DEV | FS_SVC);
}

zx::channel SystemInstance::CloneFs(const char* path) {
  if (!strcmp(path, "dev")) {
    return devmgr::devfs_root_clone();
  }
  zx::channel h0, h1;
  if (zx::channel::create(0, &h0, &h1) != ZX_OK) {
    return zx::channel();
  }
  if (!strcmp(path, "boot")) {
    zx_status_t status = fdio_open("/boot", ZX_FS_RIGHT_READABLE, h1.release());
    if (status != ZX_OK) {
      log(ERROR, "devcoordinator: fdio_open(\"/boot\") failed: %s\n", zx_status_get_string(status));
      return zx::channel();
    }
    return h0;
  }
  zx::unowned_channel fs(fs_root);
  int flags = FS_DIR_FLAGS;
  if (!strcmp(path, "hub")) {
    fs = zx::unowned_channel(appmgr_client);
  } else if (!strcmp(path, "svc")) {
    flags = ZX_FS_RIGHT_READABLE | ZX_FS_RIGHT_WRITABLE;
    fs = zx::unowned_channel(svchost_outgoing);
    path = ".";
  } else if (!strncmp(path, "dev/", 4)) {
    fs = devmgr::devfs_root_borrow();
    path += 4;
  }
  zx_status_t status = fdio_open_at(fs->get(), path, flags, h1.release());
  if (status != ZX_OK) {
    log(ERROR, "devcoordinator: fdio_open_at failed for path %s: %s\n", path,
        zx_status_get_string(status));
    return zx::channel();
  }
  return h0;
}
