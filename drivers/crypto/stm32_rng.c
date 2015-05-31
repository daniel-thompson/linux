/*
 * stm32_rng - STMicroelectronics STM32 RNG device driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/slab.h>

#define RNG_CR 0x00
#define RNG_CR_RNGEN BIT(2)

#define RNG_SR 0x04
#define RNG_SR_SEIS BIT(6)
#define RNG_SR_CEIS BIT(5)
#define RNG_SR_DRDY BIT(0)

#define RNG_DR 0x08

/*
 * It takes 40 cycles @ 48MHz to generate each random number (e.g. <1us).
 * At the time of writing STM32 parts max out at ~200MHz meaning a timeout
 * of 500 leaves us a very comfortable margin for error. The loop to which
 * the timeout applies takes at least 4 instructions per cycle so the
 * timeout is enough to take us up to multi-GHz parts!
 */
#define RNG_TIMEOUT 500

struct stm32_rng_private {
	struct hwrng rng;
	void __iomem *base;
	struct clk *clk;
};

static int stm32_rng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	struct stm32_rng_private *priv =
	    container_of(rng, struct stm32_rng_private, rng);
	u32 cr, sr;
	int retval = 0;

	/* enable random number generation */
	clk_enable(priv->clk);
	cr = readl(priv->base + RNG_CR);
	writel(cr | RNG_CR_RNGEN, priv->base + RNG_CR);

	while (max > sizeof(u32)) {
		sr = readl(priv->base + RNG_SR);
		if (!sr && wait) {
			unsigned int timeout = RNG_TIMEOUT;

			do {
				cpu_relax();
				sr = readl(priv->base + RNG_SR);
			} while (!sr && --timeout);
		}

		/* Has h/ware error dection been triggered? */
		if (WARN_ON(sr & (RNG_SR_SEIS | RNG_SR_CEIS)))
			break;

		/* No data ready... */
		if (!sr)
			break;

		*(u32 *)data = readl(priv->base + RNG_DR);

		retval += sizeof(u32);
		data += sizeof(u32);
		max -= sizeof(u32);
	}

	/* disable the generator */
	writel(cr, priv->base + RNG_CR);
	clk_disable(priv->clk);

	return retval || !wait ? retval : -EIO;
}

static int stm32_rng_init(struct hwrng *rng)
{
	struct stm32_rng_private *priv =
	    container_of(rng, struct stm32_rng_private, rng);
	int err;
	u32 sr;

	err = clk_prepare(priv->clk);
	if (err)
		return err;

	/* clear error indicators */
	sr = readl(priv->base + RNG_SR);
	sr &= ~(RNG_SR_SEIS | RNG_SR_CEIS);
	writel(sr, priv->base + RNG_SR);

	return 0;
}

static void stm32_rng_cleanup(struct hwrng *rng)
{
	struct stm32_rng_private *priv =
	    container_of(rng, struct stm32_rng_private, rng);

	clk_unprepare(priv->clk);
}

static int stm32_rng_remove(struct platform_device *ofdev)
{
	struct device *dev = &ofdev->dev;
	struct stm32_rng_private *priv = dev_get_drvdata(dev);

	hwrng_unregister(&priv->rng);
	iounmap(priv->base);
	kfree(priv);

	return 0;
}

static int stm32_rng_probe(struct platform_device *ofdev)
{
	struct device *dev = &ofdev->dev;
	struct device_node *np = ofdev->dev.of_node;
	struct stm32_rng_private *priv;
	int err;

	priv = kzalloc(sizeof(struct stm32_rng_private), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev_set_drvdata(dev, priv);

	priv->base = of_iomap(np, 0);
	if (!priv->base) {
		dev_err(dev, "failed to of_iomap\n");
		err = -ENOMEM;
		goto err_out;
	}

	priv->clk = devm_clk_get(&ofdev->dev, NULL);
	if (IS_ERR(priv->clk)) {
		dev_err(dev, "cannot get clock\n");
		goto err_out;
	}

	priv->rng.name = dev_driver_string(dev),
	priv->rng.init = stm32_rng_init,
	priv->rng.cleanup = stm32_rng_cleanup,
	priv->rng.read = stm32_rng_read,
	priv->rng.priv = (unsigned long) dev;

	err = hwrng_register(&priv->rng);
	if (err) {
		dev_err(dev, "failed to register hwrng: %d\n", err);
		goto err_out;
	}

	return 0;

err_out:
	stm32_rng_remove(ofdev);

	return err;
}

static const struct of_device_id stm32_rng_match[] = {
	{
		.compatible = "st,stm32-rng",
	},
	{},
};
MODULE_DEVICE_TABLE(of, stm32_rng_match);

static struct platform_driver stm32_rng_driver = {
	.driver = {
		.name = "stm32_rng",
		.of_match_table = stm32_rng_match,
	},
	.probe = stm32_rng_probe,
	.remove = stm32_rng_remove,
};

module_platform_driver(stm32_rng_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel Thompson <daniel.thompson@linaro.org>");
MODULE_DESCRIPTION("STMicroelectronics STM32 RNG device driver");
