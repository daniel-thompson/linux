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
 * This PCI driver binds to the Toshiba TC956X (physical) PCI function
 * (VID 0x1179, DID 0x0220).  There are two of these present on the
 * TC956X SoC.
 */

#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pci.h>

#include <soc/toshiba/tc956x-dwmac.h>

#define DRIVER_NAME			TC956X_PCIE_DRIVER_NAME "2"

#define PCI_DEVICE_ID_TOSHIBA_TC956X	0x0220

/**
 * struct tc956x_func - PCIe function device information
 * @dev:	Device pointer
 * @ovcs_id:	Devicetree overlay changeset ID
 */
struct tc956x_func {
	struct device *dev;
	int ovcs_id;
};

/* These are defined tc956x_pci.dtbo.S, created by the build process */
extern const char __dtbo_tc956x_pci_p1b5d0f0_begin[];
extern const char __dtbo_tc956x_pci_p1b5d0f0_end[];
extern const char __dtbo_tc956x_pci_p1b5d0f1_begin[];
extern const char __dtbo_tc956x_pci_p1b5d0f1_end[];

static int
tc956x_load_overlay(unsigned int fn, struct device_node *np, int *ovcs_id)
{
	const char *begin = fn ? __dtbo_tc956x_pci_p1b5d0f1_begin
			       : __dtbo_tc956x_pci_p1b5d0f0_begin;
	const char *end = fn ? __dtbo_tc956x_pci_p1b5d0f1_end
			     : __dtbo_tc956x_pci_p1b5d0f0_end;

	return of_overlay_fdt_apply(begin, end - begin, ovcs_id, np);
}

static void tc956x_unload_overlay(int *ovcs_id)
{
	of_overlay_remove(ovcs_id);
}

static int
tc956x_function_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	unsigned int fn = PCI_FUNC(pdev->devfn);
	struct device *dev = &pdev->dev;
	struct tc956x_func *func;
	struct device_node *np;
	int ret;

	dev_info(dev, " === %s: starting\n", __func__);

	if (fn > 1)
		return dev_err_probe(dev, -EINVAL, "bad function %u\n", fn);

	/* Despite being a PCI device, we require devicetree */
	np = dev_of_node(dev);
	if (!np)
		return dev_err_probe(dev, -EINVAL, "no devicetree node\n");

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	func = devm_kzalloc(dev, sizeof(*func), GFP_KERNEL);
	if (!func)
		return -ENOMEM;
	func->dev = dev;

	ret = tc956x_load_overlay(fn, np, &func->ovcs_id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to load overlay\n");

	dev_info(dev, " === calling of_platform_default_populate()\n");
	ret = of_platform_default_populate(np, NULL, dev);
	if (ret)
		goto err_unload_overlay;

	pci_set_drvdata(pdev, func);

	dev_info(dev, " === %s: successful\n", __func__);

	return 0;

err_unload_overlay:
	tc956x_unload_overlay(&func->ovcs_id);

	return dev_err_probe(dev, ret, "failed to populate bus\n");
}

static void tc956x_function_remove(struct pci_dev *pdev)
{
	struct tc956x_func *func = pci_get_drvdata(pdev);

	of_platform_depopulate(&pdev->dev);

	if (func)
		tc956x_unload_overlay(&func->ovcs_id);

	pci_clear_master(pdev);
}

static const struct pci_device_id tc956x_function_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_TOSHIBA, PCI_DEVICE_ID_TOSHIBA_TC956X), },
	{ },
};
MODULE_DEVICE_TABLE(pci, tc956x_function_id_table);

static struct pci_driver tc956x_function_driver = {
	.name		= DRIVER_NAME,
	.id_table	= tc956x_function_id_table,
	.probe		= tc956x_function_probe,
	.remove		= tc956x_function_remove,
	.driver		= {
		.name		= DRIVER_NAME,
		.owner		= THIS_MODULE,
	},
};

module_pci_driver(tc956x_function_driver);

MODULE_DESCRIPTION("Toshiba TC956X PCIe Embedded Function Driver");
MODULE_LICENSE("GPL");
