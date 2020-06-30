// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "inspect.h"

#include <ddk/driver.h>
#include <driver-info/driver-info.h>
#include <fs/vfs_types.h>
#include <fs/vmo_file.h>
#include <fs/vnode.h>

#include "device.h"
#include "init_task.h"
#include "resume_task.h"
#include "suspend_task.h"
#include "unbind_task.h"

zx::status<> InspectDevfs::InitInspectFile(const fbl::RefPtr<Device>& dev) {
  if (dev->inspect_file() != nullptr) {
    return zx::error(ZX_ERR_INTERNAL);
  }

  if (!dev->inspect().vmo()) {
    // Device doesn't have an inspect VMO to publish.
    return zx::ok();
  }

  dev->inspect_file() = fbl::MakeRefCounted<fs::VmoFile>(dev->inspect().vmo(), 0, ZX_PAGE_SIZE);
  return zx::ok();
}

zx::status<> InspectDevfs::Publish(const fbl::RefPtr<Device>& dev) {
  if (!dev->inspect().vmo()) {
    // Device doesn't have an inspect VMO to publish.
    return zx::ok();
  }

  if (dev->inspect_file() == nullptr) {
    return zx::error(ZX_ERR_INTERNAL);
  }

  return AddClassDirEntry(dev);
}

zx::status<> InspectDevfs::InitInspectFileAndPublish(const fbl::RefPtr<Device>& dev) {
  zx::status<> status = InitInspectFile(dev);
  if (status.is_error()) {
    return status.take_error();
  }
  return Publish(dev);
}

// TODO(surajmalhotra): Ideally this would take a RefPtr, but currently this is
// invoked in the dtor for Device.
void InspectDevfs::Unpublish(Device* dev) {
  // Remove reference in class directory if it exists
  auto [dir, seqcount] = GetProtoDir(dev->protocol_id());
  if (dir != nullptr) {
    dir->RemoveEntry(dev->link_name(), dev->inspect_file().get());
  }

  dev->inspect_file() = nullptr;
}

InspectDevfs::InspectDevfs(const fbl::RefPtr<fs::PseudoDir>& root_dir) : root_dir_(root_dir) {
  std::copy(std::begin(kProtoInfos), std::end(kProtoInfos), proto_infos_.begin());
}

zx::status<InspectDevfs> InspectDevfs::Create(const fbl::RefPtr<fs::PseudoDir>& root_dir) {
  InspectDevfs devfs(root_dir);

  zx::status<> status = devfs.PrepopulateProtocolDirs();
  if (status.is_error()) {
    return status.take_error();
  }

  return zx::ok(std::move(devfs));
}

zx::status<> InspectDevfs::PrepopulateProtocolDirs() {
  auto class_devnode = fbl::MakeRefCounted<fs::PseudoDir>();
  zx::status<> status = zx::make_status(root_dir_->AddEntry("class", class_devnode));
  if (status.is_error()) {
    return status.take_error();
  }

  for (auto& info : proto_infos_) {
    if (!(info.flags & PF_NOPUB)) {
      auto node = fbl::MakeRefCounted<fs::PseudoDir>();
      auto status = zx::make_status(class_devnode->AddEntry(info.name, node));
      if (status.is_error()) {
        return status.take_error();
      }
      info.devnode = std::move(node);
    }
  }
  return zx::ok();
}

std::tuple<fbl::RefPtr<fs::PseudoDir>, uint32_t*> InspectDevfs::GetProtoDir(uint32_t id) {
  for (auto& info : proto_infos_) {
    if (info.id == id) {
      return {info.devnode, &info.seqcount};
    }
  }
  return {nullptr, nullptr};
}

zx::status<> InspectDevfs::AddClassDirEntry(const fbl::RefPtr<Device>& dev) {
  // Create link in /dev/class/... if this id has a published class
  auto [dir, seqcount] = GetProtoDir(dev->protocol_id());
  if (dir == nullptr) {
    // No class dir for this type, so ignore it
    return zx::ok();
  }

  char tmp[32];
  const char* name = nullptr;

  if (dev->protocol_id() != ZX_PROTOCOL_CONSOLE) {
    for (unsigned n = 0; n < 1000; n++) {
      snprintf(tmp, sizeof(tmp), "%03u", ((*seqcount)++) % 1000);
      fbl::RefPtr<fs::Vnode> node;
      if (dir->Lookup(&node, tmp) == ZX_ERR_NOT_FOUND) {
        name = tmp;
        break;
      }
    }
    if (name == nullptr) {
      return zx::error(ZX_ERR_ALREADY_EXISTS);
    }
  } else {
    name = dev->name().data();
  }

  zx::status<> status = zx::make_status(dir->AddEntry(name, dev->inspect_file()));
  if (status.is_error()) {
    return status;
  }
  dev->set_link_name(name);
  return zx::ok();
}

InspectManager::InspectManager(async_dispatcher_t* dispatcher) {
  inspect_vmo_ = inspect_.DuplicateVmo();

  diagnostics_dir_ = fbl::MakeRefCounted<fs::PseudoDir>();
  auto driver_manager_dir = fbl::MakeRefCounted<fs::PseudoDir>();
  diagnostics_dir_->AddEntry("driver_manager", driver_manager_dir);

  driver_host_dir_ = fbl::MakeRefCounted<fs::PseudoDir>();
  driver_manager_dir->AddEntry("driver_host", driver_host_dir_);

  uint64_t vmo_size;
  ZX_ASSERT(inspect_vmo_.get_size(&vmo_size) == ZX_OK);

  auto vmo_file = fbl::MakeRefCounted<fs::VmoFile>(inspect_vmo_, 0, vmo_size);
  driver_manager_dir->AddEntry("dm.inspect", vmo_file);

  auto status = InspectDevfs::Create(diagnostics_dir_);
  ZX_ASSERT(status.is_ok());
  devfs_ = std::move(status.value());

  if (dispatcher) {
    zx::channel local;
    zx::channel::create(0, &diagnostics_client_, &local);
    diagnostics_vfs_ = std::make_unique<fs::SynchronousVfs>(dispatcher);
    diagnostics_vfs_->ServeDirectory(diagnostics_dir_, std::move(local));
  }

  devices_ = root_node().CreateChild("devices");
  device_count_ = root_node().CreateUint("device_count", 0);
}

DeviceInspect::DeviceInspect(inspect::Node& devices, inspect::UintProperty& device_count,
                             std::string name, zx::vmo inspect_vmo)
    : device_count_node_(device_count), vmo_(std::move(inspect_vmo)) {
  device_node_ = devices.CreateChild(name);
  // Increment device count.
  device_count_node_.Add(1);

  // create properties with default values
  state_ = device_node_.CreateString("state", "");
  local_id_ = device_node_.CreateUint("driver_host_local_id", 0);
}

DeviceInspect::~DeviceInspect() {
  // Decrement device count.
  device_count_node_.Subtract(1);
}

void DeviceInspect::set_properties(const fbl::Array<const zx_device_prop_t>& props) {
  inspect::Node properties_array;

  // Add a node only if there are any `props`
  if (props.size()) {
    properties_array = device_node_.CreateChild("properties");
  }

  for (uint32_t i = 0; i < props.size(); ++i) {
    const zx_device_prop_t* p = &props[i];
    const char* param_name = di_bind_param_name(p->id);
    auto property = properties_array.CreateChild(std::to_string(i));
    property.CreateUint("value", p->value, &static_values_);
    if (param_name) {
      property.CreateString("id", param_name, &static_values_);
    } else {
      property.CreateString("id", std::to_string(p->id), &static_values_);
    }
    static_values_.emplace(std::move(property));
  }

  // Place the node into value list as props will not change in the lifetime of the device.
  if (props.size()) {
    static_values_.emplace(std::move(properties_array));
  }
}
