// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED. DO NOT EDIT.
// Generated from the banjo.examples.example7 banjo file

#pragma once

#include <banjo/examples/example7.h>
#include <ddk/driver.h>
#include <ddktl/device-internal.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>
#include <zircon/types.h>
#include <ddktl/protocol/composite.h>

#include "example7-internal.h"

// DDK example7-protocol support
//
// :: Proxies ::
//
// ddk::HelloProtocolClient is a simple wrapper around
// hello_protocol_t. It does not own the pointers passed to it.
//
// :: Mixins ::
//
// ddk::HelloProtocol is a mixin class that simplifies writing DDK drivers
// that implement the hello protocol. It doesn't set the base protocol.
//
// :: Examples ::
//
// // A driver that implements a ZX_PROTOCOL_HELLO device.
// class HelloDevice;
// using HelloDeviceType = ddk::Device<HelloDevice, /* ddk mixins */>;
//
// class HelloDevice : public HelloDeviceType,
//                      public ddk::HelloProtocol<HelloDevice> {
//   public:
//     HelloDevice(zx_device_t* parent)
//         : HelloDeviceType(parent) {}
//
//     void HelloSay(const char* req, char* out_response, size_t response_capacity);
//
//     ...
// };

namespace ddk {

template <typename D, typename Base = internal::base_mixin>
class HelloProtocol : public Base {
public:
    HelloProtocol() {
        internal::CheckHelloProtocolSubclass<D>();
        hello_protocol_ops_.say = HelloSay;

        if constexpr (internal::is_base_proto<Base>::value) {
            auto dev = static_cast<D*>(this);
            // Can only inherit from one base_protocol implementation.
            ZX_ASSERT(dev->ddk_proto_id_ == 0);
            dev->ddk_proto_id_ = ZX_PROTOCOL_HELLO;
            dev->ddk_proto_ops_ = &hello_protocol_ops_;
        }
    }

protected:
    hello_protocol_ops_t hello_protocol_ops_ = {};

private:
    static void HelloSay(void* ctx, const char* req, char* out_response, size_t response_capacity) {
        static_cast<D*>(ctx)->HelloSay(req, out_response, response_capacity);
    }
};

class HelloProtocolClient {
public:
    HelloProtocolClient()
        : ops_(nullptr), ctx_(nullptr) {}
    HelloProtocolClient(const hello_protocol_t* proto)
        : ops_(proto->ops), ctx_(proto->ctx) {}

    HelloProtocolClient(zx_device_t* parent) {
        hello_protocol_t proto;
        if (device_get_protocol(parent, ZX_PROTOCOL_HELLO, &proto) == ZX_OK) {
            ops_ = proto.ops;
            ctx_ = proto.ctx;
        } else {
            ops_ = nullptr;
            ctx_ = nullptr;
        }
    }

    HelloProtocolClient(CompositeProtocolClient& composite, const char* fragment_name) {
        zx_device_t* fragment;
        bool found = composite.GetFragment(fragment_name, &fragment);
        hello_protocol_t proto;
        if (found && device_get_protocol(fragment, ZX_PROTOCOL_HELLO, &proto) == ZX_OK) {
            ops_ = proto.ops;
            ctx_ = proto.ctx;
        } else {
            ops_ = nullptr;
            ctx_ = nullptr;
        }
    }

    // Create a HelloProtocolClient from the given parent device.
    //
    // If ZX_OK is returned, the created object will be initialized in |result|.
    static zx_status_t CreateFromDevice(zx_device_t* parent,
                                        HelloProtocolClient* result) {
        hello_protocol_t proto;
        zx_status_t status = device_get_protocol(
                parent, ZX_PROTOCOL_HELLO, &proto);
        if (status != ZX_OK) {
            return status;
        }
        *result = HelloProtocolClient(&proto);
        return ZX_OK;
    }

    // Create a HelloProtocolClient from the given composite protocol.
    //
    // If ZX_OK is returned, the created object will be initialized in |result|.
    static zx_status_t CreateFromComposite(CompositeProtocolClient& composite,
                                           const char* fragment_name,
                                           HelloProtocolClient* result) {
        zx_device_t* fragment;
        bool found = composite.GetFragment(fragment_name, &fragment);
        if (!found) {
          return ZX_ERR_NOT_FOUND;
        }
        return CreateFromDevice(fragment, result);
    }

    void GetProto(hello_protocol_t* proto) const {
        proto->ctx = ctx_;
        proto->ops = ops_;
    }
    bool is_valid() const {
        return ops_ != nullptr;
    }
    void clear() {
        ctx_ = nullptr;
        ops_ = nullptr;
    }

    void Say(const char* req, char* out_response, size_t response_capacity) const {
        ops_->say(ctx_, req, out_response, response_capacity);
    }

private:
    hello_protocol_ops_t* ops_;
    void* ctx_;
};

} // namespace ddk
