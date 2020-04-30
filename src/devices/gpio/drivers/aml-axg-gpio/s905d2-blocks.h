// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_GPIO_DRIVERS_AML_AXG_GPIO_S905D2_BLOCKS_H_
#define SRC_DEVICES_GPIO_DRIVERS_AML_AXG_GPIO_S905D2_BLOCKS_H_

#include <array>

#include <soc/aml-s905d2/s905d2-gpio.h>

#include "aml-axg-gpio.h"

namespace gpio {

static AmlGpioBlock s905d2_gpio_blocks[] = {
    // GPIO Z Block
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOZ_START + 0),
        .pin_block = S905D2_GPIOZ_START,
        .pin_count = 8,
        .mux_offset = S905D2_PERIPHS_PIN_MUX_6,
        .oen_offset = S905D2_PREG_PAD_GPIO4_EN_N,
        .input_offset = S905D2_PREG_PAD_GPIO4_I,
        .output_offset = S905D2_PREG_PAD_GPIO4_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG4,
        .pull_en_offset = S905D2_PAD_PULL_UP_EN_REG4,
        .mmio_index = 0,
        .pin_start = S905D2_GPIOZ_PIN_START,
        .ds_offset = S905D2_PAD_DS_REG4A,
    },
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOZ_START + 8),
        .pin_block = S905D2_GPIOZ_START,
        .pin_count = 8,
        .mux_offset = S905D2_PERIPHS_PIN_MUX_7,
        .oen_offset = S905D2_PREG_PAD_GPIO4_EN_N,
        .input_offset = S905D2_PREG_PAD_GPIO4_I,
        .output_offset = S905D2_PREG_PAD_GPIO4_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG4,
        .pull_en_offset = S905D2_PAD_PULL_UP_EN_REG4,
        .mmio_index = 0,
        .pin_start = S905D2_GPIOZ_PIN_START,
        .ds_offset = S905D2_PAD_DS_REG4A,
    },
    // GPIO A Block
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOA_START + 0),
        .pin_block = S905D2_GPIOA_START,
        .pin_count = 8,
        .mux_offset = S905D2_PERIPHS_PIN_MUX_D,
        .oen_offset = S905D2_PREG_PAD_GPIO5_EN_N,
        .input_offset = S905D2_PREG_PAD_GPIO5_I,
        .output_offset = S905D2_PREG_PAD_GPIO5_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG5,
        .pull_en_offset = S905D2_PAD_PULL_UP_EN_REG5,
        .mmio_index = 0,
        .pin_start = S905D2_GPIOA_PIN_START,
        .ds_offset = S905D2_PAD_DS_REG5A,
    },
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOA_START + 8),
        .pin_block = S905D2_GPIOA_START,
        .pin_count = 8,
        .mux_offset = S905D2_PERIPHS_PIN_MUX_E,
        .oen_offset = S905D2_PREG_PAD_GPIO5_EN_N,
        .input_offset = S905D2_PREG_PAD_GPIO5_I,
        .output_offset = S905D2_PREG_PAD_GPIO5_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG5,
        .pull_en_offset = S905D2_PAD_PULL_UP_EN_REG5,
        .mmio_index = 0,
        .pin_start = S905D2_GPIOA_PIN_START,
        .ds_offset = S905D2_PAD_DS_REG5A,
    },
    // GPIO BOOT Block
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOBOOT_START + 0),
        .pin_block = S905D2_GPIOBOOT_START,
        .pin_count = 8,
        .mux_offset = S905D2_PERIPHS_PIN_MUX_0,
        .oen_offset = S905D2_PREG_PAD_GPIO0_EN_N,
        .input_offset = S905D2_PREG_PAD_GPIO0_I,
        .output_offset = S905D2_PREG_PAD_GPIO0_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG0,
        .pull_en_offset = S905D2_PAD_PULL_UP_EN_REG0,
        .mmio_index = 0,
        .pin_start = S905D2_GPIOBOOT_PIN_START,
        .ds_offset = S905D2_PAD_DS_REG0A,
    },
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOBOOT_START + 8),
        .pin_block = S905D2_GPIOBOOT_START,
        .pin_count = 8,
        .mux_offset = S905D2_PERIPHS_PIN_MUX_1,
        .oen_offset = S905D2_PREG_PAD_GPIO0_EN_N,
        .input_offset = S905D2_PREG_PAD_GPIO0_I,
        .output_offset = S905D2_PREG_PAD_GPIO0_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG0,
        .pull_en_offset = S905D2_PAD_PULL_UP_EN_REG0,
        .mmio_index = 0,
        .pin_start = S905D2_GPIOBOOT_PIN_START,
        .ds_offset = S905D2_PAD_DS_REG0A,
    },
    // GPIO C Block
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOC_START + 0),
        .pin_block = S905D2_GPIOC_START,
        .pin_count = 8,
        .mux_offset = S905D2_PERIPHS_PIN_MUX_9,
        .oen_offset = S905D2_PREG_PAD_GPIO1_EN_N,
        .input_offset = S905D2_PREG_PAD_GPIO1_I,
        .output_offset = S905D2_PREG_PAD_GPIO1_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG1,
        .pull_en_offset = S905D2_PAD_PULL_UP_EN_REG1,
        .mmio_index = 0,
        .pin_start = S905D2_GPIOC_PIN_START,
        .ds_offset = S905D2_PAD_DS_REG1A,
    },
    // GPIO X Block
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOX_START + 0),
        .pin_block = S905D2_GPIOX_START,
        .pin_count = 8,
        .mux_offset = S905D2_PERIPHS_PIN_MUX_3,
        .oen_offset = S905D2_PREG_PAD_GPIO2_EN_N,
        .input_offset = S905D2_PREG_PAD_GPIO2_I,
        .output_offset = S905D2_PREG_PAD_GPIO2_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG2,
        .pull_en_offset = S905D2_PAD_PULL_UP_EN_REG2,
        .mmio_index = 0,
        .pin_start = S905D2_GPIOX_PIN_START,
        .ds_offset = S905D2_PAD_DS_REG2A,
    },
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOX_START + 8),
        .pin_block = S905D2_GPIOX_START,
        .pin_count = 8,
        .mux_offset = S905D2_PERIPHS_PIN_MUX_4,
        .oen_offset = S905D2_PREG_PAD_GPIO2_EN_N,
        .input_offset = S905D2_PREG_PAD_GPIO2_I,
        .output_offset = S905D2_PREG_PAD_GPIO2_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG2,
        .pull_en_offset = S905D2_PAD_PULL_UP_EN_REG2,
        .mmio_index = 0,
        .pin_start = S905D2_GPIOX_PIN_START,
        .ds_offset = S905D2_PAD_DS_REG2A,
    },
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOX_START + 16),
        .pin_block = S905D2_GPIOX_START,
        .pin_count = 4,
        .mux_offset = S905D2_PERIPHS_PIN_MUX_5,
        .oen_offset = S905D2_PREG_PAD_GPIO2_EN_N,
        .input_offset = S905D2_PREG_PAD_GPIO2_I,
        .output_offset = S905D2_PREG_PAD_GPIO2_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG2,
        .pull_en_offset = S905D2_PAD_PULL_UP_EN_REG2,
        .mmio_index = 0,
        .pin_start = S905D2_GPIOX_PIN_START,
        .ds_offset = S905D2_PAD_DS_REG2B,
    },
    // GPIO H Block
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOH_START + 0),
        .pin_block = S905D2_GPIOH_START,
        .pin_count = 8,
        .mux_offset = S905D2_PERIPHS_PIN_MUX_B,
        .oen_offset = S905D2_PREG_PAD_GPIO3_EN_N,
        .input_offset = S905D2_PREG_PAD_GPIO3_I,
        .output_offset = S905D2_PREG_PAD_GPIO3_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG3,
        .pull_en_offset = S905D2_PAD_PULL_UP_EN_REG3,
        .mmio_index = 0,
        .pin_start = S905D2_GPIOH_PIN_START,
        .ds_offset = S905D2_PAD_DS_REG3A,
    },
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOH_START + 8),
        .pin_block = S905D2_GPIOH_START,
        .pin_count = 1,
        .mux_offset = S905D2_PERIPHS_PIN_MUX_C,
        .oen_offset = S905D2_PREG_PAD_GPIO3_EN_N,
        .input_offset = S905D2_PREG_PAD_GPIO3_I,
        .output_offset = S905D2_PREG_PAD_GPIO3_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG3,
        .pull_en_offset = S905D2_PAD_PULL_UP_EN_REG3,
        .mmio_index = 0,
        .pin_start = S905D2_GPIOH_PIN_START,
        .ds_offset = S905D2_PAD_DS_REG3A,
    },
    // GPIO AO Block
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOAO_START + 0),
        .pin_block = S905D2_GPIOAO_START,
        .pin_count = 8,
        .mux_offset = S905D2_AO_RTI_PINMUX_REG0,
        .oen_offset = S905D2_AO_GPIO_O_EN_N,
        .input_offset = S905D2_AO_GPIO_I,
        .output_offset = S905D2_AO_GPIO_O,
        .output_shift = 0,
        .pull_offset = S905D2_PULL_UP_REG3,
        .pull_en_offset = S905D2_GPIOAO_PULL_EN_REG,
        .mmio_index = 1,
        .pin_start = S905D2_GPIOA0_PIN_START,
        .ds_offset = S905D2_AO_PAD_DS_A,
    },
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOAO_START + 8),
        .pin_block = S905D2_GPIOAO_START,
        .pin_count = 4,
        .mux_offset = S905D2_AO_RTI_PINMUX_REG1,
        .oen_offset = S905D2_AO_GPIO_O_EN_N,
        .input_offset = S905D2_AO_GPIO_I,
        .output_offset = S905D2_AO_GPIO_O,
        .output_shift = 0,
        .pull_offset = S905D2_GPIOAO_PULL_UP_REG,
        .pull_en_offset = S905D2_GPIOAO_PULL_EN_REG,
        .mmio_index = 1,
        .pin_start = S905D2_GPIOA0_PIN_START,
        .ds_offset = S905D2_AO_PAD_DS_A,
    },
    // GPIO E Block
    {
        .block_lock = MTX_INIT,

        .start_pin = (S905D2_GPIOE_START + 0),
        .pin_block = S905D2_GPIOE_START,
        .pin_count = 3,
        .mux_offset = S905D2_AO_RTI_PINMUX_REG1,
        .oen_offset = S905D2_AO_GPIO_O_EN_N,
        .input_offset = S905D2_AO_GPIO_I,
        .output_offset = S905D2_AO_GPIO_O,
        .output_shift = 16,
        .pull_offset = S905D2_GPIOAO_PULL_UP_REG,
        .pull_en_offset = S905D2_GPIOAO_PULL_EN_REG,
        .mmio_index = 1,
        .pin_start = S905D2_GPIOA0_PIN_START,
        .ds_offset = S905D2_AO_PAD_DS_B,
    },
};

static AmlGpioInterrupt s905d2_interrupt_block = {
    .interrupt_lock = MTX_INIT,

    .pin_select_offset = S905D2_GPIO_0_3_PIN_SELECT,
    .edge_polarity_offset = S905D2_GPIO_INT_EDGE_POLARITY,
    .filter_select_offset = S905D2_GPIO_FILTER_SELECT,
};

}  // namespace gpio

#endif  // SRC_DEVICES_GPIO_DRIVERS_AML_AXG_GPIO_S905D2_BLOCKS_H_
