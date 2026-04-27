/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 */

#ifndef __TOSHIBA_TC9564_DWMAC_H__
#define __TOSHIBA_TC9564_DWMAC_H__

#include <linux/compiler_types.h>
#include <linux/types.h>

#define TC9564_PCIE_DRIVER_NAME	"tc9564_pci"

#define TC9564_XGMAC_DEV_NAME	"dwmac-tc9564"

/* Starting address of the space translated by the PCIe endpoint bridge */
#define TC9564_SLV00_SRC_ADDR	0x0000001000000000ULL

/**
 * struct tc9564_dwmac_data - Structure passed to stmmac auxiliary devices.
 * @msigen:		I/O mapped address used by MSIGEN
 * @msigen_irq:		IRQ number used by MSIGEN
 *
 * This structure is passed via platform data to the stmmac auxiliary devices.
 */
struct tc9564_dwmac_data {
	void __iomem *msigen;
	unsigned int msigen_irq;
};

#endif /* __TOSHIBA_TC9564_DWMAC_H__*/
