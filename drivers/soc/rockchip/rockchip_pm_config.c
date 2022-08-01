/*
 * Rockchip Generic power configuration support.
 *
 * Copyright (c) 2017 ROCKCHIP, Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/machine.h>
#include <linux/rockchip/rockchip_sip.h>
#include <linux/suspend.h>
#include <dt-bindings/input/input.h>
#include <linux/pm.h>

#define PM_INVALID_GPIO	0xffff

static u32 pm_suspend_mem, pm_suspend_ultra, pm_suspend_regulator_ultra;

static const struct of_device_id pm_match_table[] = {
	{ .compatible = "rockchip,pm-px30",},
	{ .compatible = "rockchip,pm-rk322x",},
	{ .compatible = "rockchip,pm-rk3288",},
	{ .compatible = "rockchip,pm-rk3308",},
	{ .compatible = "rockchip,pm-rk3328",},
	{ .compatible = "rockchip,pm-rk3368",},
	{ .compatible = "rockchip,pm-rk3399",},
	{ },
};

static void rockchip_pm_virt_pwroff_prepare(void)
{
	int error;

	regulator_suspend_prepare(PM_SUSPEND_MEM);

	error = disable_nonboot_cpus();
	if (error) {
		pr_err("Disable nonboot cpus failed!\n");
		return;
	}

	// drivers/firmware/rockchip_sip.c
	sip_smc_set_suspend_mode(VIRTUAL_POWEROFF, 0, 1);
	sip_smc_virtual_poweroff();
}

//extern int __init vendor_storage_init(void);
static int __init pm_config_probe(struct platform_device *pdev)
{
	const struct of_device_id *match_id;
	struct device_node *node;
	u32 mode_config = 0;
	u32 wakeup_config = 0;
	u32 pwm_regulator_config = 0;
	int gpio_temp[10];
	u32 sleep_debug_en = 0;
	u32 apios_suspend = 0;
	u32 virtual_poweroff_en = 0;
	enum of_gpio_flags flags;
	int i = 0;
	int length;

	match_id = of_match_node(pm_match_table, pdev->dev.of_node);
	if (!match_id)
		return -ENODEV;

	node = of_find_node_by_name(NULL, "rockchip-suspend");

	if (IS_ERR_OR_NULL(node)) {
		dev_err(&pdev->dev, "%s dev node err\n",  __func__);
		return -ENODEV;
	}

	if (of_property_read_u32_array(node,
				       "rockchip,sleep-mode-config",
				       &mode_config, 1)) {
		dev_warn(&pdev->dev, "not set sleep mode config\n");
	} else {		
		pm_suspend_mem = mode_config;
		sip_smc_set_suspend_mode(SUSPEND_MODE_CONFIG, mode_config, 0);
	}

	if (!of_property_read_u32_array(node,
				       "rockchip,ultra-sleep-mode-config",
				       &mode_config, 1))
		pm_suspend_ultra = mode_config;

	if (of_property_read_u32_array(node,
				       "rockchip,wakeup-config",
				       &wakeup_config, 1))
		dev_warn(&pdev->dev, "not set wakeup-config\n");
	else
		sip_smc_set_suspend_mode(WKUP_SOURCE_CONFIG, wakeup_config, 0);

	if (of_property_read_u32_array(node,
				       "rockchip,pwm-regulator-config",
				       &pwm_regulator_config, 1))
		dev_warn(&pdev->dev, "not set pwm-regulator-config\n");
	else {
		pm_suspend_regulator_ultra = pwm_regulator_config;
	}

	length = of_gpio_named_count(node, "rockchip,power-ctrl");

	if (length > 0 && length < 10) {
		for (i = 0; i < length; i++) {
			gpio_temp[i] = of_get_named_gpio_flags(node,
							     "rockchip,power-ctrl",
							     i,
							     &flags);
			if (!gpio_is_valid(gpio_temp[i]))
				break;
			sip_smc_set_suspend_mode(GPIO_POWER_CONFIG,
						 i,
						 gpio_temp[i]);
		}
	}
	sip_smc_set_suspend_mode(GPIO_POWER_CONFIG, i, PM_INVALID_GPIO);

	if (!of_property_read_u32_array(node,
					"rockchip,sleep-debug-en",
					&sleep_debug_en, 1))
		sip_smc_set_suspend_mode(SUSPEND_DEBUG_ENABLE,
					 sleep_debug_en,
					 0);

	if (!of_property_read_u32_array(node,
					"rockchip,apios-suspend",
					&apios_suspend, 1))
		sip_smc_set_suspend_mode(APIOS_SUSPEND_CONFIG,
					 apios_suspend,
					 0);

	if (!of_property_read_u32_array(node,
					"rockchip,virtual-poweroff",
					&virtual_poweroff_en, 1) &&
	    virtual_poweroff_en)
		pm_power_off_prepare = rockchip_pm_virt_pwroff_prepare;

	//if( pm_suspend_ultra && pm_suspend_regulator_ultra) vendor_storage_init();
	return 0;
}

//extern suspend_state_t get_suspend_state(void);
extern bool fb_power_off(void);
static int pm_config_suspend(struct platform_device *dev,
				 pm_message_t state)
{

#if 0
	suspend_state_t suspend_state;

	suspend_state = get_suspend_state();
	if (suspend_state == PM_SUSPEND_ULTRA && pm_suspend_ultra && pm_suspend_regulator_ultra) {
		sip_smc_set_suspend_mode(SUSPEND_MODE_CONFIG,
					 pm_suspend_ultra,
					 0);
		sip_smc_set_suspend_mode(PWM_REGULATOR_CONFIG,
					 pm_suspend_regulator_ultra,
					 0);
	}
	else {
                sip_smc_set_suspend_mode(SUSPEND_MODE_CONFIG,
                                         pm_suspend_mem,
                                         0);
	}
#else 
	bool is_ultra = false;
	if(fb_power_off()) {
		if(pm_suspend_ultra && pm_suspend_regulator_ultra) {
			sip_smc_set_suspend_mode(SUSPEND_MODE_CONFIG,
					 pm_suspend_ultra,
					 0);
			sip_smc_set_suspend_mode(PWM_REGULATOR_CONFIG,
					 pm_suspend_regulator_ultra,
					 0);
			is_ultra = true;
		}
	}
	if(!is_ultra){
		sip_smc_set_suspend_mode(SUSPEND_MODE_CONFIG,
                     pm_suspend_mem,
                     0);
	}
#endif 
	return 0;
}

static struct platform_driver pm_driver = {
	.probe = pm_config_probe,
	.suspend = pm_config_suspend,
	.driver = {
		.name = "rockchip-pm",
		.of_match_table = pm_match_table,
	},
};

static int __init rockchip_pm_drv_register(void)
{
	return platform_driver_register(&pm_driver);
}
subsys_initcall(rockchip_pm_drv_register);
