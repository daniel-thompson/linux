// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 */

/*
 * The Toshiba TC9564 implements a PCIe Gen 3 switch that connects an
 * upstream x4 port to three downstream PCIe ports--two external ones
 * and an internal one which implements an internal PCIe endpoint.  The
 * endpoint implements two PCIe functions, each having a Synopsys XGMAC
 * Ethernet interface.
 *
 * The XGMACs access the PCIe bus via an AXI bus.  The AXI bus uses
 * addresses above 64 GB (2^36), and an address translation unit
 * translates between this AXI bus space and PCIe bus space.
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define DRIVER_NAME			"tc9564-translate"

/*
 * The bus translation function has four AXI translation table entries
 * each with eight 4-byte registers.  These entries translate between
 * an internal AXI bus address space and "external" PCIe address space.
 * The Ethernet MACs access the PCIe subsystem via this bus.  Currently
 * we only use the first translation table entry.
 */
#define AXI4_ENTRY_BASE(id)		((id) * AXI4_TABLE_STRIDE)
#define AXI4_TABLE_ENTRY_COUNT		4
#define AXI4_TABLE_STRIDE               0x20

/*
 * Address translation space parameters used for entry 0.
 *
 * The size value determines the size (2^(size+1)) of the AXI bus address
 * space, which begins at TC9564_SLV00_SRC_ADDR.  It defines a mask that
 * extracts the lower bits from the AXI space to determine the bus-relative
 * offset.  That address is added (actually, OR'd) to the translated base
 * address (recorded in TRSL_ADDR_HI and TRSL_ADDR_LO registers) to produce
 * a PCIe bus address.  We simply use zero as the translated base address.
 */
#define TC9564_SLV00_SRC_ADDR		0x0000001000000000ULL
#define SLV00_ATR_SIZE			35	/* 2^36 (64 gigabytes) */

/* Translation entry registers, fields, and values used */
#define SRC_ADDR_LO_OFFSET		0x0000
#define ATR_IMPL			BIT(0)		/* 1 = enabled */
#define ATR_SIZE_MASK			GENMASK(6, 1)	/* 2^(SIZE+1) */
#define SRC_ADDR_HI_OFFSET		0x0004
#define TRSL_ADDR_LO_OFFSET		0x0008
#define TRSL_ADDR_HI_OFFSET		0x000c
#define TRSL_PARAM_OFFSET		0x0010
#define TRSL_ID_MASK			GENMASK(3, 0)
#define TRSL_ID_PCIE_TX_RX		0
#define TRSL_PARAM_MASK			GENMASK(27, 16)

/*
 * The lower bits of the source address must be zero, because the
 * "implemented" bit and the address translation space size are
 * encoded there in the SRC_ADDR_LO register.
 */
static_assert(!(TC9564_SLV00_SRC_ADDR & ATR_IMPL));
static_assert(!(lower_32_bits(TC9564_SLV00_SRC_ADDR) & ATR_SIZE_MASK));

/*
 * The size field defines the size of the translation space as
 * (2^(ATR_SIZE + 1)).  The minimum size is 4096 bytes, so ATR_SIZE
 * value must be 11 or more.
 */
static_assert(SLV00_ATR_SIZE >= 11);

struct tc9564_translate {
	struct regmap *regmap;
	u32 offset;		/* Offset to the translation table base */
};

/**
 * tc9564_translate_entry() - Configure one translation table entry
 * @regmap:	Regmap used to configure the translation table entries
 * @offset:	Offset of the base of the entry within the regmap
 * @src:	64-bit source address (translated address is always 0x0)
 * @atr_size:	Translation address space size endcoding
 * @trsl_param:	Translation parameter value
 */
static void tc9564_translate_entry(struct regmap *regmap, u32 offset, u64 src,
				   u32 atr_size, u32 trsl_param)
{
	u32 val;

	val = lower_32_bits(src) | atr_size;
	/* No errors returned for MMIO regmap */
	regmap_write(regmap, offset + SRC_ADDR_LO_OFFSET, val);

	val = upper_32_bits(src);
	regmap_write(regmap, offset + SRC_ADDR_HI_OFFSET, val);

	/* The translated base address is always just 0x0 */
	regmap_write(regmap, offset + TRSL_ADDR_LO_OFFSET, 0);
	regmap_write(regmap, offset + TRSL_ADDR_HI_OFFSET, 0);
}

/**
 * tc9564_translate_config() - Configure the translation unit registers
 * @translate:	Private translation structure
 *
 * Define the translation between AXI bus accesses and PCI TLPs.
 * TC9564_SLV00_SRC_ADDR defines the base address of the AXI address
 * range.  AXI addresses are translated to the PCIe address range,
 * whose base address we set to be 0x0.
 */
static void tc9564_translate_config(struct tc9564_translate *translate)
{
	struct regmap *regmap = translate->regmap;
	u32 offset = translate->offset;
	u32 trsl_param;
	u32 atr_size;

	/* This TRSL_PARAM value is assigned for all four table entries */
	trsl_param = u32_encode_bits(TRSL_ID_PCIE_TX_RX, TRSL_ID_MASK);

	/*
	 * We only use the first AXI4 translation table entry:
	 *	EDMA address region:	0x10 0000 0000 - 0x1f ffff ffff
	 *	is translated to:	0x00 0000 0000 - 0x0f ffff ffff
	 */
	atr_size = u32_encode_bits(SLV00_ATR_SIZE, ATR_SIZE_MASK);
	atr_size |= ATR_IMPL;
	tc9564_translate_entry(regmap, offset, TC9564_SLV00_SRC_ADDR,
			       atr_size, trsl_param);

	/* Set all other unused entries to default values (no translation) */
	for (u32 i = 1; i < AXI4_TABLE_ENTRY_COUNT; i++)
		tc9564_translate_entry(regmap, offset + AXI4_ENTRY_BASE(i),
				       0, 0, trsl_param);
}

static int tc9564_translate_probe(struct platform_device *pdev)
{
	struct tc9564_translate *translate;
	struct device *dev = &pdev->dev;
	struct device_node *np;
	struct regmap *regmap;
	u32 min_size;
	u64 offset;
	u64 size;
	int ret;

	np = dev_of_node(dev);
	if (!np)
		return dev_err_probe(dev, -EINVAL, "no devicetree node\n");

	ret = of_property_read_reg(np, 0, &offset, &size);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get reg property\n");

	min_size = AXI4_TABLE_ENTRY_COUNT * AXI4_TABLE_STRIDE;
	if (size < min_size)
		return dev_err_probe(dev, -EINVAL,
				     "reg size too small (%llu < %u)\n",
				     size, min_size);

	regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to get bridge regmap\n");

	translate = devm_kzalloc(dev, sizeof(*translate), GFP_KERNEL);
	if (!translate)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate translation data\n");

	translate->regmap = regmap;
	translate->offset = offset;

	dev_set_drvdata(dev, translate);

	/* Do the initial configuraiton */

	tc9564_translate_config(translate);

	return 0;
}

static int tc9564_translate_suspend_noirq(struct device *dev)
{
	return 0;
}

/* We need to reconfigure address translation when we resume */
static int tc9564_translate_resume_noirq(struct device *dev)
{
	struct tc9564_translate *translate = dev_get_drvdata(dev);

	tc9564_translate_config(translate);

	return 0;
}

static DEFINE_NOIRQ_DEV_PM_OPS(tc9564_translate_pm_ops,
			       tc9564_translate_suspend_noirq,
			       tc9564_translate_resume_noirq);

static const struct of_device_id tc9564_translate_match[] = {
	{ .compatible	= "toshiba,tc9564-translate", },
	{ },
};
MODULE_DEVICE_TABLE(of, tc9564_translate_match);

static struct platform_driver tc9564_translate_driver = {
	.probe		= tc9564_translate_probe,
	.driver		= {
		.name		= DRIVER_NAME,
		.of_match_table	= of_match_ptr(tc9564_translate_match),
		.pm		= pm_sleep_ptr(&tc9564_translate_pm_ops),
	},
};

module_platform_driver(tc9564_translate_driver);

MODULE_DESCRIPTION("Toshiba TC9564 Configuration Driver");
MODULE_LICENSE("GPL");
