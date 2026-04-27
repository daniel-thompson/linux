/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 */

#ifndef __TOSHIBA_TC9564_CLOCKS_H__
#define __TOSHIBA_TC9564_CLOCKS_H__

#define CLOCK_MCU		0
#define CLOCK_INTC		1
/* #define CLOCK_PCIE		2 */
/* #define CLOCK_I2C		3 */
#define CLOCK_SRAM		4
#define CLOCK_UART		5
#define CLOCK_MSIGEN		6
#define CLOCK_PLL		7
#define CLOCK_SGMII		8
#define CLOCK_REFCLKO		9

#define CLOCK_MAC0_TX		10
#define CLOCK_MAC0_RX		11
#define CLOCK_MAC0_125M		12
#define CLOCK_MAC0_312_5M	13
#define CLOCK_MAC0_ALL		14

#define CLOCK_MAC1_TX		15
#define CLOCK_MAC1_RX		16
#define CLOCK_MAC1_RMII		17
#define CLOCK_MAC1_125M		18
#define CLOCK_MAC1_312_5M	19
#define CLOCK_MAC1_ALL		20

#endif /* __TOSHIBA_TC9564_CLOCKS_H__*/
