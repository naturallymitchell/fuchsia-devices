// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "driver.h"

#include <dirent.h>
#include <fcntl.h>
#include <fuchsia/io/llcpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <new>
#include <string>

#include <ddk/binding.h>
#include <driver-info/driver-info.h>
#include <fbl/string_printf.h>

#include "env.h"
#include "fdio.h"
#include "src/devices/lib/log/log.h"

namespace {

namespace fio = llcpp::fuchsia::io;

struct AddContext {
  const char* libname;
  DriverLoadCallback func;
  // This is optional. If present, holds the driver shared library that was loaded ephemerally.
  zx::vmo vmo;
};

bool is_driver_disabled(const char* name) {
  // driver.<driver_name>.disable
  auto option = fbl::StringPrintf("driver.%s.disable", name);
  return getenv_bool(option.data(), false);
}

void found_driver(zircon_driver_note_payload_t* note, const zx_bind_inst_t* bi, void* cookie) {
  auto context = static_cast<AddContext*>(cookie);

  // ensure strings are terminated
  note->name[sizeof(note->name) - 1] = 0;
  note->vendor[sizeof(note->vendor) - 1] = 0;
  note->version[sizeof(note->version) - 1] = 0;

  if (is_driver_disabled(note->name)) {
    return;
  }

  auto drv = std::make_unique<Driver>();
  if (drv == nullptr) {
    return;
  }

  auto binding = std::make_unique<zx_bind_inst_t[]>(note->bindcount);
  if (binding == nullptr) {
    return;
  }
  const size_t bindlen = note->bindcount * sizeof(zx_bind_inst_t);
  memcpy(binding.get(), bi, bindlen);
  drv->binding.reset(binding.release());
  drv->binding_size = static_cast<uint32_t>(bindlen);

  drv->flags = note->flags;
  drv->libname.Set(context->libname);
  drv->name.Set(note->name);

  if (context->vmo.is_valid()) {
    drv->dso_vmo = std::move(context->vmo);
  }

  VLOGF(2, "Found driver: %s", (char*)cookie);
  VLOGF(2, "        name: %s", note->name);
  VLOGF(2, "      vendor: %s", note->vendor);
  VLOGF(2, "     version: %s", note->version);
  VLOGF(2, "       flags: %#x", note->flags);
  VLOGF(2, "     binding:");
  for (size_t n = 0; n < note->bindcount; n++) {
    VLOGF(2, "         %03zd: %08x %08x", n, bi[n].op, bi[n].arg);
  }

  context->func(drv.release(), note->version);
}

}  // namespace

void find_loadable_drivers(const std::string& path, DriverLoadCallback func) {
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) {
    return;
  }
  AddContext context = {"", std::move(func), zx::vmo{}};

  struct dirent* de;
  while ((de = readdir(dir)) != nullptr) {
    if (de->d_name[0] == '.') {
      continue;
    }
    if (de->d_type != DT_REG) {
      continue;
    }
    auto libname = fbl::StringPrintf("%s/%s", path.c_str(), de->d_name);
    context.libname = libname.data();

    int fd = openat(dirfd(dir), de->d_name, O_RDONLY);
    if (fd < 0) {
      continue;
    }
    zx_status_t status = di_read_driver_info(fd, &context, found_driver);
    close(fd);

    if (status == ZX_ERR_NOT_FOUND) {
      LOGF(INFO, "Missing info from driver '%s'", libname.data());
    } else if (status != ZX_OK) {
      LOGF(ERROR, "Failed to read info from driver '%s': %s", libname.data(),
           zx_status_get_string(status));
    }
  }
  closedir(dir);
}

zx_status_t load_driver_vmo(std::string_view libname, zx::vmo vmo, DriverLoadCallback func) {
  zx_handle_t vmo_handle = vmo.get();
  AddContext context = {libname.data(), std::move(func), std::move(vmo)};

  auto di_vmo_read = [](void* vmo, void* data, size_t len, size_t off) {
    return zx_vmo_read(*((zx_handle_t*)vmo), data, off, len);
  };
  zx_status_t status = di_read_driver_info_etc(&vmo_handle, di_vmo_read, &context, found_driver);

  if (status == ZX_ERR_NOT_FOUND) {
    LOGF(INFO, "Missing info from driver '%s'", libname);
  } else if (status != ZX_OK) {
    LOGF(ERROR, "Failed to read info from driver '%s': %s", libname, zx_status_get_string(status));
  }
  return status;
}

zx_status_t load_vmo(std::string_view libname, zx::vmo* out_vmo) {
  int fd = -1;
  zx_status_t r = fdio_open_fd(
      libname.data(), fio::wire::OPEN_RIGHT_READABLE | fio::wire::OPEN_RIGHT_EXECUTABLE, &fd);
  if (r != ZX_OK) {
    LOGF(ERROR, "Cannot open driver '%s'", libname.data());
    return ZX_ERR_IO;
  }
  zx::vmo vmo;
  r = fdio_get_vmo_exec(fd, vmo.reset_and_get_address());
  close(fd);
  if (r != ZX_OK) {
    LOGF(ERROR, "Cannot get driver VMO '%s'", libname.data());
    return r;
  }
  const char* vmo_name = strrchr(libname.data(), '/');
  if (vmo_name != nullptr) {
    ++vmo_name;
  } else {
    vmo_name = libname.data();
  }
  r = vmo.set_property(ZX_PROP_NAME, vmo_name, strlen(vmo_name));
  if (r != ZX_OK) {
    LOGF(ERROR, "Cannot set name on driver VMO to '%s'", libname.data());
    return r;
  }
  *out_vmo = std::move(vmo);
  return r;
}

void load_driver(const char* path, DriverLoadCallback func) {
  // TODO: check for duplicate driver add
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    LOGF(ERROR, "Cannot open driver '%s'", path);
    return;
  }

  AddContext context = {path, std::move(func), zx::vmo{}};
  zx_status_t status = di_read_driver_info(fd, &context, found_driver);
  close(fd);

  if (status == ZX_ERR_NOT_FOUND) {
    LOGF(INFO, "Missing info from driver '%s'", path);
  } else if (status != ZX_OK) {
    LOGF(ERROR, "Failed to read info from driver '%s': %s", path, zx_status_get_string(status));
  }
}
