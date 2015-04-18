/*
 * Copyright (C) 2015
 * Kamil Lulko <rev13@wp.pl>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

		.macro	addruart, rp, rv, tmp
#ifdef CONFIG_DEBUG_UART_PHYS
		ldr	\rp, =CONFIG_DEBUG_UART_PHYS
		ldr	\rv, =CONFIG_DEBUG_UART_VIRT
#endif
		.endm

		.macro	senduart,rd,rx
		str	\rd, [\rx, #0x4]
		.endm

		.macro	waituart,rd,rx
		.endm

		.macro	busyuart,rd,rx
1002:		ldr	\rd, [\rx, #0x0]
		tst	\rd, #1 << 7
		beq	1002b
		.endm
