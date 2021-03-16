// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/i2c/c/banjo.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/platform-defs.h>
#include <lib/device-protocol/i2c.h>
#include <librtc.h>
#include <stdlib.h>
#include <zircon/assert.h>

#include "src/devices/rtc/drivers/nxp/pcf8563_rtc_bind.h"

typedef struct {
  i2c_protocol_t i2c;
} pcf8563_context;

static zx_status_t pcf8563_rtc_get(void* ctx, fuchsia_hardware_rtc_Time* rtc) {
  ZX_DEBUG_ASSERT(ctx);

  pcf8563_context* context = ctx;
  uint8_t write_buf[] = {0x02};
  uint8_t read_buf[7];
  zx_status_t err =
      i2c_write_read_sync(&context->i2c, write_buf, sizeof write_buf, read_buf, sizeof read_buf);
  if (err) {
    return err;
  }

  rtc->seconds = from_bcd(read_buf[0] & 0x7f);
  rtc->minutes = from_bcd(read_buf[1] & 0x7f);
  rtc->hours = from_bcd(read_buf[2] & 0x3f);
  rtc->day = from_bcd(read_buf[3] & 0x3f);
  rtc->month = from_bcd(read_buf[5] & 0x1f);
  rtc->year = ((read_buf[5] & 0x80) ? 2000 : 1900) + from_bcd(read_buf[6]);

  return ZX_OK;
}

static zx_status_t pcf8563_rtc_set(void* ctx, const fuchsia_hardware_rtc_Time* rtc) {
  ZX_DEBUG_ASSERT(ctx);

  // An invalid time was supplied.
  if (rtc_is_invalid(rtc)) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  int year = rtc->year;
  uint8_t century = (year < 2000) ? 0 : 0x80;
  if (century) {
    year -= 2000;
  } else {
    year -= 1900;
  }
  ZX_DEBUG_ASSERT(year < 100);

  uint8_t write_buf[] = {0x02,
                         to_bcd(rtc->seconds),
                         to_bcd(rtc->minutes),
                         to_bcd(rtc->hours),
                         to_bcd(rtc->day),
                         0,  // day of week
                         century | to_bcd(rtc->month),
                         to_bcd((uint8_t)year)};

  pcf8563_context* context = ctx;
  zx_status_t err = i2c_write_read_sync(&context->i2c, write_buf, sizeof write_buf, NULL, 0);
  if (err) {
    return err;
  }

  return ZX_OK;
}

static zx_status_t fidl_Get(void* ctx, fidl_txn_t* txn) {
  fuchsia_hardware_rtc_Time rtc;
  pcf8563_rtc_get(ctx, &rtc);
  return fuchsia_hardware_rtc_DeviceGet_reply(txn, &rtc);
}

static zx_status_t fidl_Set(void* ctx, const fuchsia_hardware_rtc_Time* rtc, fidl_txn_t* txn) {
  zx_status_t status = pcf8563_rtc_set(ctx, rtc);
  return fuchsia_hardware_rtc_DeviceSet_reply(txn, status);
}

static fuchsia_hardware_rtc_Device_ops_t fidl_ops = {
    .Get = fidl_Get,
    .Set = fidl_Set,
};

static zx_status_t pcf8563_rtc_message(void* ctx, fidl_incoming_msg_t* msg, fidl_txn_t* txn) {
  return fuchsia_hardware_rtc_Device_dispatch(ctx, txn, msg, &fidl_ops);
}

static zx_protocol_device_t pcf8563_rtc_device_proto = {.version = DEVICE_OPS_VERSION,
                                                        .message = pcf8563_rtc_message};

static zx_status_t pcf8563_bind(void* ctx, zx_device_t* parent) {
  pcf8563_context* context = calloc(1, sizeof *context);
  if (!context) {
    zxlogf(ERROR, "%s: failed to create device context", __FUNCTION__);
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t status = device_get_protocol(parent, ZX_PROTOCOL_I2C, &context->i2c);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to acquire i2c", __FUNCTION__);
    free(context);
    return status;
  }

  device_add_args_t args = {.version = DEVICE_ADD_ARGS_VERSION,
                            .name = "rtc",
                            .ops = &pcf8563_rtc_device_proto,
                            .proto_id = ZX_PROTOCOL_RTC,
                            .ctx = context};

  zx_device_t* dev;
  status = device_add(parent, &args, &dev);
  if (status != ZX_OK) {
    free(context);
    return status;
  }

  fuchsia_hardware_rtc_Time rtc;
  sanitize_rtc(context, &rtc, pcf8563_rtc_get, pcf8563_rtc_set);
  return ZX_OK;
}

static zx_driver_ops_t pcf8563_rtc_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = pcf8563_bind,
};

ZIRCON_DRIVER(pcf8563_rtc, pcf8563_rtc_ops, "pcf8563_rtc", "0.1");
