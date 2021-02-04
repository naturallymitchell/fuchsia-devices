// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED. DO NOT EDIT.
// Generated from the banjo.examples.order banjo file

#pragma once


#include <zircon/compiler.h>
#include <zircon/types.h>

__BEGIN_CDECLS

// Forward declarations
typedef struct zz_struct zz_struct_t;
typedef uint32_t mm_enum_t;
#define MM_ENUM_ONE UINT32_C(1)
#define MM_ENUM_TWO UINT32_C(2)
#define MM_ENUM_THREE UINT32_C(3)
typedef struct xx_struct xx_struct_t;
#define LL_CONSTANT UINT32_C(12345)

// Declarations
struct zz_struct {
    int8_t something;
};

struct xx_struct {
    zz_struct_t field;
    mm_enum_t field_again;
};


__END_CDECLS
