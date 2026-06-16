// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 *
 * Derived from code having the following copyrights:
 * Copyright (C) 2011-2012  Vayavya Labs Pvt Ltd
 * Copyright (C) 2025 Toshiba Electronic Devices & Storage Corporation
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/mfd/syscon.h>
#include <linux/pcs/pcs-xpcs-regmap.h>
#include <linux/pcs/pcs-xpcs.h>
#include <linux/phy.h>
#include <linux/regmap.h>
#include <linux/stmmac.h>
#include <linux/types.h>
#include <linux/units.h>

#include <soc/toshiba/tc956x-dwmac.h>

#include "common.h"
#include "dwxgmac2.h"
#include "stmmac.h"

#define DRIVER_NAME			"dwmac-tc956x"

#define TC956X_PTP_CLOCK_RATE		(250 * HZ_PER_MHZ)

#define TC956X_RX_FIFO_KB		46	/* Shared by all RX queues */
#define TC956X_TX_FIFO_KB		46	/* Shared by all TX queues */

/* Fields and values for the EMACTL registers */
#define EMAC_CTL_OFFSET(_mac_id)	((_mac_id) ? 0x1074 : 0x1070)
#define EMAC_SP_SEL_MASK		GENMASK(3, 0)
#define SP_SEL_2500BASEX		4
#define SP_SEL_SGMII_1000M		5
#define SP_SEL_SGMII_100M		6
#define SP_SEL_SGMII_10M		7
#define EMAC_PHY_INF_SEL_MASK		GENMASK(5, 4)
#define PCS_CLK_PHY			1	/* Clock from PHY */
#define EMAC_INV_SGM_SIG_DET		BIT(6)	/* 1 = polarity inverted */
#define EMAC_LPIHWCLKEN			BIT(8)	/* 1 = low power mode */
#define EMAC_INIT_DONE			BIT(21)

/* MSIGEN Registers */
#define MSI_OUT_EN_OFFSET		0x0000
#define MSI_MASK_CLR_OFFSET		0x000c
#define MSI_MASK_VALUE			BIT(0)
#define MSI_INT_STS_OFFSET		0x0010

enum msigen_hwirq {
	HWIRQ_LPI		= 0,
	HWIRQ_PMT		= 1,
	HWIRQ_EVENT		= 2,
	HWIRQ_TX0		= 3,
	HWIRQ_RX0		= 11,
	HWIRQ_XPCS		= 19,
	HWIRQ_PHY		= 20,
	HWIRQ_PFMAILBOX		= 21,
	HWIRQ_MSIREQ_PLS	= 24
};

#define HWIRQ_COUNT			25

/* Offset to the XPCS memory block, relative to the EMAC address range */
#define DWMAC_XPCS_OFFSET		0x3a00

/* Offset to the PMATOP memory block, relative to the EMAC address range */
#define DWMAC_PMATOP_OFFSET			0x4000

#define PMA_CML_GL_PM_CFG0			0x01b8

/*
 * Five sets three registers must be configured for PMA.  The HWT_REFCLK
 * registers are each separated by 0x14 bytes.  The Common0 configuration
 * registers are separated by 0x8 bytes.
 */
#define PMA_REG_COUNT				5

#define PMA_HWT_REFCK_R_EN			0x1080
#define PMA_HWT_REFCK_TERM_EN			0x1090
#define PMA_HWT_REFCK_STRIDE			0x0014

#define PMA_COMM_CFG_0_1			0x1888
#define PMA_COMM_CFG_0_1_STRIDE			0x0008

/* PMA_COMM_CFG_0_1 fields (WRITE_MASK is a field name) */
#define COMM_CFG_WRITE_MASK_MASK		GENMASK(16, 9)
#define WRITE_MASK_VALUE			0xf7	/* Power-on value */
#define COMM_CFG_ENABLE				BIT(8)
#define COMM_CFG_WRITE_DATA_MASK		GENMASK(7, 0)
#define WRITE_DATA_VALUE			0x04	/* Power-on value */

enum {
	RESET_ID_MAC,
	RESET_ID_XPCS,
	RESET_ID_PMA,
};

static const char *tc956x_reset_names[] = {
	[RESET_ID_MAC]	= "toshiba,tc956x-mac-reset",
	[RESET_ID_XPCS]	= "toshiba,tc956x-xpcs-reset",
	[RESET_ID_PMA]	= "toshiba,tc956x-pma-reset",
};

static const char *tc956x_clock_names[] = {
	"toshiba,tc956x-mac-tx-clock",
	"toshiba,tc956x-mac-rx-clock",
	"toshiba,tc956x-mac-all-clock",
	"toshiba,tc956x-mac-rmii-clock",	/* eMAC 1 only; must be last */
};

/**
 * struct tc956x_data - Toshiba-specific platform data
 * @dev:		Device pointer
 * @irq_domain:		MSIGEN IRQ domain
 * @auxbus_data:	Pointer to data passed from the parent device
 * @ioaddr:		Pointer to mapped eMAC memory
 * @mac_id:		MAC number (0 or 1)
 * @plat:		Pointer to our stmmac platform data
 * @resets:		Reset controller bulk array
 * @clocks:		Clock controller bulk array
 * @clock_count:	Number of valid elements in the clock array
 * @config_regmap:	Regmap used to access eMAC configuration registers
 * @msigen_regmap:	Regmap used to access MSIGEN registers
 * @msigen_irq:		IRQ number for MSIGEN
 * @dma_cfg:		DMA config buffer used by plat_stmmacenet_data
 * @mdio_bus_data:	MDIO bus data used by plat_stmmacenet_data
 * @axi:		AXI data used by plat_stmmacenet_data
 * @res:		Structure passed to stmmac_dvr_probe()
 * @desc:		DMA descriptor data used by mac_device_info
 * @dma:		DMA operations data used by mac_device_info
 */
struct tc956x_data {
	struct device *dev;
	struct irq_domain *irq_domain;
	// struct tc956x_dwmac_data *auxbus_data;
	void __iomem *ioaddr;
	u8 mac_id;
	struct plat_stmmacenet_data *plat;
	struct reset_control_bulk_data resets[ARRAY_SIZE(tc956x_reset_names)];
	struct clk_bulk_data clocks[ARRAY_SIZE(tc956x_clock_names)];
	u32 clock_count;

	struct regmap *config_regmap;
	struct regmap *msigen_regmap;
	unsigned int msigen_irq;

	/* These three fields are used by the plat_stmmacenet_data structure */
	struct stmmac_dma_cfg dma_cfg;
	struct stmmac_mdio_bus_data mdio_bus_data;
	struct stmmac_axi axi;

	/* This field is data passed to stmmac_dvr_probe() */
	struct stmmac_resources res;

	/* These two fields are used by the mac_device_info structure */
	struct stmmac_desc_ops desc;
	struct stmmac_dma_ops dma;
};

struct tc956x_mac_speed {
	phy_interface_t phy_interface;
	int speed;
	u32 sp_sel;
};

static struct tc956x_mac_speed mac_speed[] = {
	{ PHY_INTERFACE_MODE_2500BASEX,	SPEED_2500,  SP_SEL_2500BASEX },
	{ PHY_INTERFACE_MODE_SGMII,	SPEED_1000,  SP_SEL_SGMII_1000M },
	{ PHY_INTERFACE_MODE_SGMII,	SPEED_100,   SP_SEL_SGMII_100M },
	{ PHY_INTERFACE_MODE_SGMII,	SPEED_10,    SP_SEL_SGMII_10M },
};

/* TC956x uses indirect addressing so this need only describe a 1KiB range */
static const struct regmap_config xpcs_regmap_config = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_base	= 0x00,		/* Minimum XPCS reg offset */
	.max_register	= 0xff,		/* Register DW_VR_CSR_VIEWPORT */
	.reg_shift	= REGMAP_UPSHIFT(2),
};

/* TODO */
#if 0
static void tc956x_msigen_irq_handler(struct irq_desc *desc)
{
	struct irq_domain *irq_domain = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct irq_chip_generic *gc;
	unsigned long status;
	unsigned int hwirq;

	gc = irq_get_domain_generic_chip(irq_domain, 0);

	chained_irq_enter(chip, desc);

	status = irq_reg_readl(gc, MSI_INT_STS_OFFSET);
	for_each_set_bit(hwirq, &status, HWIRQ_COUNT)
		generic_handle_domain_irq(irq_domain, hwirq);

	/*
	 * Clear the MSI flag. Most interrupts within TC956X are level-high
	 * type. If any interrupts are still asserted then clearing this flag
	 * will cause the (edge-triggered) MSI to be regenerated.
	 */
	irq_reg_writel(gc, MSI_MASK_VALUE, MSI_MASK_CLR_OFFSET);

	chained_irq_exit(chip, desc);
}
#endif

/* TODO */
// Daniel:  This should use a regmap for the msigen pointer
static int tc956x_msigen_irq_chip_init(struct irq_chip_generic *gc)
{
#if 0
	struct tc956x_data *td = gc->domain->host_data;

	gc->reg_base = td->auxbus_data->msigen;
	gc->chip_types[0].regs.mask = MSI_OUT_EN_OFFSET;
	gc->chip_types[0].chip.irq_mask = irq_gc_mask_clr_bit;
	gc->chip_types[0].chip.irq_unmask = irq_gc_mask_set_bit;

	/* Disable all interrupts */
	irq_reg_writel(gc, 0, MSI_OUT_EN_OFFSET);
#endif
	return 0;
}

/* TODO */
// Daniel:  Implement gc->reg_writel() callback?
static void tc956x_msigen_irq_chip_exit(struct irq_chip_generic *gc)
{
	irq_reg_writel(gc, 0, MSI_OUT_EN_OFFSET);
}

/* TODO */
// Daniel:  Needs to get the msigen_irq some other way
static int tc956x_msigen_irq_domain_init(struct irq_domain *irq_domain)
{
#if 0
	struct tc956x_data *td = irq_domain->host_data;

	irq_set_chained_handler_and_data(td->auxbus_data->msigen_irq,
					 tc956x_msigen_irq_handler,
					 irq_domain);
#endif
	return 0;
}

/* TODO */
static void tc956x_msigen_irq_domain_exit(struct irq_domain *irq_domain)
{
#if 0
	struct tc956x_data *td = irq_domain->host_data;

	irq_set_chained_handler_and_data(td->auxbus_data->msigen_irq,
					 NULL, NULL);
#endif
}

/* TODO */
/* We have one IRQ chip instance with 25 IRQs in its domain */
static struct irq_domain *
tc956x_msigen_irq_domain_instantiate(struct tc956x_data *td)
{
	struct irq_domain_chip_generic_info dgc_info;
	struct irq_domain_info info;

	dgc_info.name = devm_kasprintf(td->dev, GFP_KERNEL, "tc956x-msigen-%d",
				       td->mac_id);
	if (!dgc_info.name)
		return ERR_PTR(-ENOMEM);

	dgc_info.handler = handle_level_irq;
	dgc_info.irqs_per_chip = HWIRQ_COUNT;
	dgc_info.num_ct = 1;
	dgc_info.init = tc956x_msigen_irq_chip_init;
	dgc_info.exit = tc956x_msigen_irq_chip_exit;

	info.domain_flags = IRQ_DOMAIN_FLAG_DESTROY_GC;
	info.size = HWIRQ_COUNT;
	info.hwirq_max = HWIRQ_COUNT;
	info.ops = &irq_generic_chip_ops;
	info.host_data = td;
	info.dgc_info = &dgc_info;
	info.init = tc956x_msigen_irq_domain_init;
	info.exit = tc956x_msigen_irq_domain_exit;

	return devm_irq_domain_instantiate(td->dev, &info);
}

/**
 * tc956x_pma_init() - Initialize PMA
 * @td:	bsp_priv pointer
 *
 * Initialize (or re-initialize) the PMA, configure the clocks and wait for the
 * eMAC to be ready.
 */
static void tc956x_pma_init(struct tc956x_data *td)
{
	struct reset_control *rstc = td->resets[RESET_ID_PMA].rstc;
	struct regmap *regmap = td->config_regmap;
	void __iomem *pmatop;
	u32 val;
	int ret;
	u32 i;

	/*
	 * When we re-initialize the PMA then the reset will already have
	 * been deasserted. We must make sure the PMA reset is asserted
	 * before we change the clock settings.
	 */
	WARN_ON(reset_control_assert(rstc));

	pmatop = td->ioaddr + DWMAC_PMATOP_OFFSET;

	/* Power on CML buffer (0 = normal mode, 1 = power down) */
	writel(0, pmatop + PMA_CML_GL_PM_CFG0);

	/* This value switches clock from C0_REFCK to CLK_REF_I */
	val = u32_encode_bits(WRITE_MASK_VALUE, COMM_CFG_WRITE_MASK_MASK);
	val |= COMM_CFG_ENABLE;
	val |= u32_encode_bits(WRITE_DATA_VALUE, COMM_CFG_WRITE_DATA_MASK);

	for (i = 0; i < PMA_REG_COUNT; i++) {
		u32 offset =  i * PMA_HWT_REFCK_STRIDE;

		/* Disable C0_REFCK and 100 ohm termination */
		writel(0, pmatop + PMA_HWT_REFCK_R_EN + offset);
		writel(0, pmatop + PMA_HWT_REFCK_TERM_EN + offset);

		/* Switch clock from C0_REFCK to CLK_REF_I */
		offset =  i * PMA_COMM_CFG_0_1_STRIDE;
		writel(val, pmatop + PMA_COMM_CFG_0_1 + offset);
	}

	WARN_ON(reset_control_deassert(rstc));

	ret = regmap_read_poll_timeout(regmap, EMAC_CTL_OFFSET(td->mac_id),
				       val, val & EMAC_INIT_DONE, 50, 1000000);
	WARN(ret, "timeout waiting for MAC %u init done\n", td->mac_id);
}

static int tc956x_mac_speed_select(struct tc956x_data *td,
				   phy_interface_t interface, int speed)
{
	struct net_device *netdev;
	int i;

	for (i = 0; i < ARRAY_SIZE(mac_speed); i++) {
		if (mac_speed[i].speed && mac_speed[i].speed != speed)
			continue;

		if (mac_speed[i].phy_interface == interface)
			return mac_speed[i].sp_sel;
	}
	netdev = dev_get_drvdata(td->dev);
	netdev_err(netdev, "%s/%d unsupported\n",
		   phy_modes(interface), speed);

	return -EOPNOTSUPP;
}

static int tc956x_mac_configure(struct tc956x_data *td,
				phy_interface_t interface, int speed)
{
	struct regmap *regmap = td->config_regmap;
	u32 offset = EMAC_CTL_OFFSET(td->mac_id);
	int sp_sel;
	u32 val;

	sp_sel = tc956x_mac_speed_select(td, interface, speed);
	if (sp_sel < 0)
		return sp_sel;

	/* No error checking needed for MMIO regmap */
	regmap_read(regmap, offset, &val);
	val |= EMAC_LPIHWCLKEN;
	val &= ~EMAC_INV_SGM_SIG_DET;
	val = u32_replace_bits(val, PCS_CLK_PHY, EMAC_PHY_INF_SEL_MASK);
	val = u32_replace_bits(val, sp_sel, EMAC_SP_SEL_MASK);
	regmap_write(regmap, offset, val);

	return 0;
}

static void tc956x_mac_enable(struct tc956x_data *td)
{
	WARN_ON(clk_bulk_prepare_enable(td->clock_count, td->clocks));

	WARN_ON(reset_control_deassert(td->resets[RESET_ID_MAC].rstc));

	tc956x_pma_init(td);

	WARN_ON(reset_control_deassert(td->resets[RESET_ID_XPCS].rstc));
}

static void tc956x_mac_disable(struct tc956x_data *td)
{
	WARN_ON(reset_control_bulk_assert(ARRAY_SIZE(td->resets), td->resets));

	clk_bulk_disable_unprepare(td->clock_count, td->clocks);
}

static void tc956x_mac_init_state(struct tc956x_data *td)
{
	tc956x_mac_disable(td);
}

/*
 * Override method for dwxgmac301_dma_ops->init_rx_chan
 *
 * This differs from the dwxgmac301_dma_ops->init_rx_chan by translating the DMA
 * address for TC956x internal bus. The window that provides DMA access to PCI
 * is linearly mapped at 0x10_0000_0000.
 */
static void tc956x_dma_init_rx_chan(struct stmmac_priv *priv,
				    void __iomem *ioaddr,
				    struct stmmac_dma_cfg *dma_cfg,
				    dma_addr_t phy, u32 chan)
{
	u64 translated = phy + TC956X_SLV00_SRC_ADDR;

	dwxgmac2_dma_init_rx_chan(priv, ioaddr, dma_cfg, phy, chan);

	writel(upper_32_bits(translated),
	       ioaddr + XGMAC_DMA_CH_RxDESC_HADDR(chan));
	writel(lower_32_bits(translated),
	       ioaddr + XGMAC_DMA_CH_RxDESC_LADDR(chan));
}

/* Override method for dwxgmac301_dma_ops->init_tx_chan */
static void tc956x_dma_init_tx_chan(struct stmmac_priv *priv,
				    void __iomem *ioaddr,
				    struct stmmac_dma_cfg *dma_cfg,
				    dma_addr_t phy, u32 chan)
{
	u64 translated = phy + TC956X_SLV00_SRC_ADDR;

	dwxgmac2_dma_init_tx_chan(priv, ioaddr, dma_cfg, phy, chan);

	writel(upper_32_bits(translated),
	       ioaddr + XGMAC_DMA_CH_TxDESC_HADDR(chan));
	writel(lower_32_bits(translated),
	       ioaddr + XGMAC_DMA_CH_TxDESC_LADDR(chan));
}

/* Override method for dwxgmac210_desc_ops->set_addr */
static void tc956x_desc_set_addr(struct dma_desc *p, dma_addr_t addr)
{
	u64 translated = addr + TC956X_SLV00_SRC_ADDR;

	p->des0 = cpu_to_le32(lower_32_bits(translated));
	p->des1 = cpu_to_le32(upper_32_bits(translated));
}

/* Override method for dwxgmac210_desc_ops->set_sec_addr */
static void tc956x_desc_set_sec_addr(struct dma_desc *p, dma_addr_t addr,
				     bool is_valid)
{
	u64 translated = addr + TC956X_SLV00_SRC_ADDR;

	p->des2 = cpu_to_le32(lower_32_bits(translated));
	p->des3 = cpu_to_le32(upper_32_bits(translated));
}

/*
 * Use mac_setup to apply the override methods above.
 *
 * The memory for the modified ops structures is pre-allocated as part of
 * struct tc956x_data.
 */
static int tc956x_mac_setup(void *apriv, struct mac_device_info *mac)
{
	struct stmmac_priv *priv = apriv;
	struct tc956x_data *td;

	td = priv->plat->bsp_priv;

	/* dwxgmac301_dma_ops needs extending to provide DMA address translation */
	td->dma = dwxgmac301_dma_ops;
	td->dma.init_rx_chan = tc956x_dma_init_rx_chan;
	td->dma.init_tx_chan = tc956x_dma_init_tx_chan;
	mac->dma = &td->dma;

	/* dwxgmac210_desc_ops also needs extending for the same reason */
	td->desc = dwxgmac210_desc_ops;
	td->desc.set_addr = tc956x_desc_set_addr;
	td->desc.set_sec_addr = tc956x_desc_set_sec_addr;
	mac->desc = &td->desc;

	priv->hw = mac;

	return dwxgmac2_setup(priv);
}

static int tc956x_pcs_init(struct stmmac_priv *priv)
{
	void __iomem *emac = priv->ioaddr;
	struct regmap *xpcs_regmap;
	void __iomem *xpcs_addr;
	struct dw_xpcs *xpcs;

	xpcs_addr = emac + DWMAC_XPCS_OFFSET;
	xpcs_regmap = devm_regmap_init_mmio(priv->device, xpcs_addr,
					    &xpcs_regmap_config);
	if (IS_ERR(xpcs_regmap))
		return PTR_ERR(xpcs_regmap);

	xpcs = devm_xpcs_regmap_register(priv->device, xpcs_regmap);
	if (IS_ERR(xpcs))
		return PTR_ERR(xpcs);

	xpcs_config_eee_mult_fact(xpcs, priv->plat->mult_fact_100ns);
	priv->hw->phylink_pcs = xpcs_to_phylink_pcs(xpcs);

	return 0;
}

static struct phylink_pcs *tc956x_select_pcs(struct stmmac_priv *priv,
					     phy_interface_t interface)
{
	return priv->hw->phylink_pcs;
}

static void tc956x_fix_mac_speed(void *bsp_priv, phy_interface_t interface,
				 int speed, unsigned int mode)
{
	struct tc956x_data *td = bsp_priv;

	tc956x_mac_configure(td, interface, speed);
	tc956x_pma_init(td);
}

static int tc956x_dwmac_suspend(struct device *dev, void *bsp_priv)
{
	struct tc956x_data *td = bsp_priv;

	tc956x_mac_disable(td);

	return 0;
}

static int tc956x_dwmac_resume(struct device *dev, void *bsp_priv)
{
	struct tc956x_data *td = bsp_priv;

	tc956x_mac_enable(td);

	return 0;
}

/* Called by tc956x_dwmac_probe(); return errors with dev_err_probe() */
static int tc956x_dwmac_parse_dt(struct tc956x_data *td)
{
	struct device_node *mdio_node;
	struct device *dev = td->dev;
	struct device_node *np;

	np = dev_of_node(dev);
	if (!np)
		return dev_err_probe(dev, -EINVAL, "no devicetree node\n");

	/* Find the MDIO bus */
	for_each_child_of_node(np, mdio_node)
		if (of_device_is_compatible(mdio_node, "snps,dwmac-mdio"))
			break;

	/* Pass the MDIO bus (if there is one) to the core driver */
	if (mdio_node) {
		td->plat->mdio_node = mdio_node;
		td->plat->mdio_bus_data->needs_reset = true;
	}

	return 0;
}

/* Called by tc956x_dwmac_probe(); return errors with dev_err_probe() */
static int tc956x_plat_dat_init(struct tc956x_data *td)
{
	struct plat_stmmacenet_data *plat;
	phy_interface_t phy_interface;
	struct device *dev = td->dev;
	struct stmmac_axi *axi;
	int ret;
	u32 i;

	ret = device_get_phy_mode(dev);
	if (ret < 0)
		return -ENODEV;
	phy_interface = ret;

	/* The platform structure is allocated with devm_kzalloc() */
	plat = stmmac_plat_dat_alloc(dev);
	if (!plat)
		return -ENOMEM;

	plat->core_type = DWMAC_CORE_XGMAC;
	plat->bus_id = td->mac_id;
	plat->phy_interface = phy_interface;
	plat->mdio_bus_data = &td->mdio_bus_data;
	/* Parent PCI device is used for DMA */
	plat->dma_device = dev->parent;
	plat->dma_cfg = &td->dma_cfg;
	plat->dma_cfg->pbl = 32;
	plat->dma_cfg->pblx8 = true;

	/*
	 * Our MAC clock rate is fixed at 125 MHz.  For XGMAC, clk_csr 0
	 * represents "divide by 62" and gets the best rate under 2.5 MHz.
	 */
	plat->clk_csr = 0;	/* MDC clock = clk_csr_i / 62 */
	plat->default_an_inband = true;
	plat->force_sf_dma_mode = true;
	plat->unicast_filter_entries = 32;

	/*
	 * TC956x has 8 RX queues but we observe significantly reduced RX
	 * bandwidth if we don't have at least 8k FIFO space per queue, so
	 * by default we avoid using all the queues.
	 */
	plat->rx_queues_to_use = 4;

	/*
	 * TC956x has 8 TX queues but only #0 to #3 work for general IP traffic.
	 * For now we will limit the driver to only these queues.
	 */
	plat->tx_queues_to_use = 4;

	/*
	 * Oversized FIFOs result in reduced performance in bandwidth tests.
	 * Limit them to 8KiB per queue, or the total available.
	 */
	plat->tx_fifo_size =
		min(TC956X_TX_FIFO_KB, 8 * plat->tx_queues_to_use) * SZ_1K;
	plat->rx_fifo_size =
		min(TC956X_RX_FIFO_KB, 8 * plat->rx_queues_to_use) * SZ_1K;
	plat->host_dma_width = 36;

	plat->rx_sched_algorithm = MTL_RX_ALGORITHM_SP;
	plat->tx_sched_algorithm = MTL_TX_ALGORITHM_WRR;

	/* Default RX chan is set to queue index (0..rx_queues_to_use-1) */
	for (i = 0; i < plat->rx_queues_to_use; i++)
		plat->rx_queues_cfg[i].mode_to_use = MTL_QUEUE_DCB;

	for (i = 0; i < plat->tx_queues_to_use; i++) {
		plat->tx_queues_cfg[i].weight = 12;
		plat->tx_queues_cfg[i].mode_to_use = MTL_QUEUE_DCB;

		/* Only queues 5-8 support time-based scheduling on TC956X */
		if (i >= 5)
			plat->tx_queues_cfg[i].tbs_en = 1;
	}

	plat->fix_mac_speed = tc956x_fix_mac_speed;
	plat->suspend = tc956x_dwmac_suspend;
	plat->resume = tc956x_dwmac_resume;
	plat->mac_setup = tc956x_mac_setup;
	plat->pcs_init = tc956x_pcs_init;
	plat->select_pcs = tc956x_select_pcs;

	plat->bsp_priv = td;
	plat->clk_ptp_rate = TC956X_PTP_CLOCK_RATE;

	/* AXI Configuration */
	axi = &td->axi;
	axi->axi_lpi_en = 1;
	axi->axi_wr_osr_lmt = 31;
	axi->axi_rd_osr_lmt = 31;
	/* All sizes (2^2..2^8) are supported */
	axi->axi_blen_regval = DMA_AXI_BLEN_MASK;
	plat->axi = axi;

	plat->flags = STMMAC_FLAG_MULTI_MSI_EN | STMMAC_FLAG_TSO_EN;

	td->plat = plat;

	return 0;
}

/*
 * The domain was created with IRQ_DOMAIN_FLAG_DESTROY_GC, so any mapped IRQs
 * will be disposed when the domain is removed (when the device is destroyed).
 */
static int tc956x_stmmac_resources_init(struct tc956x_data *td)
{
	struct irq_domain *irq_domain = td->irq_domain;
	struct stmmac_resources *res = &td->res;
	u32 i;

	res->irq = irq_create_mapping(irq_domain, HWIRQ_EVENT);
	if (!res->irq)
		return -EINVAL;

	for (i = 0; i < td->plat->tx_queues_to_use; i++) {
		res->tx_irq[i] = irq_create_mapping(irq_domain, HWIRQ_TX0 + i);
		if (!res->tx_irq[i])
			return -EINVAL;
	}

	for (i = 0; i < td->plat->rx_queues_to_use; i++) {
		res->rx_irq[i] = irq_create_mapping(irq_domain, HWIRQ_RX0 + i);
		if (!res->rx_irq[i])
			return -EINVAL;
	}

	res->addr = td->ioaddr;

	return 0;
}

static void tc956x_stmmac_resources_exit(struct tc956x_data *td)
{
	struct stmmac_resources *res = &td->res;
	u32 i;

	for (i = 0; i < td->plat->rx_queues_to_use; i++)
		irq_dispose_mapping(res->rx_irq[i]);

	for (i = 0; i < td->plat->tx_queues_to_use; i++)
		irq_dispose_mapping(res->tx_irq[i]);

	irq_dispose_mapping(res->irq);
}

/* Look up resets, and ensure they're initally all asserted */
static int tc956x_reset_init(struct tc956x_data *td)
{
	u32 reset_count = ARRAY_SIZE(td->resets);
	int ret;
	u32 i;

	for (i = 0; i < reset_count; i++)
		td->resets[i].id = tc956x_reset_names[i];

	ret = devm_reset_control_bulk_get_exclusive(td->dev, reset_count,
						    td->resets);
	if (ret)
		return ret;

	return reset_control_bulk_assert(reset_count, td->resets);
}

static int tc956x_clock_init(struct tc956x_data *td)
{
	struct device *dev = td->dev;
	u32 clock_count;
	int ret;
	u32 i;

	/* MAC 1 has one more clock than MAC0 does (RMII) */
	clock_count = ARRAY_SIZE(td->clocks);
	if (!td->mac_id)
		clock_count--;

	for (i = 0; i < clock_count; i++)
		td->clocks[i].id = tc956x_clock_names[i];

	ret = devm_clk_bulk_get(dev, clock_count, td->clocks);
	if (ret)
		return ret;

	td->clock_count = clock_count;

	return 0;
}

static int tc956x_dwmac_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tc956x_data *td;
	struct device_node *np;
	struct regmap *regmap;
	static u32 mac_id;
	int ret;

	dev_info(dev, " === %s starting\n", __func__);

	td = devm_kzalloc(dev, sizeof(*td), GFP_KERNEL);
	if (!td)
		return -ENOMEM;

	td->dev = dev;
	/* TODO */
#if 0
	td->auxbus_data = dev_get_platdata(dev);
	if (!td->auxbus_data)
		return dev_err_probe(dev, -EINVAL, "no platform data\n");
#endif

	np = dev_of_node(dev);
	regmap = syscon_regmap_lookup_by_phandle(np, "toshiba,config-syscon");
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to get config regmap\n");
	td->config_regmap = regmap;

	regmap = syscon_regmap_lookup_by_phandle(np, "toshiba,msigen-syscon");
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to get msigen regmap\n");
	td->msigen_regmap = regmap;
	td->msigen_irq = 0;	/* TODO Daniel */

	td->ioaddr = devm_of_iomap(dev, np, 0, NULL);
	if (IS_ERR(td->ioaddr))
		return dev_err_probe(dev, -EINVAL, "failed to map memory\n");

	/* XXX We need to know the MAC ID; this needs to be done differently */
	td->mac_id = mac_id++;

	ret = tc956x_reset_init(td);
	if (ret)
		return dev_err_probe(dev, ret ,"failed to initalize resets\n");

	ret = tc956x_clock_init(td);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initalize clocks\n");

	ret = tc956x_plat_dat_init(td);
	if (ret)
		return ret;

	/* If successful, we hold a reference to the platform MDIO DT node */
	ret = tc956x_dwmac_parse_dt(td);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse devicetree\n");

	td->irq_domain = tc956x_msigen_irq_domain_instantiate(td);
	if (IS_ERR(td->irq_domain)) {
		ret = dev_err_probe(dev, PTR_ERR(td->irq_domain),
				    "failed to instantiate IRQ domain\n");
		goto err_put_mdio;
	}

	ret = tc956x_stmmac_resources_init(td);
	if (ret) {
		ret = dev_err_probe(dev, ret,
				    "failed to initialize stmmac resources\n");
		goto err_put_mdio;
	}

	/* Put the MAC in a known initial state, then enable it */
	tc956x_mac_init_state(td);
	tc956x_mac_enable(td);

	dev_info(dev, " === %s successful (quitting early)\n", __func__);

	return 0;	/* TODO */

	ret = stmmac_dvr_probe(dev, td->plat, &td->res);
	if (ret) {
		ret = dev_err_probe(dev, ret, "failed stmmac probe\n");
		goto err_disable_mac;
	}

	dev_info(dev, " === %s successful\n", __func__);

	return 0;

err_disable_mac:
	tc956x_mac_disable(td);
	tc956x_stmmac_resources_exit(td);
err_put_mdio:
	of_node_put(td->plat->mdio_node);

	return ret;
}

static void tc956x_dwmac_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev = dev_get_drvdata(dev);
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct tc956x_data *td = priv->plat->bsp_priv;

	stmmac_dvr_remove(dev);
	tc956x_mac_disable(td);
	tc956x_stmmac_resources_exit(td);
	of_node_put(td->plat->mdio_node);
}

static const struct of_device_id tc956x_dwmac_ids[] = {
	{ .compatible = "toshiba,tc9564-xgmac", },
	{ },
};
MODULE_DEVICE_TABLE(of, tc956x_dwmac_ids);

static struct platform_driver tc956x_dwmac_driver = {
	.probe			= tc956x_dwmac_probe,
	.remove			= tc956x_dwmac_remove,
	.driver = {
		.name		= DRIVER_NAME,
		.of_match_table	= tc956x_dwmac_ids,
		.pm		= &stmmac_simple_pm_ops,
		.owner		= THIS_MODULE,
	},
};
module_platform_driver(tc956x_dwmac_driver);

MODULE_DESCRIPTION("Toshiba TC956x PCIe Ethernet Network Driver");
MODULE_LICENSE("GPL");
