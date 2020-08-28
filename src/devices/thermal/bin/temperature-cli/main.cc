// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/adc/llcpp/fidl.h>
#include <fuchsia/hardware/temperature/llcpp/fidl.h>
#include <lib/fdio/directory.h>
#include <stdio.h>
#include <string.h>

constexpr char kUsageMessage[] = R"""(Usage: temperature-cli <device> <command>

    resolution - Get adc resolution (for adc class device)
    read - read adc sample (for adc class device)
    readnorm - read normalized adc sample [0.0-1.0] (for adc class device)

    Example:
    temperature-cli /dev/class/temperature/000
    - or -
    temperature-cli /dev/class/adc/000 read
    temperature-cli /dev/class/adc/000 resolution
)""";

namespace FidlTemperature = llcpp::fuchsia::hardware::temperature;
namespace FidlAdc = llcpp::fuchsia::hardware::adc;

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("%s", kUsageMessage);
    return 0;
  }

  zx::channel local, remote;
  zx_status_t status = zx::channel::create(0, &local, &remote);
  if (status != ZX_OK) {
    printf("Failed to create channel: status = %d\n", status);
    return -1;
  }

  status = fdio_service_connect(argv[1], remote.release());
  if (status != ZX_OK) {
    printf("Failed to open sensor: status = %d\n", status);
    return -1;
  }

  if (argc < 3) {
    FidlTemperature::Device::SyncClient client(std::move(local));

    auto response = client.GetTemperatureCelsius();
    if (response.ok()) {
      if (!response->status) {
        printf("temperature = %f\n", response->temp);
      } else {
        printf("GetTemperatureCelsius failed: status = %d\n", response->status);
      }
    }
  } else {
    FidlAdc::Device::SyncClient client(std::move(local));
    if (strcmp(argv[2], "resolution") == 0) {
      auto response = client.GetResolution();
      if (response.ok()) {
        if (response->result.is_err()) {
          printf("GetResolution failed: status = %d\n", response->result.err());
        } else {
          printf("adc resolution  = %u\n", response->result.response().resolution);
        }
      } else {
        printf("GetResolution fidl call failed: status = %d\n", response.status());
      }
    } else if (strcmp(argv[2], "read") == 0) {
      auto response = client.GetSample();
      if (response.ok()) {
        if (response->result.is_err()) {
          printf("GetSample failed: status = %d\n", response->result.err());
        } else {
          printf("Value = %u\n", response->result.response().value);
        }
      } else {
        printf("GetSample fidl call failed: status = %d\n", response.status());
      }
    } else if (strcmp(argv[2], "readnorm") == 0) {
      auto response = client.GetNormalizedSample();
      if (response.ok()) {
        if (response->result.is_err()) {
          printf("GetSampleNormalized failed: status = %d\n", response->result.err());
        } else {
          printf("Value  = %f\n", response->result.response().value);
        }
      } else {
        printf("GetSampleNormalized fidl call failed: status = %d\n", response.status());
      }
    } else {
      printf("%s", kUsageMessage);
      return 1;
    }
  }
  return 0;
}
