/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 */

#ifndef __TOSHIBA_TC956X_DWMAC_H__
#define __TOSHIBA_TC956X_DWMAC_H__

#include <linux/compiler_types.h>
#include <linux/types.h>

#define TC956X_PCIE_DRIVER_NAME	"tc956x_pci"

#define TC956X_XGMAC_DEV_NAME	"dwmac-tc956x"

/* Starting address of the space translated by the PCIe endpoint bridge */
#define TC956X_SLV00_SRC_ADDR	0x0000001000000000ULL

/**
 * struct tc956x_dwmac_data - Structure passed to stmmac auxiliary devices.
 * @msigen:		I/O mapped address used by MSIGEN
 * @msigen_irq:		IRQ number used by MSIGEN
 *
 * This structure is passed via platform data to the stmmac auxiliary devices.
 */
struct tc956x_dwmac_data {
	void __iomem *msigen;
	unsigned int msigen_irq;
};

#endif /* __TOSHIBA_TC956X_DWMAC_H__*/
