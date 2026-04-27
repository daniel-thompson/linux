// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 */

#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/toshiba,tc9564.h>

#define DRIVER_NAME	"tc9564-clk"

struct tc9564_clock_init {
	const char *name;	/* NULL means unused entry */
	u32 offset_index;	/* Index into clocks->offset[] */
	u32 mask;
};

struct tc9564_clock {
	struct clk_hw hw;
	u32 which;
	u32 offset;
	u32 mask;		/* Zero means undefined clock */
};

struct tc9564_clocks {
	struct device *dev;
	struct regmap *regmap;
	u32 offset[2];
	size_t clock_count;
	struct tc9564_clock clocks[] __counted_by(clock_count);
};

#define TC9564_CLOCK_INIT(_name, _offset_index, _bit) \
	[CLOCK_##_name] = {                           \
		.name = #_name,                       \
		.offset_index = _offset_index,        \
		.mask = BIT(_bit),                    \
	}

static const struct tc9564_clock_init tc9564_clock_init[] = {
	TC9564_CLOCK_INIT(MCU, 0, 0),
	TC9564_CLOCK_INIT(INTC, 0, 4),
	/* TC9564_CLOCK_INIT(PCIE, 0, 9), */
	/* TC9564_CLOCK_INIT(I2C, 0, 12), */
	TC9564_CLOCK_INIT(SRAM, 0, 13),
	TC9564_CLOCK_INIT(UART, 0, 16),
	TC9564_CLOCK_INIT(MSIGEN, 0, 18),
	TC9564_CLOCK_INIT(PLL, 0, 24),
	TC9564_CLOCK_INIT(SGMII, 0, 25),
	TC9564_CLOCK_INIT(REFCLKO, 0, 26),

	TC9564_CLOCK_INIT(MAC0_TX, 0, 7),
	TC9564_CLOCK_INIT(MAC0_RX, 0, 14),
	TC9564_CLOCK_INIT(MAC0_125M, 0, 29),
	TC9564_CLOCK_INIT(MAC0_312_5M, 0, 30),
	TC9564_CLOCK_INIT(MAC0_ALL, 0, 31),

	TC9564_CLOCK_INIT(MAC1_TX, 1, 7),
	TC9564_CLOCK_INIT(MAC1_RX, 1, 14),
	TC9564_CLOCK_INIT(MAC1_RMII, 1, 15),
	TC9564_CLOCK_INIT(MAC1_125M, 1, 29),
	TC9564_CLOCK_INIT(MAC1_312_5M, 1, 30),
	TC9564_CLOCK_INIT(MAC1_ALL, 1, 31),
};
#define TC9564_CLOCK_COUNT	ARRAY_SIZE(tc9564_clock_init)

static struct tc9564_clock *hw_to_tc9564_clock(struct clk_hw *hw)
{
	return container_of(hw, struct tc9564_clock, hw);
}

static struct tc9564_clocks *tc9564_clock_to_clocks(struct tc9564_clock *clock)
{
	return container_of(clock, struct tc9564_clocks, clocks[clock->which]);
}

static int tc9564_clk_manage(struct clk_hw *hw, bool enable)
{
	struct tc9564_clock *clock = hw_to_tc9564_clock(hw);
	struct tc9564_clocks *clocks;
	u32 offset = clock->offset;
	u32 mask = clock->mask;

	clocks = tc9564_clock_to_clocks(clock);

	if (!clock->mask) {
		dev_err(clocks->dev, "invalid clock (%s)\n",
			enable ? "enable" : "disable");
		return -ENXIO;
	}

	return regmap_update_bits(clocks->regmap, offset, mask,
				  enable ? mask : 0);
}

static int tc9564_clk_enable(struct clk_hw *hw)
{
	return tc9564_clk_manage(hw, true);
}

static void tc9564_clk_disable(struct clk_hw *hw)
{
	(void) tc9564_clk_manage(hw, false);
}

static const struct clk_ops tc9564_clk_ops = {
	.enable = tc9564_clk_enable,
	.disable = tc9564_clk_disable,
};

static void tc9564_clock_disable_all(struct tc9564_clocks *clocks)
{
	for (int i = 0; i < clocks->clock_count; i++) {
		struct tc9564_clock *clock = &clocks->clocks[i];

		if (clock->mask)
			regmap_update_bits(clocks->regmap, clock->offset,
					   clock->mask, 0);
	}
}

static struct clk_hw *tc9564_clk_hw_get(struct of_phandle_args *clkspec,
					void *data)
{
	struct tc9564_clocks *clocks = data;
	unsigned int i = clkspec->args[0];

	if (i >= clocks->clock_count) {
		dev_err(clocks->dev, "invalid index %u\n", i);
		return ERR_PTR(-EINVAL);
	}

	return &clocks->clocks[i].hw;
}

static int tc9564_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tc9564_clocks *clocks;
	struct device_node *np;
	size_t clocks_size;
	int reg_size;
	u64 addr;
	u64 size;
	int ret;

	np = dev_of_node(dev);
	if (!np)
		return dev_err_probe(dev, -EINVAL, "no devicetree node\n");

	clocks_size = struct_size(clocks, clocks, TC9564_CLOCK_COUNT);
	clocks = devm_kzalloc(dev, clocks_size, GFP_KERNEL);
	if (!clocks)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate clocks\n");

	clocks->dev = dev;
	clocks->clock_count = TC9564_CLOCK_COUNT;

	clocks->regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(clocks->regmap))
		return dev_err_probe(dev, PTR_ERR(clocks->regmap),
				     "failed to get config regmap\n");
	reg_size = regmap_get_val_bytes(clocks->regmap);

	for (int i = 0; i < 2; i++) {
		ret = of_property_read_reg(np, i, &addr, &size);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to get reg %d\n", i);

		if (size != reg_size)
			return dev_err_probe(dev, -EINVAL,
					     "bad reg %d size %llu\n", i, size);

		clocks->offset[i] = lower_32_bits(addr);
	}

	for (int i = 0; i < TC9564_CLOCK_COUNT; i++) {
		const struct tc9564_clock_init *clock_init;
		struct clk_init_data init = { };
		struct tc9564_clock *clock;

		clock_init = &tc9564_clock_init[i];
		if (!clock_init->name)
			continue;

		init.name = clock_init->name;
		init.ops = &tc9564_clk_ops;

		clock = &clocks->clocks[i];
		clock->hw.init = &init;

		ret = devm_clk_hw_register(dev, &clock->hw);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to register clock \"%s\"\n",
					     clock_init->name);

		clock->which = i;
		clock->offset = clocks->offset[clock_init->offset_index];
		clock->mask = clock_init->mask;
	}

	ret = devm_of_clk_add_hw_provider(dev, tc9564_clk_hw_get, clocks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add clock\n");

	platform_set_drvdata(pdev, clocks);

	/* Force all clocks to be initially disabled */
	tc9564_clock_disable_all(clocks);

	return 0;
}

static void tc9564_clk_remove(struct platform_device *pdev)
{
	struct tc9564_clocks *clocks = platform_get_drvdata(pdev);

	/* Leave all clocks disabled when done */
	tc9564_clock_disable_all(clocks);
}

static const struct of_device_id tc9564_clk_ids[] = {
	{ .compatible = "toshiba,tc9564-clock" },
	{ },
};
MODULE_DEVICE_TABLE(of, tc9564_clk_ids);

static struct platform_driver tc9564_clk_driver = {
	.probe	= tc9564_clk_probe,
	.remove	= tc9564_clk_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table = tc9564_clk_ids,
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_platform_driver(tc9564_clk_driver);

MODULE_DESCRIPTION("Toshiba TC9564 Clock Driver");
MODULE_LICENSE("GPL");
