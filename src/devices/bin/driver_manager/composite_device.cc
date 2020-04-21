// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "composite_device.h"

#include <zircon/status.h>

#include <utility>

#include "binding_internal.h"
#include "coordinator.h"
#include "fidl.h"
#include "src/devices/lib/log/log.h"

// CompositeDevice methods

CompositeDevice::CompositeDevice(fbl::String name, fbl::Array<const zx_device_prop_t> properties,
                                 uint32_t fragments_count, uint32_t coresident_device_index,
                                 fbl::Array<std::unique_ptr<Metadata>> metadata)
    : name_(std::move(name)),
      properties_(std::move(properties)),
      fragments_count_(fragments_count),
      coresident_device_index_(coresident_device_index),
      metadata_(std::move(metadata)) {}

CompositeDevice::~CompositeDevice() = default;

zx_status_t CompositeDevice::Create(
    const fbl::StringPiece& name,
    llcpp::fuchsia::device::manager::CompositeDeviceDescriptor comp_desc,
    std::unique_ptr<CompositeDevice>* out) {
  fbl::String name_obj(name);
  fbl::Array<zx_device_prop_t> properties(new zx_device_prop_t[comp_desc.props.count()],
                                          comp_desc.props.count());
  memcpy(properties.data(), comp_desc.props.data(),
         comp_desc.props.count() * sizeof(comp_desc.props.data()[0]));

  fbl::Array<std::unique_ptr<Metadata>> metadata(
      new std::unique_ptr<Metadata>[comp_desc.metadata.count()], comp_desc.metadata.count());

  for (size_t i = 0; i < comp_desc.metadata.count(); i++) {
    std::unique_ptr<Metadata> md;
    zx_status_t status = Metadata::Create(comp_desc.metadata[i].data.count(), &md);
    if (status != ZX_OK) {
      return status;
    }

    md->type = comp_desc.metadata[i].key;
    md->length = comp_desc.metadata[i].data.count();
    memcpy(md->Data(), comp_desc.metadata[i].data.data(), md->length);
    metadata[i] = std::move(md);
  }

  auto dev = std::make_unique<CompositeDevice>(
      std::move(name), std::move(properties), comp_desc.fragments.count(),
      comp_desc.coresident_device_index, std::move(metadata));
  for (uint32_t i = 0; i < comp_desc.fragments.count(); ++i) {
    const auto& fidl_fragment = comp_desc.fragments[i];
    size_t parts_count = fidl_fragment.parts_count;
    fbl::Array<FragmentPartDescriptor> parts(new FragmentPartDescriptor[parts_count], parts_count);
    for (size_t j = 0; j < parts_count; ++j) {
      const auto& fidl_part = fidl_fragment.parts[j];
      size_t program_count = fidl_part.match_program_count;
      fbl::Array<zx_bind_inst_t> match_program(new zx_bind_inst_t[program_count], program_count);
      for (size_t k = 0; k < program_count; ++k) {
        match_program[k] = zx_bind_inst_t{
            .op = fidl_part.match_program[k].op,
            .arg = fidl_part.match_program[k].arg,
        };
      }
      parts[j] = {std::move(match_program)};
    }

    auto fragment = std::make_unique<CompositeDeviceFragment>(dev.get(), i, std::move(parts));
    dev->unbound_.push_back(std::move(fragment));
  }
  *out = std::move(dev);
  return ZX_OK;
}

bool CompositeDevice::TryMatchFragments(const fbl::RefPtr<Device>& dev, size_t* index_out) {
  for (auto itr = bound_.begin(); itr != bound_.end(); ++itr) {
    if (itr->TryMatch(dev)) {
      LOGF(ERROR, "Ambiguous bind for composite device %p '%s': device 1 '%s', device 2 '%s'", this,
           name_.data(), itr->bound_device()->name().data(), dev->name().data());
      return false;
    }
  }
  for (auto itr = unbound_.begin(); itr != unbound_.end(); ++itr) {
    if (itr->TryMatch(dev)) {
      VLOGF(1, "Found a match for composite device %p '%s': device '%s'", this, name_.data(),
            dev->name().data());
      *index_out = itr->index();
      return true;
    }
  }
  VLOGF(1, "No match for composite device %p '%s': device '%s'", this, name_.data(),
        dev->name().data());
  return false;
}

zx_status_t CompositeDevice::BindFragment(size_t index, const fbl::RefPtr<Device>& dev) {
  // Find the fragment we're binding
  CompositeDeviceFragment* fragment = nullptr;
  for (auto& unbound_fragment : unbound_) {
    if (unbound_fragment.index() == index) {
      fragment = &unbound_fragment;
      break;
    }
  }
  ZX_ASSERT_MSG(fragment != nullptr, "Attempted to bind fragment that wasn't unbound!\n");

  zx_status_t status = fragment->Bind(dev);
  if (status != ZX_OK) {
    return status;
  }
  bound_.push_back(unbound_.erase(*fragment));
  return ZX_OK;
}

zx_status_t CompositeDevice::TryAssemble() {
  ZX_ASSERT(device_ == nullptr);
  if (!unbound_.is_empty()) {
    return ZX_ERR_SHOULD_WAIT;
  }

  fbl::RefPtr<DriverHost> driver_host;
  for (auto& fragment : bound_) {
    // Find the driver_host to put everything in (if we don't find one, nullptr
    // means "a new driver_host").
    if (fragment.index() == coresident_device_index_) {
      driver_host = fragment.bound_device()->host();
    }
    // Make sure the fragment driver has created its device
    if (fragment.fragment_device() == nullptr) {
      return ZX_ERR_SHOULD_WAIT;
    }
  }

  Coordinator* coordinator = nullptr;
  uint64_t fragment_local_ids[fuchsia_device_manager_FRAGMENTS_MAX] = {};

  // Create all of the proxies for the fragment devices, in the same process
  for (auto& fragment : bound_) {
    const auto& fragment_dev = fragment.fragment_device();
    auto bound_dev = fragment.bound_device();
    coordinator = fragment_dev->coordinator;

    // If the device we're bound to is proxied, we care about its proxy
    // rather than it, since that's the side that we communicate with.
    if (bound_dev->proxy()) {
      bound_dev = bound_dev->proxy();
    }

    // Check if we need to use the proxy.  If not, share a reference to
    // the instance of the fragment device.
    if (bound_dev->host() == driver_host) {
      fragment_local_ids[fragment.index()] = fragment_dev->local_id();
      continue;
    }

    // We need to create it.  Double check that we haven't ended up in a state
    // where the proxies would need to be in different processes.
    if (driver_host != nullptr && fragment_dev->proxy() != nullptr &&
        fragment_dev->proxy()->host() != nullptr && fragment_dev->proxy()->host() != driver_host) {
      LOGF(ERROR, "Cannot create composite device, device proxies are in different driver_hosts");
      return ZX_ERR_BAD_STATE;
    }

    zx_status_t status = coordinator->PrepareProxy(fragment_dev, driver_host);
    if (status != ZX_OK) {
      return status;
    }
    // If we hadn't picked a driver_host, use the one that was created just now.
    if (driver_host == nullptr) {
      driver_host = fragment_dev->proxy()->host();
      ZX_ASSERT(driver_host != nullptr);
    }
    // Stash the local ID after the proxy has been created
    fragment_local_ids[fragment.index()] = fragment_dev->proxy()->local_id();
  }

  zx::channel coordinator_rpc_local, coordinator_rpc_remote;
  zx_status_t status = zx::channel::create(0, &coordinator_rpc_local, &coordinator_rpc_remote);
  if (status != ZX_OK) {
    return status;
  }

  zx::channel device_controller_rpc_local, device_controller_rpc_remote;
  status = zx::channel::create(0, &device_controller_rpc_local, &device_controller_rpc_remote);
  if (status != ZX_OK) {
    return status;
  }

  fbl::RefPtr<Device> new_device;
  status =
      Device::CreateComposite(coordinator, driver_host, *this, std::move(coordinator_rpc_local),
                              std::move(device_controller_rpc_remote), &new_device);
  if (status != ZX_OK) {
    return status;
  }
  coordinator->devices().push_back(new_device);

  // Create the composite device in the driver_host
  status = dh_send_create_composite_device(driver_host, new_device.get(), *this, fragment_local_ids,
                                           std::move(coordinator_rpc_remote),
                                           std::move(device_controller_rpc_local));
  if (status != ZX_OK) {
    LOGF(ERROR, "Failed to create composite device: %s", zx_status_get_string(status));
    return status;
  }

  device_ = std::move(new_device);
  device_->set_composite(this);

  // Add metadata
  for (size_t i = 0; i < metadata_.size(); i++) {
    // Making a copy of metadata, instead of transfering ownership, so that
    // metadata can be added again if device is recreated
    status = coordinator->AddMetadata(device_, metadata_[i]->type, metadata_[i]->Data(),
                                      metadata_[i]->length);
    if (status != ZX_OK) {
      LOGF(ERROR, "Failed to add metadata to device %p '%s': %s", device_.get(),
           device_->name().data(), zx_status_get_string(status));
      return status;
    }
  }

  status = device_->SignalReadyForBind();
  if (status != ZX_OK) {
    return status;
  }

  return ZX_OK;
}

void CompositeDevice::UnbindFragment(CompositeDeviceFragment* fragment) {
  // If the composite was fully instantiated, diassociate from it.  It will be
  // reinstantiated when this fragment is re-bound.
  if (device_ != nullptr) {
    Remove();
  }
  ZX_ASSERT(device_ == nullptr);
  ZX_ASSERT(fragment->composite() == this);
  unbound_.push_back(bound_.erase(*fragment));
}

void CompositeDevice::Remove() {
  device_->disassociate_from_composite();
  device_ = nullptr;
}

// CompositeDeviceFragment methods

CompositeDeviceFragment::CompositeDeviceFragment(CompositeDevice* composite, uint32_t index,
                                                 fbl::Array<const FragmentPartDescriptor> parts)
    : composite_(composite), index_(index), parts_(std::move(parts)) {}

CompositeDeviceFragment::~CompositeDeviceFragment() = default;

bool CompositeDeviceFragment::TryMatch(const fbl::RefPtr<Device>& dev) {
  if (parts_.size() > UINT32_MAX) {
    return false;
  }
  auto match = ::internal::MatchParts(dev, parts_.data(), static_cast<uint32_t>(parts_.size()));
  if (match != ::internal::Match::One) {
    return false;
  }
  return true;
}

zx_status_t CompositeDeviceFragment::Bind(const fbl::RefPtr<Device>& dev) {
  ZX_ASSERT(bound_device_ == nullptr);

  zx_status_t status = dev->coordinator->BindDriverToDevice(
      dev, dev->coordinator->fragment_driver(), true /* autobind */);
  if (status != ZX_OK) {
    return status;
  }

  bound_device_ = dev;
  dev->push_fragment(this);

  return ZX_OK;
}

void CompositeDeviceFragment::Unbind() {
  ZX_ASSERT(bound_device_ != nullptr);
  composite_->UnbindFragment(this);
  // Drop our reference to the device added by the fragment driver
  fragment_device_ = nullptr;
  bound_device_->disassociate_from_composite();
  bound_device_ = nullptr;
}
