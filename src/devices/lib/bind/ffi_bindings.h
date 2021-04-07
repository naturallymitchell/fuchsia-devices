// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_LIB_BIND_FFI_BINDINGS_H_
#define SRC_DEVICES_LIB_BIND_FFI_BINDINGS_H_

struct device_property_t {
  uint32_t key;
  uint32_t value;
};

extern "C" {
bool match_bind_rules(size_t bytecode_sz, const uint8_t *bytecode, size_t properties_sz,
                      const device_property_t *properties);
}

#endif  // SRC_DEVICES_LIB_BIND_FFI_BINDINGS_H_
