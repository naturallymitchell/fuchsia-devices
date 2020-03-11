// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BIN_DRIVER_MANAGER_COMPOSITE_DEVICE_H_
#define SRC_DEVICES_BIN_DRIVER_MANAGER_COMPOSITE_DEVICE_H_

#include <fuchsia/device/manager/c/fidl.h>
#include <fuchsia/device/manager/llcpp/fidl.h>

#include <memory>

#include <ddk/binding.h>
#include <fbl/array.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/string.h>
#include <fbl/string_piece.h>

#include "metadata.h"

// Forward declaration
class CompositeDevice;
class Coordinator;
class Device;

// Describes a device on the path to a fragment of a composite device
struct FragmentPartDescriptor {
  fbl::Array<const zx_bind_inst_t> match_program;
};

// A single device that is part of a composite device.
// TODO(teisenbe): Should this just be an inner-class of CompositeDevice?
class CompositeDeviceFragment {
 public:
  CompositeDeviceFragment(CompositeDevice* composite, uint32_t index,
                           fbl::Array<const FragmentPartDescriptor> parts);

  CompositeDeviceFragment(CompositeDeviceFragment&&) = delete;
  CompositeDeviceFragment& operator=(CompositeDeviceFragment&&) = delete;

  CompositeDeviceFragment(const CompositeDeviceFragment&) = delete;
  CompositeDeviceFragment& operator=(const CompositeDeviceFragment&) = delete;

  ~CompositeDeviceFragment();

  // Attempt to match this fragment against |dev|.  Returns true if the match
  // was successful.
  bool TryMatch(const fbl::RefPtr<Device>& dev);

  // Bind this fragment to the given device.
  zx_status_t Bind(const fbl::RefPtr<Device>& dev);

  // Unbind this fragment.
  void Unbind();

  uint32_t index() const { return index_; }
  CompositeDevice* composite() const { return composite_; }
  // If not nullptr, this fragment has been bound to this device
  const fbl::RefPtr<Device>& bound_device() const { return bound_device_; }

  const fbl::RefPtr<Device>& fragment_device() const { return fragment_device_; }
  // Registers (or unregisters) the fragment device (i.e. an instance of the
  // "fragment" driver) that bound to bound_device().
  void set_fragment_device(fbl::RefPtr<Device> device) { fragment_device_ = std::move(device); }

  // Used for embedding a fragment in the CompositeDevice's bound and unbound
  // lists.
  struct Node {
    static fbl::DoublyLinkedListNodeState<std::unique_ptr<CompositeDeviceFragment>>& node_state(
        CompositeDeviceFragment& obj) {
      return obj.node_;
    }
  };

  // Used for embedding this fragment in the bound_device's fragments' list.
  struct DeviceNode {
    static fbl::DoublyLinkedListNodeState<CompositeDeviceFragment*>& node_state(
        CompositeDeviceFragment& obj) {
      return obj.device_node_;
    }
  };

 private:
  // The CompositeDevice that this is a part of
  CompositeDevice* const composite_;

  // The index of this fragment within its CompositeDevice
  const uint32_t index_;

  // A description of the devices from the root of the tree to the fragment
  // itself.
  const fbl::Array<const FragmentPartDescriptor> parts_;

  // If this fragment has been bound to a device, this points to that device.
  fbl::RefPtr<Device> bound_device_ = nullptr;
  // Once the bound device has the fragment driver attach to it, this points
  // to the device managed by the fragment driver.
  fbl::RefPtr<Device> fragment_device_ = nullptr;

  fbl::DoublyLinkedListNodeState<std::unique_ptr<CompositeDeviceFragment>> node_;
  fbl::DoublyLinkedListNodeState<CompositeDeviceFragment*> device_node_;
};

// A device composed of other devices.
class CompositeDevice {
 public:
  // Only public because of make_unique.  You probably want Create().
  CompositeDevice(fbl::String name, fbl::Array<const zx_device_prop_t> properties,
                  uint32_t fragments_count, uint32_t coresident_device_index,
                  fbl::Array<std::unique_ptr<Metadata>> metadata);

  CompositeDevice(CompositeDevice&&) = delete;
  CompositeDevice& operator=(CompositeDevice&&) = delete;

  CompositeDevice(const CompositeDevice&) = delete;
  CompositeDevice& operator=(const CompositeDevice&) = delete;

  ~CompositeDevice();

  static zx_status_t Create(const fbl::StringPiece& name,
                            llcpp::fuchsia::device::manager::CompositeDeviceDescriptor comp_desc,
                            std::unique_ptr<CompositeDevice>* out);

  const fbl::String& name() const { return name_; }
  const fbl::Array<const zx_device_prop_t>& properties() const { return properties_; }
  uint32_t fragments_count() const { return fragments_count_; }

  // Returns a reference to the constructed composite device, if it exists.
  fbl::RefPtr<Device> device() const { return device_; }

  // Attempt to match any of the unbound fragments against |dev|.  Returns true
  // if a fragment was match.  |*fragment_out| will be set to the index of
  // the matching fragment.
  bool TryMatchFragments(const fbl::RefPtr<Device>& dev, size_t* index_out);

  // Bind the fragment with the given index to the specified device
  zx_status_t BindFragment(size_t index, const fbl::RefPtr<Device>& dev);

  // Mark the given fragment as unbound.  Note that since we don't expose
  // this device's fragments in the API, this method can only be invoked by
  // CompositeDeviceFragment
  void UnbindFragment(CompositeDeviceFragment* fragment);

  // Creates the actual device and orchestrates the creation of the composite
  // device in a devhost.
  // Returns ZX_ERR_SHOULD_WAIT if some fragment is not fully ready (i.e. has
  // either not been matched or the fragment driver that bound to it has not
  // yet published its device).
  zx_status_t TryAssemble();

  // Forget about the composite device that was constructed.  If TryAssemble()
  // is invoked after this, it will reassemble the device.
  void Remove();

  // Node for list of composite devices the coordinator knows about
  struct Node {
    static fbl::DoublyLinkedListNodeState<std::unique_ptr<CompositeDevice>>& node_state(
        CompositeDevice& obj) {
      return obj.node_;
    }
  };

  using FragmentList = fbl::DoublyLinkedList<std::unique_ptr<CompositeDeviceFragment>,
                                              CompositeDeviceFragment::Node>;
  FragmentList& bound_fragments() { return bound_; }

 private:
  const fbl::String name_;
  const fbl::Array<const zx_device_prop_t> properties_;
  const uint32_t fragments_count_;
  const uint32_t coresident_device_index_;
  const fbl::Array<std::unique_ptr<Metadata>> metadata_;

  FragmentList unbound_;
  FragmentList bound_;
  fbl::DoublyLinkedListNodeState<std::unique_ptr<CompositeDevice>> node_;

  // Once the composite has been assembled, this refers to the constructed
  // device.
  fbl::RefPtr<Device> device_;
};

#endif  // SRC_DEVICES_BIN_DRIVER_MANAGER_COMPOSITE_DEVICE_H_
