/*
 * Generic platform ehci driver
 *
 * Copyright 2007 Steven Brown <sbrown@cortland.com>
 * Copyright 2010-2012 Hauke Mehrtens <hauke@hauke-m.de>
 * Copyright 2014 Hans de Goede <hdegoede@redhat.com>
 *
 * Derived from the ohci-ssb driver
 * Copyright 2007 Michael Buesch <m@bues.ch>
 *
 * Derived from the EHCI-PCI driver
 * Copyright (c) 2000-2004 by David Brownell
 *
 * Derived from the ohci-pci driver
 * Copyright 1999 Roman Weissgaerber
 * Copyright 2000-2002 David Brownell
 * Copyright 1999 Linus Torvalds
 * Copyright 1999 Gregory P. Smith
 *
 * Licensed under the GNU/GPL. See COPYING for details.
 */
#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/usb/ehci_pdriver.h>

#include "ehci.h"

#define DRIVER_DESC "EHCI generic platform driver"
#define EHCI_MAX_CLKS 3
#define hcd_to_ehci_priv(h) ((struct ehci_platform_priv *)hcd_to_ehci(h)->priv)

struct ehci_platform_priv {
	struct clk *clks[EHCI_MAX_CLKS];
	struct reset_control *rst;
	struct phy **phys;
	int num_phys;
	bool reset_on_resume;
};

static const char hcd_name[] = "ehci-platform";
static struct device *gusbpdata;

static void ehci_hcd_restart(struct usb_hcd *hcd)
{
	int irq, retval;

	if (!hcd || hcd->irq < 0) {
		dev_err(hcd->self.controller, "%s: hcd is null\n", __func__);
	}

	dev_info(hcd->self.controller,
		 "%s: enter restart ehci hcd process.\n", __func__);

	/*
	 * note: record irq.
	 * Because after usb_remove_hcd, hcd->irq is cleared,
	 * resulting in failure to register ehci interrupt.
	 */
	irq = hcd->irq;

	/* remove usb hcd */
	usb_remove_hcd(hcd);

	/* register usb hcd */
	retval = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (retval)
		dev_err(hcd->self.controller,
			"%s: register usb hcd fail(%d).\n", __func__, retval);
}

static ssize_t ehci_hcd_show(struct device *device,
			     struct device_attribute *attr,
			     char *buf)
{
	struct usb_hcd *hcd = dev_get_drvdata(device);
	struct ehci_hcd *ehci = hcd_to_ehci (hcd);

	if (ehci->rh_state == EHCI_RH_RUNNING)
		return sprintf(buf, "Current is running state\n");
	else if (ehci->rh_state == EHCI_RH_SUSPENDED)
		return sprintf(buf, "Current is suspend state\n");
	else if (ehci->rh_state == EHCI_RH_STOPPING)
		return sprintf(buf, "Current is stopping state\n");
	else
		return sprintf(buf, "Current is halt state\n");

}

static ssize_t ehci_hcd_store(struct device *device,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct usb_hcd *hcd = dev_get_drvdata(device);
	int rc = count;

	if ((!strncmp(buf, "1", 1)) || (!strncmp(buf, "restart", 7))) {
		/* restart hcd */
		ehci_hcd_restart(hcd);
	} else {
		dev_err(device, "Error input! Input '1' or 'restart'\n");
		rc = -EINVAL;
	}

	return rc;
}

static DEVICE_ATTR_RW(ehci_hcd);

static struct attribute *ehci_hcd_ctrl_attrs[] = {
	&dev_attr_ehci_hcd.attr,
	NULL,
};

static struct attribute_group ehci_hcd_ctrl_group = {
	.name = NULL,
	.attrs = ehci_hcd_ctrl_attrs,
};


// 20210807: add for meige 4G-CAT1,reset host.
int ehci_reset_ehci_host(void)
{
	struct usb_hcd *hcd;
	if(gusbpdata == NULL) return -EINVAL;

	hcd = dev_get_drvdata(gusbpdata);
	if(hcd == NULL) return -ENOENT;
	
	printk("%s: hcd=%p\n", __func__, hcd);
	ehci_hcd_restart(hcd);
	return 0;
}

static void ehci_rockchip_relinquish_port(struct usb_hcd *hcd, int portnum)
{
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);
	u32 __iomem *status_reg = &ehci->regs->port_status[--portnum];
	u32 portsc;

	portsc = ehci_readl(ehci, status_reg);
	portsc &= ~(PORT_OWNER | PORT_RWC_BITS);

	ehci_writel(ehci, portsc, status_reg);
}

static int ehci_platform_reset(struct usb_hcd *hcd)
{
	struct platform_device *pdev = to_platform_device(hcd->self.controller);
	struct usb_ehci_pdata *pdata = dev_get_platdata(&pdev->dev);
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);
	int retval;

	ehci->has_synopsys_hc_bug = pdata->has_synopsys_hc_bug;

	if (pdata->pre_setup) {
		retval = pdata->pre_setup(hcd);
		if (retval < 0)
			return retval;
	}

	ehci->caps = hcd->regs + pdata->caps_offset;
	retval = ehci_setup(hcd);
	if (retval)
		return retval;

	if (pdata->no_io_watchdog)
		ehci->need_io_watchdog = 0;
	return 0;
}

static int ehci_platform_power_on(struct platform_device *dev)
{
	struct usb_hcd *hcd = platform_get_drvdata(dev);
	struct ehci_platform_priv *priv = hcd_to_ehci_priv(hcd);
	int clk, ret, phy_num;

	for (clk = 0; clk < EHCI_MAX_CLKS && priv->clks[clk]; clk++) {
		ret = clk_prepare_enable(priv->clks[clk]);
		if (ret)
			goto err_disable_clks;
	}

	for (phy_num = 0; phy_num < priv->num_phys; phy_num++) {
		ret = phy_init(priv->phys[phy_num]);
		if (ret)
			goto err_exit_phy;
		ret = phy_power_on(priv->phys[phy_num]);
		if (ret) {
			phy_exit(priv->phys[phy_num]);
			goto err_exit_phy;
		}
	}

	return 0;

err_exit_phy:
	while (--phy_num >= 0) {
		phy_power_off(priv->phys[phy_num]);
		phy_exit(priv->phys[phy_num]);
	}
err_disable_clks:
	while (--clk >= 0)
		clk_disable_unprepare(priv->clks[clk]);

	return ret;
}

static void ehci_platform_power_off(struct platform_device *dev)
{
	struct usb_hcd *hcd = platform_get_drvdata(dev);
	struct ehci_platform_priv *priv = hcd_to_ehci_priv(hcd);
	int clk, phy_num;

	for (phy_num = 0; phy_num < priv->num_phys; phy_num++) {
		phy_power_off(priv->phys[phy_num]);
		phy_exit(priv->phys[phy_num]);
	}

	for (clk = EHCI_MAX_CLKS - 1; clk >= 0; clk--)
		if (priv->clks[clk])
			clk_disable_unprepare(priv->clks[clk]);
}

static struct hc_driver __read_mostly ehci_platform_hc_driver;

static const struct ehci_driver_overrides platform_overrides __initconst = {
	.reset =		ehci_platform_reset,
	.extra_priv_size =	sizeof(struct ehci_platform_priv),
};

static struct usb_ehci_pdata ehci_platform_defaults = {
	.power_on =		ehci_platform_power_on,
	.power_suspend =	ehci_platform_power_off,
	.power_off =		ehci_platform_power_off,
};

static ssize_t ehci_wakeup_source_show(struct device *device,
			          struct device_attribute *attr,
			          char *buf)
{
	bool is_wakeup_source = device_may_wakeup(device);

	if (is_wakeup_source)
		return sprintf(buf, "%s device is wakeup source\n", dev_name(device));
	else
		return sprintf(buf, "%s device is no wakeup source\n", dev_name(device));

	return -EINVAL;
}

static ssize_t ehci_wakeup_source_store(struct device *device,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	bool is_wakeup_source = device_may_wakeup(device);
	int rc = count;

	if ((!strncmp(buf, "1", 1)) || (!strncmp(buf, "enable-wakeup-source", 20))) {
		if (is_wakeup_source)
			dev_warn(device, "no need set, because it has wakeup source\n");
		else
			device_init_wakeup(device, true);
	} else if ((!strncmp(buf, "0", 1)) || (!strncmp(buf, "disable-wakeup-source", 21))) {
		if (is_wakeup_source)
			device_init_wakeup(device, false);
	} else {
		dev_err(device, "Error input! Input '1' or '0'\n");
		rc = -EINVAL;
	}

	return rc;
}

static DEVICE_ATTR_RW(ehci_wakeup_source);

static struct attribute *ehci_wakeup_source_ctrl_attrs[] = {
	&dev_attr_ehci_wakeup_source.attr,
	NULL,
};

static struct attribute_group ehci_wakeup_source_ctrl_group = {
	.name = NULL,
	.attrs = ehci_wakeup_source_ctrl_attrs,
};

// 20210807: add for meige 4G-CAT1.
int ehci_wakeup_ctrl(bool wakeup)
{
	bool is_wakeup_source = device_may_wakeup(gusbpdata);

	if(is_wakeup_source == wakeup)
		return 0;
	device_init_wakeup(gusbpdata, wakeup);
	printk(" 4g ehci_wakeup_ctrl:%d\n",wakeup);
	return 0;
}

static int ehci_platform_probe(struct platform_device *dev)
{
	struct usb_hcd *hcd;
	struct resource *res_mem;
	struct usb_ehci_pdata *pdata = dev_get_platdata(&dev->dev);
	struct ehci_platform_priv *priv;
	struct ehci_hcd *ehci;
	int err, irq, phy_num, clk = 0;
	struct device *mdev = &dev->dev;

	if (usb_disabled())
		return -ENODEV;

	/*
	 * Use reasonable defaults so platforms don't have to provide these
	 * with DT probing on ARM.
	 */
	if (!pdata)
		pdata = &ehci_platform_defaults;

	err = dma_coerce_mask_and_coherent(&dev->dev,
		pdata->dma_mask_64 ? DMA_BIT_MASK(64) : DMA_BIT_MASK(32));
	if (err) {
		dev_err(&dev->dev, "Error: DMA mask configuration failed\n");
		return err;
	}

	irq = platform_get_irq(dev, 0);
	if (irq < 0) {
		dev_err(&dev->dev, "no irq provided");
		return irq;
	}

	hcd = usb_create_hcd(&ehci_platform_hc_driver, &dev->dev,
			     dev_name(&dev->dev));
	if (!hcd)
		return -ENOMEM;

	platform_set_drvdata(dev, hcd);
	dev->dev.platform_data = pdata;
	priv = hcd_to_ehci_priv(hcd);
	ehci = hcd_to_ehci(hcd);

	if (pdata == &ehci_platform_defaults && dev->dev.of_node) {
		if (of_property_read_bool(dev->dev.of_node, "big-endian-regs"))
			ehci->big_endian_mmio = 1;

		if (of_property_read_bool(dev->dev.of_node, "big-endian-desc"))
			ehci->big_endian_desc = 1;

		if (of_property_read_bool(dev->dev.of_node, "big-endian"))
			ehci->big_endian_mmio = ehci->big_endian_desc = 1;

		if (of_property_read_bool(dev->dev.of_node,
					  "needs-reset-on-resume"))
			priv->reset_on_resume = true;

		if (of_property_read_bool(dev->dev.of_node,
					  "has-transaction-translator"))
			hcd->has_tt = 1;

		if (of_machine_is_compatible("rockchip,rk3288") &&
		    of_property_read_bool(dev->dev.of_node,
					  "rockchip-relinquish-port")) {
			ehci_platform_hc_driver.relinquish_port =
					  ehci_rockchip_relinquish_port;
			hcd->rk3288_relinquish_port_quirk = 1;
		}

		priv->num_phys = of_count_phandle_with_args(dev->dev.of_node,
				"phys", "#phy-cells");

		if (priv->num_phys > 0) {
			priv->phys = devm_kcalloc(&dev->dev, priv->num_phys,
					    sizeof(struct phy *), GFP_KERNEL);
			if (!priv->phys)
				return -ENOMEM;
		} else
			priv->num_phys = 0;

		for (phy_num = 0; phy_num < priv->num_phys; phy_num++) {
			priv->phys[phy_num] = devm_of_phy_get_by_index(
					&dev->dev, dev->dev.of_node, phy_num);
			if (IS_ERR(priv->phys[phy_num])) {
				err = PTR_ERR(priv->phys[phy_num]);
					goto err_put_hcd;
			}
		}

		for (clk = 0; clk < EHCI_MAX_CLKS; clk++) {
			priv->clks[clk] = of_clk_get(dev->dev.of_node, clk);
			if (IS_ERR(priv->clks[clk])) {
				err = PTR_ERR(priv->clks[clk]);
				if (err == -EPROBE_DEFER)
					goto err_put_clks;
				priv->clks[clk] = NULL;
				break;
			}
		}
	}

	priv->rst = devm_reset_control_get_optional(&dev->dev, NULL);
	if (IS_ERR(priv->rst)) {
		err = PTR_ERR(priv->rst);
		if (err == -EPROBE_DEFER)
			goto err_put_clks;
		priv->rst = NULL;
	} else {
		err = reset_control_deassert(priv->rst);
		if (err)
			goto err_put_clks;
	}

	if (pdata->big_endian_desc)
		ehci->big_endian_desc = 1;
	if (pdata->big_endian_mmio)
		ehci->big_endian_mmio = 1;
	if (pdata->has_tt)
		hcd->has_tt = 1;
	if (pdata->reset_on_resume)
		priv->reset_on_resume = true;

#ifndef CONFIG_USB_EHCI_BIG_ENDIAN_MMIO
	if (ehci->big_endian_mmio) {
		dev_err(&dev->dev,
			"Error: CONFIG_USB_EHCI_BIG_ENDIAN_MMIO not set\n");
		err = -EINVAL;
		goto err_reset;
	}
#endif
#ifndef CONFIG_USB_EHCI_BIG_ENDIAN_DESC
	if (ehci->big_endian_desc) {
		dev_err(&dev->dev,
			"Error: CONFIG_USB_EHCI_BIG_ENDIAN_DESC not set\n");
		err = -EINVAL;
		goto err_reset;
	}
#endif

	if (pdata->power_on) {
		err = pdata->power_on(dev);
		if (err < 0)
			goto err_reset;
	}

	res_mem = platform_get_resource(dev, IORESOURCE_MEM, 0);
	hcd->regs = devm_ioremap_resource(&dev->dev, res_mem);
	if (IS_ERR(hcd->regs)) {
		err = PTR_ERR(hcd->regs);
		goto err_power;
	}
	hcd->rsrc_start = res_mem->start;
	hcd->rsrc_len = resource_size(res_mem);
	if (priv->num_phys == 1)
		hcd->phy = priv->phys[0];

	err = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (err)
		goto err_power;

	device_wakeup_enable(hcd->self.controller);
    #ifdef CONFIG_LTE
	device_init_wakeup(&dev->dev, true); //210616 patch from RK. for usb not suspend
    #endif
	platform_set_drvdata(dev, hcd);
	/* Attributes */
	err = sysfs_create_group(&dev->dev.kobj, &ehci_wakeup_source_ctrl_group);
	if (err) {
		dev_err(&dev->dev, "Cannot create sysfs group: %d\n", err);
		goto err_power;
	}
	gusbpdata = mdev;

	/* Attributes */
	err = sysfs_create_group(&dev->dev.kobj, &ehci_hcd_ctrl_group);
	if (err) {
		dev_err(&dev->dev, "Cannot create sysfs group: %d\n", err);
		goto err_power;
	}

	return err;

err_power:
	if (pdata->power_off)
		pdata->power_off(dev);
err_reset:
	if (priv->rst)
		reset_control_assert(priv->rst);
err_put_clks:
	while (--clk >= 0)
		clk_put(priv->clks[clk]);
err_put_hcd:
	if (pdata == &ehci_platform_defaults)
		dev->dev.platform_data = NULL;

	usb_put_hcd(hcd);

	return err;
}

static int ehci_platform_remove(struct platform_device *dev)
{
	struct usb_hcd *hcd = platform_get_drvdata(dev);
	struct usb_ehci_pdata *pdata = dev_get_platdata(&dev->dev);
	struct ehci_platform_priv *priv = hcd_to_ehci_priv(hcd);
	int clk;

	usb_remove_hcd(hcd);

	if (pdata->power_off)
		pdata->power_off(dev);

	if (priv->rst)
		reset_control_assert(priv->rst);

	for (clk = 0; clk < EHCI_MAX_CLKS && priv->clks[clk]; clk++)
		clk_put(priv->clks[clk]);

	usb_put_hcd(hcd);

	if (pdata == &ehci_platform_defaults)
		dev->dev.platform_data = NULL;

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int ehci_platform_suspend(struct device *dev)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct usb_ehci_pdata *pdata = dev_get_platdata(dev);
	struct platform_device *pdev =
		container_of(dev, struct platform_device, dev);
	bool do_wakeup = device_may_wakeup(dev);
	int ret;

	ret = ehci_suspend(hcd, do_wakeup);
	if (ret)
		return ret;

	if (pdata->power_suspend)
		pdata->power_suspend(pdev);

	return ret;
}

static int ehci_platform_resume(struct device *dev)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	struct usb_ehci_pdata *pdata = dev_get_platdata(dev);
	struct platform_device *pdev =
		container_of(dev, struct platform_device, dev);
	struct ehci_platform_priv *priv = hcd_to_ehci_priv(hcd);

	if (pdata->power_on) {
		int err = pdata->power_on(pdev);
		if (err < 0)
			return err;
	}

	ehci_resume(hcd, priv->reset_on_resume);
	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static const struct of_device_id vt8500_ehci_ids[] = {
	{ .compatible = "via,vt8500-ehci", },
	{ .compatible = "wm,prizm-ehci", },
	{ .compatible = "generic-ehci", },
	{ .compatible = "cavium,octeon-6335-ehci", },
	{}
};
MODULE_DEVICE_TABLE(of, vt8500_ehci_ids);

static const struct acpi_device_id ehci_acpi_match[] = {
	{ "PNP0D20", 0 }, /* EHCI controller without debug */
	{ }
};
MODULE_DEVICE_TABLE(acpi, ehci_acpi_match);

static const struct platform_device_id ehci_platform_table[] = {
	{ "ehci-platform", 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, ehci_platform_table);

static SIMPLE_DEV_PM_OPS(ehci_platform_pm_ops, ehci_platform_suspend,
	ehci_platform_resume);

static struct platform_driver ehci_platform_driver = {
	.id_table	= ehci_platform_table,
	.probe		= ehci_platform_probe,
	.remove		= ehci_platform_remove,
	.shutdown	= usb_hcd_platform_shutdown,
	.driver		= {
		.name	= "ehci-platform",
		//.pm	= &ehci_platform_pm_ops,
		.of_match_table = vt8500_ehci_ids,
		.acpi_match_table = ACPI_PTR(ehci_acpi_match),
	}
};

static int __init ehci_platform_init(void)
{
	if (usb_disabled())
		return -ENODEV;

	pr_info("%s: " DRIVER_DESC "\n", hcd_name);

	ehci_init_driver(&ehci_platform_hc_driver, &platform_overrides);
	return platform_driver_register(&ehci_platform_driver);
}
module_init(ehci_platform_init);

static void __exit ehci_platform_cleanup(void)
{
	platform_driver_unregister(&ehci_platform_driver);
}
module_exit(ehci_platform_cleanup);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("Hauke Mehrtens");
MODULE_AUTHOR("Alan Stern");
MODULE_LICENSE("GPL");
