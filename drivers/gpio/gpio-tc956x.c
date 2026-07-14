// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 */

/*
 * The Toshiba TC956X implements a PCIe Gen 3 switch that connects an
 * upstream x4 port to two downstream PCIe x2 ports.  It incorporates
 * an internal endpoint on a internal PCIe port that implements two
 * Synopsys XGMAC Ethernet interfaces.
 *
 * 35 GPIOs are also implemented by an embedded GPIO controller.  Three
 * registers control the first 32 GPIOs (other than 20 and 21, which are
 * reserved).  Three other registers control GPIOs 32 through 36. GPIOs
 * 22-24, 27-28, 31, and 34 are treated as "input only".
 *
 * There is a TC956X PCI power controller driver that accesses the
 * direction and output value registers for GPIOs 2 and 3.  These
 * GPIOs control the reset signal for the two downstream PCIe ports.
 * Their values will never change during operation of this driver, and
 * this driver reserves these two GPIOS.
 */

#include <linux/gpio/driver.h>
#include <linux/gpio/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define DRIVER_NAME		"tc956x-gpio"

#define TC956X_GPIO_COUNT	37	/* Number of GPIOs (20-21 reserved) */

/*
 * These offsets are relative to the base of the chip configuration space
 * mapped by the system controller referred to by the "toshiba,config-syscon"
 * property.
 */
#define GPIO_IN0_OFFSET		0x1200		/* Input value (0-31) */
#define GPIO_EN0_OFFSET		0x1208		/* 0: out; 1: in (0-31) */
#define GPIO_OUT0_OFFSET	0x1210		/* Output value (0-31) */

/*
 * There are two sets of registers, each representing (up to) 32 GPIOs with a
 * stride of 4 bytes (IN1 is 4 bytes past IN0, EN1 is 4 bytes past EN0, etc.).
 */
#define GPIO_PER_REG		32
#define GPIO_REG_STRIDE		4

static int tc956x_gpio_init_valid_mask(struct gpio_chip *gc,
				       unsigned long *valid_mask,
				       unsigned int ngpios)
{
	/*
	 * GPIOs 2 and 3 are used by the PCI power control driver, and
	 * we don't allow them to be used.  GPIOs 20 and 21 are reserved
	 * (and not usable).
	 */
	bitmap_fill(valid_mask, ngpios);
	bitmap_clear(valid_mask, 2, 2);
	bitmap_clear(valid_mask, 20, 2);

	return 0;
}

static int tc956x_gpio_probe(struct platform_device *pdev)
{
	DECLARE_BITMAP(zeroes, TC956X_GPIO_COUNT);
	DECLARE_BITMAP(fixed, TC956X_GPIO_COUNT);
	struct gpio_regmap_config config = { };
	struct gpio_regmap *gpio_regmap;
	struct device *dev = &pdev->dev;
	struct regmap *regmap;

	regmap = syscon_regmap_lookup_by_phandle(dev_of_node(dev),
						 "toshiba,config-syscon");
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to get config regmap\n");

	/*
	 * Only some of our GPIOs are fixed direction:
	 *	22, 23, 24, 27, 28, 31, and 34	(all input-only)
	 * Set up the fixed bitmap to indicate which are fixed.
	 */
	bitmap_zero(fixed, TC956X_GPIO_COUNT);
	bitmap_set(fixed, 22, 3);
	bitmap_set(fixed, 27, 2);
	set_bit(31, fixed);
	set_bit(34, fixed);

	/* All fixed GPIOs are input; the zeroes bitmap indicates that. */
	bitmap_zero(zeroes, TC956X_GPIO_COUNT);

	config.parent = dev;
	config.regmap = regmap;
	config.label = DRIVER_NAME;
	config.ngpio = TC956X_GPIO_COUNT;
	config.reg_dat_base = GPIO_REGMAP_ADDR(GPIO_IN0_OFFSET);
	config.reg_set_base = GPIO_REGMAP_ADDR(GPIO_OUT0_OFFSET);
	config.reg_dir_in_base = GPIO_REGMAP_ADDR(GPIO_EN0_OFFSET);
	config.reg_stride = GPIO_REG_STRIDE;
	config.ngpio_per_reg = GPIO_PER_REG;
	config.init_valid_mask = tc956x_gpio_init_valid_mask;
	config.fixed_direction_mask = fixed;
	config.fixed_direction_output = zeroes;

	gpio_regmap = devm_gpio_regmap_register(dev, &config);
	if (IS_ERR(gpio_regmap))
		return dev_err_probe(dev, PTR_ERR(gpio_regmap),
				     "registration failed\n");

	return 0;
};

static void tc956x_gpio_remove(struct platform_device *pdev)
{
	/* Nothing to do for now */
}

static const struct of_device_id tc956x_gpio_ids[] = {
	{ .compatible	= "toshiba,tc9564-gpio", },
	{ },
};
MODULE_DEVICE_TABLE(of, tc956x_gpio_ids);

static struct platform_driver tc956x_gpio_driver = {
	.probe	= tc956x_gpio_probe,
	.remove	= tc956x_gpio_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= tc956x_gpio_ids,
		.owner		= THIS_MODULE,
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_platform_driver(tc956x_gpio_driver);

MODULE_DESCRIPTION("Toshiba TC956X GPIO Driver");
MODULE_LICENSE("GPL");
