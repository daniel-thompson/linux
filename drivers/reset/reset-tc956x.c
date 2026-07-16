// SPDX-License-Identifier: GPL-2.0
//

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 */

#include <linux/bits.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#include <dt-bindings/reset/toshiba,tc9564.h>

#define DRIVER_NAME		"tc956x-reset"

struct tc956x_reset {
	u32 zero_one;		/* Index into resets->offset[] */
	u32 mask;		/* Zero means undefined reset */
};

struct tc956x_resets {
	struct regmap *regmap;
	u32 offset[2];
	struct reset_controller_dev rcdev;
};

#define TC956X_RESET_INIT(_name, _zero_one, _bit)	\
	[RESET_ ## _name] = {				\
		.zero_one	= _zero_one,		\
		.mask		= BIT(_bit),		\
	}

static const struct tc956x_reset tc9564_reset[] = {
	TC956X_RESET_INIT(MCU, 0, 0),
	TC956X_RESET_INIT(MCU1, 0, 1),
	TC956X_RESET_INIT(INTC, 0, 4),
	/* TC956X_RESET_INIT(PCIE, 0, 9), */
	/* TC956X_RESET_INIT(I2C, 0, 12), */
	TC956X_RESET_INIT(UART, 0, 16),
	TC956X_RESET_INIT(MSIGEN, 0, 18),

	TC956X_RESET_INIT(MAC0_MAC, 0, 7),
	TC956X_RESET_INIT(MAC0_PMA, 0, 30),
	TC956X_RESET_INIT(MAC0_XPCS, 0, 31),

	TC956X_RESET_INIT(MAC1_MAC, 1, 7),
	TC956X_RESET_INIT(MAC1_PMA, 1, 30),
	TC956X_RESET_INIT(MAC1_XPCS, 1, 31),
};

/* Mask that includes all meaningful bits in each reset control register */
#define TC956X_RESET0_ALL_MASK	0xc0050093	/* 0xc0051293 */
#define TC956X_RESET1_ALL_MASK	0xc0000080

static struct tc956x_resets *
tc956x_rcdev_to_resets(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct tc956x_resets, rcdev);
}

static int
tc956x_reset_manage(struct tc956x_resets *resets, unsigned long id, bool assert)
{
	const struct tc956x_reset *reset = &tc9564_reset[id];

	if (id < resets->rcdev.nr_resets && reset->mask) {
		u32 offset = resets->offset[reset->zero_one];
		struct regmap *regmap = resets->regmap;
		u32 mask = reset->mask;

		/* No errors returned for MMIO regmap */
		regmap_update_bits(regmap, offset, mask, assert ? mask : 0);

		return 0;
	}

	pr_warn("invalid reset (%sassert id %lu)!\n", assert ? "" : "de", id);

	return -EINVAL;
}

static int tc956x_reset_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct tc956x_resets *resets = tc956x_rcdev_to_resets(rcdev);

	return tc956x_reset_manage(resets, id, true);
}

static int tc956x_reset_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	struct tc956x_resets *resets = tc956x_rcdev_to_resets(rcdev);

	return tc956x_reset_manage(resets, id, false);
}

static const struct reset_control_ops tc956x_reset_control_ops = {
	.assert		= tc956x_reset_assert,
	.deassert	= tc956x_reset_deassert,
};

static void tc956x_reset_assert_all(struct tc956x_resets *resets)
{
	struct regmap *regmap = resets->regmap;

	regmap_write(regmap, resets->offset[0], TC956X_RESET0_ALL_MASK);
	regmap_write(regmap, resets->offset[1], TC956X_RESET1_ALL_MASK);
}

static int tc956x_reset_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tc956x_resets *resets;
	struct device_node *np;
	struct regmap *regmap;
	u32 offset[2];
	int reg_size;
	u64 addr;
	u64 size;
	int ret;
	int i;

	np = dev_of_node(dev);
	if (!np)
		return dev_err_probe(dev, -EINVAL, "no devicetree node\n");

	regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to get config regmap\n");
	reg_size = regmap_get_val_bytes(regmap);

	for (i = 0; i < 2; i++) {
		ret = of_property_read_reg(np, i, &addr, &size);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to get offset %d\n", i);

		if (size != reg_size)
			return dev_err_probe(dev, -EINVAL,
					     "bad offset %d size %llu\n", i,
					     size);
		offset[i] = lower_32_bits(addr);
	}

	resets = kzalloc_obj(*resets);
	if (!resets)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate resets\n");

	resets->regmap = regmap;
	resets->offset[0] = offset[0];
	resets->offset[1] = offset[1];

	resets->rcdev.ops = &tc956x_reset_control_ops;
	resets->rcdev.owner = THIS_MODULE;
	resets->rcdev.dev = dev;
	resets->rcdev.of_node = np;
	resets->rcdev.nr_resets = ARRAY_SIZE(tc9564_reset);

	ret = reset_controller_register(&resets->rcdev);
	if (ret) {
		kfree(resets);
		return dev_err_probe(dev, ret, "failed registration\n");
	}
	platform_set_drvdata(pdev, resets);

	/* Force all resets to be initially asserted */
	tc956x_reset_assert_all(resets);

	return 0;
}

static void tc956x_reset_remove(struct platform_device *pdev)
{
	struct tc956x_resets *resets = platform_get_drvdata(pdev);

	/* Leave all resets asserted when done */
	tc956x_reset_assert_all(resets);
	kfree(resets);
}

static const struct of_device_id tc956x_reset_ids[] = {
	{ .compatible = "toshiba,tc956x-resets" },
	{ },
};
MODULE_DEVICE_TABLE(of, tc956x_reset_ids);

static struct platform_driver tc956x_reset_driver = {
	.probe	= tc956x_reset_probe,
	.remove	= tc956x_reset_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table = tc956x_reset_ids,
		.owner		= THIS_MODULE,
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_platform_driver(tc956x_reset_driver);

MODULE_DESCRIPTION("Toshiba TC956X Reset Driver");
MODULE_LICENSE("GPL");
