// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 */

/*
 * The Toshiba TC956X implements a PCIe Gen 3 switch that connects an
 * upstream x4 port to three downstream PCIe ports--two external ones
 * and an internal one which implements an internal PCIe endpoint.  The
 * endpoint implements two PCIe functions, each having a Synopsys XGMAC
 * Ethernet interface.
 *
 * The TC956X implements other functionality, including an embedded
 * MCU, a UART, a GPIO controller, internal resets and clocks, and
 * interrupt handling.  These features are separate from (and in some
 * cases used by) both Ethernet XGMACs.  Each Ethernet MAC must be
 * attached to a working PHY for it to be functional, and for this
 * reason either of them (or both!) might not be usable/used.
 *
 * To support the non-XGMAC functionality on the TC956X regardless of
 * the presence of either Ethernet PHY, the Ethernet functions are
 * treated as two parts:  a PCIe function; and a Synopsys XGMAC component.
 * The PCIe function has access to the BARs used by the XGMAC, and maps
 * them for use.  Each XGMAP is treated as an auxiliary sub-device of
 * its (parent) PCIe function, and is probed and bound separate from it.
 *
 * This PCI driver binds to the Toshiba TC956X (physical) PCI function
 * (VID 0x1179, DID 0x0220).  There are two of these present on the
 * TC956X SoC.  This driver maps the PCI BARs and performs other initial
 * setup, then creates auxiliary devices.
 *
 * Embedded PCI function 0 manages non-MAC functionality.  This includes
 * creating and registering the GPIO auxiliary device (if necessary), as
 * well as asserting and deasserting internal reset signals and enabling
 * and disabling internal clocks.
 *
 * Both PCI functions create auxiliary devices to implement an Ethernet
 * XGMAC.  A block of data (struct tc956x_dwmac_data) is shared using
 * the auxiliary device's platform data with the stmmac driver that
 * binds to the XGMAC auxiliary device.  This includes a number of
 * pointers to memory regions used by the stmmac driver.
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/compiler_types.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/dev_printk.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/types.h>

#include <soc/toshiba/tc956x-dwmac.h>

#define DRIVER_NAME			TC956X_PCIE_DRIVER_NAME

#define PCI_DEVICE_ID_TOSHIBA_TC956X	0xabcd	/* 0x0220 */

/* PCI BAR assignments */
#define PCI_BAR_SFR			4	/* For all other features */

/* Reset and clock register offsets.  MAC resets and clocks are controlled
 * by bits in register 0 for MAC0, register 1 for MAC1.  Other non-MAC
 * resets and clocks (whose IDs are defined here) are controlled by bits
 * in register 0.
 *
 * These are relative to the base of the clock/reset regmap.
 */
#define RSTCTRL0_OFFSET			0x0008
#define RSTCTRL1_OFFSET			0x0010
#define CLKCTRL0_OFFSET			0x0004
#define CLKCTRL1_OFFSET			0x000c

/* Resets (asserted or deasserted) */
enum reset_id {
	RESET_MCU		= 0,
	RESET_MCU1		= 1,
	RESET_MSIGEN		= 18,
	RESET_INTC		= 4,
	RESET_UART0		= 16,
};

/* Clocks (enabled or disabled) */
enum clock_id {
	CLOCK_MCU		= 0,
	CLOCK_SRAM		= 13,
	CLOCK_MSIGEN		= 18,
	CLOCK_PLL		= 24,
	CLOCK_SGMII		= 25,
	CLOCK_REFCLKO		= 26,	/* 25 MHz clock output signal */
	CLOCK_INTC		= 4,
	CLOCK_UART0		= 16,
};

/*
 * The TC956X implements an "SFR" address space, which provides access
 * to *all* internal IP block registers, both MAC and non-MAC.  This
 * space is also accessible via an I2C interface used by the PCI pwrctl
 * driver (in "pci-pwrctrl-tc9563.c"), though that driver accesses the
 * range in a very limited way.  For the MAC functions we divide up the
 * range, providing specific addresses needed by the stmmac driver.
 */
#define MSIGEN_OFFSET(_mac_id)		((_mac_id) ? 0xf100 : 0xf000)

/*
 * struct tc956x_chip - Common information related to the TC956X chip
 * @dev:		Device structure for function 0
 * @sfr:		Mapped SFR regions (BAR 4, one per PCI function)
 * @reset_clock_regmap:	Regmap used for resets and clocks
 */
struct tc956x_chip {
	struct device *dev;
	void __iomem *sfr[2];
	struct regmap *reset_clock_regmap;
};

static const struct regmap_config reset_clock_regmap_config = {
	.name		= "tc956x-clk-reset",
	.reg_bits	= 32,
	.reg_stride	= 4,
	.reg_base	= 0x1000,	/* Register NCTLSTS */
	.val_bits	= 32,
	.max_register	= 0x1010,	/* Register NRSTCTRL1 */
};

/* Common clock/reset register update function (also used for MACs) */
static void tc956x_reset_clock_set(const struct tc956x_chip *chip, bool reset,
				   bool reg0, bool set, u8 bit)
{
	u32 mask = BIT(bit);
	u32 offset;

	if (reset)
		offset = reg0 ? RSTCTRL0_OFFSET : RSTCTRL1_OFFSET;
	else
		offset = reg0 ? CLKCTRL0_OFFSET : CLKCTRL1_OFFSET;

	/* Note: no need to check for errors on read/write for MMIO regmap */
	(void)regmap_update_bits(chip->reset_clock_regmap, offset, mask,
				 set ? mask : 0);
}

static void chip_reset_assert(const struct tc956x_chip *chip, enum reset_id id)
{
	tc956x_reset_clock_set(chip, true, true, true, (u8)id);
}

static void chip_reset_deassert(const struct tc956x_chip *chip,
				enum reset_id id)
{
	tc956x_reset_clock_set(chip, true, true, false, (u8)id);
}

static void chip_clock_enable(const struct tc956x_chip *chip, enum clock_id id)
{
	tc956x_reset_clock_set(chip, false, true, true, (u8)id);
}

static void chip_clock_disable(const struct tc956x_chip *chip,
			       enum clock_id id)
{
	tc956x_reset_clock_set(chip, false, true, false, (u8)id);
}

static void adev_release(struct device *dev)
{
	struct auxiliary_device *adev = to_auxiliary_dev(dev);

	of_node_put(adev->dev.of_node);
	kfree(adev);
}

static void adev_remove(void *data)
{
	struct auxiliary_device *adev = data;

	auxiliary_device_delete(adev);
	auxiliary_device_uninit(adev);
}

/* The of_node reference is always be dropped (success or not) */
static int adev_device_add(struct device *dev, const char *name, u32 id,
			   struct device_node *of_node, void *platform_data)
{
	struct auxiliary_device *adev;
	int ret;

	adev = kzalloc_obj(*adev);
	if (!adev) {
		of_node_put(of_node);
		return -ENOMEM;
	}

	adev->id = id;
	adev->name = name;
	adev->dev.parent = dev;
	adev->dev.platform_data = platform_data;
	adev->dev.release = adev_release;
	adev->dev.of_node = of_node;

	ret = auxiliary_device_init(adev);
	if (ret) {
		of_node_put(of_node);
		kfree(adev);
		return ret;
	}

	ret = auxiliary_device_add(adev);
	if (ret) {
		auxiliary_device_uninit(adev);
		return ret;
	}

	return devm_add_action_or_reset(dev, adev_remove, adev);
}

/* The two embedded XGMAC controllers have an auxiliary device driver */
static int function_xgmac_adev_add(struct pci_dev *pdev,
				   struct tc956x_chip *chip,
				   unsigned int msigen_irq)
{
	u8 mac_id = PCI_FUNC(pdev->devfn);
	struct device *dev = &pdev->dev;
	struct tc956x_dwmac_data *data;
	struct device_node *np;
	void __iomem *sfr;
	int ret;

	if (mac_id > 1)
		return -EINVAL;

	/* If there's no ethernet subnode, there's nothing to do */
	for_each_child_of_node(dev->of_node, np)
		if (!strcmp(np->name, "ethernet"))
			break;
	if (!np)
		return 0;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		of_node_put(np);
		return -ENOMEM;
	}

	sfr = chip->sfr[mac_id];

	data->msigen = sfr + MSIGEN_OFFSET(mac_id);
	data->msigen_irq = msigen_irq;

	ret = adev_device_add(dev, TC956X_XGMAC_DEV_NAME, mac_id, np, data);
	if (ret)
		return ret;

	return 0;
}

static int chip_reset_clock_init(struct tc956x_chip *chip)
{
	void __iomem *base = chip->sfr[0];
	struct device *dev = chip->dev;
	struct regmap *regmap;

	regmap = devm_regmap_init_mmio(dev, base, &reset_clock_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);
	chip->reset_clock_regmap = regmap;

	return 0;
}

static void chip_msigen_enable(struct tc956x_chip *chip)
{
	chip_clock_enable(chip, CLOCK_MSIGEN);
	chip_reset_deassert(chip, RESET_MSIGEN);
}

static void chip_msigen_disable(struct tc956x_chip *chip)
{
	chip_reset_assert(chip, RESET_MSIGEN);
	chip_clock_disable(chip, CLOCK_MSIGEN);
}

static void chip_init_state(struct tc956x_chip *chip)
{
	/* The only IP block we currently use is MSIGEN */
	chip_reset_assert(chip, RESET_MCU);
	chip_reset_assert(chip, RESET_MCU1);
	chip_reset_assert(chip, RESET_INTC);
	chip_reset_assert(chip, RESET_UART0);
	chip_clock_disable(chip, CLOCK_MCU);
	chip_clock_disable(chip, CLOCK_SRAM);
	chip_clock_disable(chip, CLOCK_PLL);
	chip_clock_disable(chip, CLOCK_SGMII);
	chip_clock_disable(chip, CLOCK_REFCLKO);
	chip_clock_disable(chip, CLOCK_INTC);
	chip_clock_disable(chip, CLOCK_UART0);

	/* Start with MSIGEN in reset with its clock disabled */
	chip_msigen_disable(chip);
}

static void chip_link_del(void *data)
{
	struct device_link *link = data;

	device_link_del(link);
}

/*
 * Function 0 will allocate the chip structure that is shared by both
 * functions.  Once it has allocated the structure it assigns it as
 * the PCI device platform data.  Function 1 can access the shared
 * chip structure by looking up the function 0 device to use its
 * platform data..
 *
 * Returns a chip structure pointer, or a pointer-coded error.
 */
static struct tc956x_chip *chip_get(struct pci_dev *pdev)
{
	unsigned int devfn = pdev->devfn;
	struct device *dev = &pdev->dev;
	struct tc956x_chip *chip;
	struct device_link *link;
	struct pci_dev *slot0;
	int ret;

	/* Function 0 just allocates the chip structure */
	if (!PCI_FUNC(devfn)) {
		chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
		if (!chip)
			return ERR_PTR(-ENOMEM);

		/*
		 * The function whose device pointer matches the chip's
		 * device pointer manages common resources (like MSIGEN).
		 */
		chip->dev = dev;

		return chip;
	}

	/* Function 1 has to get the chip structure from function 0 */
	slot0 = pci_get_slot(pdev->bus, PCI_DEVFN(PCI_SLOT(devfn), 0));
	if (!slot0)
		return ERR_PTR(-ENXIO);

	/* If function 0 hasn't set up the chip yet, try again later */
	chip = dev_get_platdata(&slot0->dev);
	if (!chip) {
		ret = -EPROBE_DEFER;
		goto err_put_slot;
	}

	/* Mark function 1's device as dependent on function 0 */
	link = device_link_add(dev, &slot0->dev, DL_FLAG_STATELESS);
	if (!link) {
		ret = -ENODEV;
		goto err_put_slot;
	}

	ret = devm_add_action_or_reset(dev, chip_link_del, link);
	if (ret)
		goto err_put_slot;

	return chip;

err_put_slot:
	pci_dev_put(slot0);

	return ERR_PTR(ret);
}

static int chip_init(struct tc956x_chip *chip, struct pci_dev *pdev)
{
	u32 id = PCI_FUNC(pdev->devfn) ? 1 : 0;
	int ret;

	/* Both chips need to map their SFR region */
	chip->sfr[id] = pcim_iomap_region(pdev, PCI_BAR_SFR, DRIVER_NAME);
	if (IS_ERR(chip->sfr[id]))
		return PTR_ERR(chip->sfr[id]);

	/* Function 0 handles common initialization */
	if (id)
		return 0;

	ret = chip_reset_clock_init(chip);
	if (ret)
		return ret;

	chip_init_state(chip);

	chip_msigen_enable(chip);

	return 0;
}

static void pcim_free_irq_vectors(void *data)
{
	struct pci_dev *pdev = data;

	pci_free_irq_vectors(pdev);
}

static int pcim_alloc_irq_vectors(struct pci_dev *pdev, unsigned int min_vecs,
				  unsigned int max_vecs, unsigned int flags)
{
	int ret;

	ret = pci_alloc_irq_vectors(pdev, min_vecs, max_vecs, flags);
	if (ret)
		return ret;

	return devm_add_action_or_reset(&pdev->dev, pcim_free_irq_vectors, pdev);
}

static int
tc956x_function_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct tc956x_chip *chip;
	unsigned int msigen_irq;
	int ret;

	if (PCI_FUNC(pdev->devfn)) {
		struct platform_device *tamap_pdev;
		struct device_node *tamap;

		tamap = of_parse_phandle(dev->of_node, "wait-for", 0);
		if (!tamap)
			return dev_err_probe(dev, -EINVAL,
					     "missing \"wait-for\" property\n");

		tamap_pdev = of_find_device_by_node(tamap);
		of_node_put(tamap);

		if (!tamap_pdev || !device_is_bound(&tamap_pdev->dev))
			return dev_err_probe(dev, -EPROBE_DEFER,
					     "waiting for address mapper\n");
	}

	/* Despite being a PCI device, we require devicetree */
	if (!dev->of_node)
		return dev_err_probe(dev, -EINVAL, "no devicetree node\n");

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	/* Function 1 gets -EPROBE_DEFER until function 0 sets platform data */
	chip = chip_get(pdev);
	if (IS_ERR(chip))
		return dev_err_probe(dev, PTR_ERR(chip), "failed to get chip\n");

	/* We called pcim_enable_device() so this will be freed automatically */
	ret = pcim_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (ret < 1)
		return dev_err_probe(dev, ret ? : -EIO,
				     "failed to allocate IRQ vectors\n");

	ret = pci_irq_vector(pdev, 0);
	if (ret < 1)
		return dev_err_probe(dev, ret ? : -EIO, "failed to get IRQ\n");
	msigen_irq = ret;

	ret = of_platform_default_populate(dev_of_node(dev), NULL, dev);
	if (ret)
		return dev_err_probe(dev, ret, "populating bus failed\n");

	ret = chip_init(chip, pdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize chip\n");

	ret = function_xgmac_adev_add(pdev, chip, msigen_irq);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add xgmap device\n");

	/* We're ready; the other function can now probe */
	dev->platform_data = chip;

	return 0;
}

static void tc956x_function_remove(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev;
	struct tc956x_chip *chip;

	chip = dev_get_platdata(dev);
	if (dev == chip->dev)
		chip_msigen_disable(chip);

	of_platform_depopulate(dev);

	pci_free_irq_vectors(pdev);

	pci_clear_master(pdev);
}

static const struct pci_device_id tc956x_function_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_TOSHIBA, PCI_DEVICE_ID_TOSHIBA_TC956X), },
	{ },
};
MODULE_DEVICE_TABLE(pci, tc956x_function_id_table);

static int tc956x_chip_suspend_noirq(struct device *dev)
{
	struct tc956x_chip *chip = dev_get_platdata(dev);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (dev == chip->dev)
		chip_msigen_disable(chip);

	/* It seems most callers ignore the return value here */
	pci_save_state(pdev);
	pci_wake_from_d3(pdev, true);

	return 0;
}

static int tc956x_chip_resume_noirq(struct device *dev)
{
	struct tc956x_chip *chip = dev_get_platdata(dev);
	struct pci_dev *pdev = to_pci_dev(dev);

	pci_wake_from_d3(pdev, false);
	pci_set_power_state(pdev, PCI_D0);
	pci_restore_state(pdev);

	if (dev != chip->dev)
		return 0;

	chip_msigen_enable(chip);

	return 0;
}

static DEFINE_NOIRQ_DEV_PM_OPS(tc956x_chip_pm_ops,
			       tc956x_chip_suspend_noirq,
			       tc956x_chip_resume_noirq);

static struct pci_driver tc956x_function_driver = {
	.name		= DRIVER_NAME,
	.id_table	= tc956x_function_id_table,
	.probe		= tc956x_function_probe,
	.remove		= tc956x_function_remove,
	.driver		= {
		.name		= DRIVER_NAME,
		.owner		= THIS_MODULE,
		.pm		= pm_sleep_ptr(&tc956x_chip_pm_ops),
	},
};

module_pci_driver(tc956x_function_driver);

MODULE_DESCRIPTION("Toshiba TC956X PCIe Embedded Function Driver");
MODULE_LICENSE("GPL");
