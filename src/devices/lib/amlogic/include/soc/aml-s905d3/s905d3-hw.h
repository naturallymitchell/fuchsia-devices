// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_LIB_AMLOGIC_INCLUDE_SOC_AML_S905D3_S905D3_HW_H_
#define SRC_DEVICES_LIB_AMLOGIC_INCLUDE_SOC_AML_S905D3_S905D3_HW_H_

#define S905D3_GPIO_BASE 0xff634400
#define S905D3_GPIO_LENGTH 0x400
#define S905D3_GPIO_A0_BASE 0xff800000
#define S905D3_GPIO_AO_LENGTH 0x1000
#define S905D3_GPIO_INTERRUPT_BASE 0xffd00000
#define S905D3_GPIO_INTERRUPT_LENGTH 0x10000

#define S905D3_MSR_CLK_BASE 0xffd18000
#define S905D3_MSR_CLK_LENGTH 0x1000

#define S905D3_TEMP_SENSOR_BASE 0xff634800
#define S905D3_TEMP_SENSOR_LENGTH 0x80

// These registers are used to derive calibration data for the temperature sensors. The registers
// are not documented in the datasheet - they were copied over from u-boot/Cast code.
#define S905D3_TEMP_SENSOR_TRIM 0xff800268
#define S905D3_TEMP_SENSOR_TRIM_LENGTH 0x4

#define S905D3_USB0_BASE 0xff500000
#define S905D3_USB0_LENGTH 0x100000

#define S905D3_USB1_BASE 0xff400000
#define S905D3_USB1_LENGTH 0x40000

#define S905D3_DOS_BASE 0xff620000
#define S905D3_DOS_LENGTH 0x10000

#define S905D3_DMC_BASE 0xff638000
#define S905D3_DMC_LENGTH 0x1000

#define S905D3_USBCTRL_BASE 0xffe09000
#define S905D3_USBCTRL_LENGTH 0x1000

#define S905D3_RESET_BASE 0xffd01000
#define S905D3_RESET_LENGTH 0x1000

#define S905D3_RESET1_BASE 0xffd01008
#define S905D3_RESET1_LENGTH 0x1000

#define S905D3_MEMORY_PD_BASE 0xff63c000
#define S905D3_MEMORY_PD_LENGTH 0x1000

#define S905D3_POWER_BASE 0xff63c100
#define S905D3_POWER_LENGTH 0x1000

#define S905D3_POWER_DOMAIN_BASE 0xff800000
#define S905D3_POWER_DOMAIN_LENGTH 0x1000

#define S905D3_SLEEP_BASE 0xff8000e8
#define S905D3_SLEEP_LENGTH 0x1000

#define S905D3_MIPI_DSI_BASE 0xffd07000
#define S905D3_MIPI_DSI_LENGTH 0x1000

#define S905D3_MIPI_TOP_DSI_BASE 0xffd073C0
#define S905D3_MIPI_TOP_DSI_LENGTH 0x40

#define S905D3_DSI_PHY_BASE 0xff644000
#define S905D3_DSI_PHY_LENGTH 0x1000

#define S905D3_USBPHY20_BASE 0xff636000
#define S905D3_USBPHY20_LENGTH 0x2000

#define S905D3_USBPHY21_BASE 0xff63a000
#define S905D3_USBPHY21_LENGTH 0x2000

#define S905D3_HIU_BASE 0xff63c000
#define S905D3_HIU_LENGTH 0x2000

#define S905D3_VPU_BASE 0xff900000
#define S905D3_VPU_LENGTH 0x40000

#define S905D3_MALI_BASE 0xffe40000
#define S905D3_MALI_LENGTH 0x40000

#define S905D3_NNA_BASE 0xff100000
#define S905D3_NNA_LENGTH 0x40000

#define S905D3_CBUS_BASE 0xffd00000
#define S905D3_CBUS_LENGTH 0x100000
#define S905D3_I2C0_BASE (S905D3_CBUS_BASE + 0x1f000)
#define S905D3_I2C1_BASE (S905D3_CBUS_BASE + 0x1e000)
#define S905D3_I2C2_BASE (S905D3_CBUS_BASE + 0x1d000)
#define S905D3_I2C3_BASE (S905D3_CBUS_BASE + 0x1c000)

#define S905D3_SPICC0_BASE 0xffd13000
#define S905D3_SPICC0_LENGTH 0x1000
#define S905D3_SPICC1_BASE 0xffd15000
#define S905D3_SPICC1_LENGTH 0x1000

#define S905D3_AOBUS_BASE 0xff800000
#define S905D3_AOBUS_LENGTH 0x100000

#define S905D3_I2C_AO_0_BASE (S905D3_AOBUS_BASE + 0x5000)
// SDIO
#define S905D3_EMMC_A_SDIO_BASE 0xffe03000
#define S905D3_EMMC_A_SDIO_LENGTH 0x2000

// PORT B
#define S905D3_EMMC_B_SDIO_BASE 0xffe05000
#define S905D3_EMMC_B_SDIO_LENGTH 0x2000

// PORT C
#define S905D3_EMMC_C_SDIO_BASE 0xffe07000
#define S905D3_EMMC_C_SDIO_LENGTH 0x2000

#define S905D3_RAW_NAND_REG_BASE (S905D3_AOBUS_BASE + 0x607800)
#define S905D3_RAW_NAND_CLOCK_BASE (S905D3_AOBUS_BASE + 0x607000)

#define S905D3_UART_A_BASE 0xffd24000
#define S905D3_UART_A_LENGTH 0x18

// Reset register offsets
#define S905D3_RESET0_REGISTER 0x04
#define S905D3_RESET1_REGISTER 0x08
#define S905D3_RESET1_USB (1 << 2)  // bit to reset USB
#define S905D3_RESET2_REGISTER 0x0c
#define S905D3_RESET3_REGISTER 0x10
#define S905D3_RESET4_REGISTER 0x14
#define S905D3_RESET6_REGISTER 0x1c
#define S905D3_RESET7_REGISTER 0x20
#define S905D3_RESET0_MASK 0x40
#define S905D3_RESET1_MASK 0x44
#define S905D3_RESET2_MASK 0x48
#define S905D3_RESET3_MASK 0x4c
#define S905D3_RESET4_MASK 0x50
#define S905D3_RESET6_MASK 0x58
#define S905D3_RESET7_MASK 0x5c
#define S905D3_RESET0_LEVEL 0x80
#define S905D3_RESET1_LEVEL 0x84
#define S905D3_RESET2_LEVEL 0x88
#define S905D3_RESET3_LEVEL 0x8c
#define S905D3_RESET4_LEVEL 0x90
#define S905D3_RESET6_LEVEL 0x98
#define S905D3_RESET7_LEVEL 0x9c

#define S905D3_I2C0_IRQ 53
#define S905D3_I2C1_IRQ 246
#define S905D3_I2C2_IRQ 247
#define S905D3_I2C3_IRQ 71
#define S905D3_I2C_AO_0_IRQ 227

#define S905D3_PWM_BASE 0xffd00000

// PWM register offsets
// These are relative to base address S905D3_PWM_BASE and in sizeof(uint32_t)
#define S905D3_PWM_AB_BASE 0xffd1b000
#define S905D3_PWM_AB_LENGTH 0x1000
#define S905D3_PWM_PWM_A 0x6c00
#define S905D3_PWM_PWM_B 0x6c01
#define S905D3_PWM_MISC_REG_AB 0x6c02
#define S905D3_DS_A_B 0x6c03
#define S905D3_PWM_TIME_AB 0x6c04
#define S905D3_PWM_A2 0x6c05
#define S905D3_PWM_B2 0x6c06
#define S905D3_PWM_BLINK_AB 0x6c07

#define S905D3_PWM_CD_BASE 0xffd1a000
#define S905D3_PWM_PWM_C 0x6800
#define S905D3_PWM_PWM_D 0x6801
#define S905D3_PWM_MISC_REG_CD 0x6802
#define S905D3_DS_C_D 0x6803
#define S905D3_PWM_TIME_CD 0x6804
#define S905D3_PWM_C2 0x6805
#define S905D3_PWM_D2 0x6806
#define S905D3_PWM_BLINK_CD 0x6807

#define S905D3_PWM_EF_BASE 0xffd19000
#define S905D3_PWM_PWM_E 0x6400
#define S905D3_PWM_PWM_F 0x6401
#define S905D3_PWM_MISC_REG_EF 0x6402
#define S905D3_DS_E_F 0x6403
#define S905D3_PWM_TIME_EF 0x6404
#define S905D3_PWM_E2 0x6405
#define S905D3_PWM_F2 0x6406
#define S905D3_PWM_BLINK_EF 0x6407

#define S905D3_AO_PWM_AB_BASE 0xFF807000
#define S905D3_AO_PWM_PWM_A 0x0
#define S905D3_AO_PWM_PWM_B 0x4
#define S905D3_AO_PWM_MISC_REG_AB 0x8
#define S905D3_AO_DS_A_B 0xc
#define S905D3_AO_PWM_TIME_AB 0x10
#define S905D3_AO_PWM_A2 0x14
#define S905D3_AO_PWM_B2 0x18
#define S905D3_AO_PWM_BLINK_AB 0x1c

#define S905D3_AO_PWM_CD_BASE 0xFF802000
#define S905D3_AO_PWM_PWM_C 0x0
#define S905D3_AO_PWM_PWM_D 0x4
#define S905D3_AO_PWM_MISC_REG_CD 0x8
#define S905D3_AO_DS_C_D 0xc
#define S905D3_AO_PWM_TIME_CD 0x10
#define S905D3_AO_PWM_C2 0x14
#define S905D3_AO_PWM_D2 0x18
#define S905D3_AO_PWM_BLINK_CD 0x1c
#define S905D3_AO_PWM_LENGTH 0x1000

#define S905D3_VIU1_VSYNC_IRQ 35
#define S905D3_USB_IDDIG_IRQ 48
#define S905D3_DEMUX_IRQ 55
#define S905D3_UART_A_IRQ 58
#define S905D3_USB0_IRQ 62
#define S905D3_USB1_IRQ 63
#define S905D3_PARSER_IRQ 64
#define S905D3_RAW_NAND_IRQ 66
#define S905D3_TS_PLL_IRQ 67
#define S905D3_DOS_MBOX_0_IRQ 75
#define S905D3_DOS_MBOX_1_IRQ 76
#define S905D3_DOS_MBOX_2_IRQ 77
#define S905D3_DMC_IRQ 84
#define S905D3_GPIO_IRQ_0 96
#define S905D3_GPIO_IRQ_1 97
#define S905D3_GPIO_IRQ_2 98
#define S905D3_GPIO_IRQ_3 99
#define S905D3_GPIO_IRQ_4 100
#define S905D3_GPIO_IRQ_5 101
#define S905D3_GPIO_IRQ_6 102
#define S905D3_GPIO_IRQ_7 103
#define S905D3_SPICC0_IRQ 113
#define S905D3_VID1_WR 118
#define S905D3_RDMA_DONE 121
#define S905D3_SPICC1_IRQ 122
#define S905D3_MALI_IRQ_GP 192
#define S905D3_MALI_IRQ_GPMMU 193
#define S905D3_MALI_IRQ_PP 194
#define S905D3_NNA_IRQ 218
#define S905D3_A0_GPIO_IRQ_0 238
#define S905D3_A0_GPIO_IRQ_1 239

#define S905D3_EMMC_A_SDIO_IRQ 221
#define S905D3_EMMC_B_SDIO_IRQ 222
#define S905D3_EMMC_C_SDIO_IRQ 223

// Alternate Functions for SDIO
#define S905D3_WIFI_SDIO_D0 S905D3_GPIOX(0)
#define S905D3_WIFI_SDIO_D0_FN 1
#define S905D3_WIFI_SDIO_D1 S905D3_GPIOX(1)
#define S905D3_WIFI_SDIO_D1_FN 1
#define S905D3_WIFI_SDIO_D2 S905D3_GPIOX(2)
#define S905D3_WIFI_SDIO_D2_FN 1
#define S905D3_WIFI_SDIO_D3 S905D3_GPIOX(3)
#define S905D3_WIFI_SDIO_D3_FN 1
#define S905D3_WIFI_SDIO_CLK S905D3_GPIOX(4)
#define S905D3_WIFI_SDIO_CLK_FN 1
#define S905D3_WIFI_SDIO_CMD S905D3_GPIOX(5)
#define S905D3_WIFI_SDIO_CMD_FN 1
#define S905D3_WIFI_SDIO_WAKE_HOST S905D3_GPIOX(7)
#define S905D3_WIFI_SDIO_WAKE_HOST_FN 1

// Alternate functions for UARTs
#define S905D3_UART_TX_A S905D3_GPIOX(12)
#define S905D3_UART_TX_A_FN 1
#define S905D3_UART_RX_A S905D3_GPIOX(13)
#define S905D3_UART_RX_A_FN 1
#define S905D3_UART_CTS_A S905D3_GPIOX(14)
#define S905D3_UART_CTS_A_FN 1
#define S905D3_UART_RTS_A S905D3_GPIOX(15)
#define S905D3_UART_RTS_A_FN 1

// Alternate function for PWM
#define S905D3_PWM_D_PIN S905D3_GPIOE(1)
#define S905D3_PWM_D_FN 3

#define S905D3_EE_PDM_BASE (0xff661000)
#define S905D3_EE_PDM_LENGTH (0x2000)

#define S905D3_EE_AUDIO_BASE (0xff660000)
#define S905D3_EE_AUDIO_LENGTH (0x1000)

#define HHI_GCLK_MPEG0_OFFSET 0x50
#define HHI_SD_EMMC_CLK_CNTL_OFFSET 0x99

// Alternate Functions for EMMC
#define S905D3_EMMC_D0 S905D3_GPIOBOOT(0)
#define S905D3_EMMC_D0_FN 1
#define S905D3_EMMC_D1 S905D3_GPIOBOOT(1)
#define S905D3_EMMC_D1_FN 1
#define S905D3_EMMC_D2 S905D3_GPIOBOOT(2)
#define S905D3_EMMC_D2_FN 1
#define S905D3_EMMC_D3 S905D3_GPIOBOOT(3)
#define S905D3_EMMC_D3_FN 1
#define S905D3_EMMC_D4 S905D3_GPIOBOOT(4)
#define S905D3_EMMC_D4_FN 1
#define S905D3_EMMC_D5 S905D3_GPIOBOOT(5)
#define S905D3_EMMC_D5_FN 1
#define S905D3_EMMC_D6 S905D3_GPIOBOOT(6)
#define S905D3_EMMC_D6_FN 1
#define S905D3_EMMC_D7 S905D3_GPIOBOOT(7)
#define S905D3_EMMC_D7_FN 1
#define S905D3_EMMC_CLK S905D3_GPIOBOOT(8)
#define S905D3_EMMC_CLK_FN 1
#define S905D3_EMMC_RST S905D3_GPIOBOOT(9)
#define S905D3_EMMC_RST_FN 1
#define S905D3_EMMC_CMD S905D3_GPIOBOOT(10)
#define S905D3_EMMC_CMD_FN 1
#define S905D3_EMMC_DS S905D3_GPIOBOOT(15)
#define S905D3_EMMC_DS_FN 1

#endif  // SRC_DEVICES_LIB_AMLOGIC_INCLUDE_SOC_AML_S905D3_S905D3_HW_H_
