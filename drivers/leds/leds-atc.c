/*
 * leds-atc - Simple driver to control ATC led on a Qualcomm PMIC
 * Copyright (c) 2015-2019, MMI. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/leds.h>
#include <linux/err.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/spmi.h>
#include <linux/platform_device.h>

#define LED_CFG_MASK	0x06
#define LED_CFG_SHIFT   1
#define LED_ON		0x03
#define LED_OFF		0x00
#define LED_BLINK_1	0x01
#define LED_BLINK_2	0x02

/**
 * @led_classdev - led class device
 * @platform_device - platform class device
 * @addr - spmi address for control register
 */
struct atc_led_data {
	struct led_classdev	cdev;
	struct platform_device *pdev;
	struct regmap *regmap;
	u32			addr;
	u8			blink;
};

static int
atc_led_masked_write(struct atc_led_data *led, u16 addr, u8 mask, u8 val)
{
	int rc;

	rc = regmap_update_bits(led->regmap, addr, mask, val);
	if (rc)
		dev_err(&led->pdev->dev,
			"Unable to regmap_update_bits to addr=%x, rc(%d)\n",
			addr, rc);
	return rc;
}

static void atc_led_write(struct atc_led_data *led, u8 cfg)
{
	atc_led_masked_write(led, led->addr, LED_CFG_MASK,
			     cfg << LED_CFG_SHIFT);
}

/*
 * The ATC pin is either off, on, or blinking in one of two PMIC patterns.
 * Any non-zero brightness means on, except that it leaves a running
 * hardware blink alone (the timer trigger sets the blink first, then
 * the brightness).
 */
static void atc_led_set(struct led_classdev *led_cdev,
				enum led_brightness value)
{
	struct atc_led_data *led;

	led = container_of(led_cdev, struct atc_led_data, cdev);

	if (value == LED_OFF)
		led->blink = 0;
	else if (led->blink)
		return;

	atc_led_write(led, value ? LED_ON : LED_OFF);
}

/* Blink in hardware, so it keeps blinking while the system is suspended */
static int atc_led_blink_set(struct led_classdev *led_cdev,
			     unsigned long *delay_on, unsigned long *delay_off)
{
	struct atc_led_data *led;

	led = container_of(led_cdev, struct atc_led_data, cdev);

	if (!*delay_on && !*delay_off) {
		led->blink = LED_BLINK_1;
	} else if (!*delay_off) {
		led->blink = 0;
		atc_led_write(led, LED_ON);
		return 0;
	} else if (!*delay_on) {
		led->blink = 0;
		atc_led_write(led, LED_OFF);
		return 0;
	} else {
		led->blink = *delay_on < 500 ? LED_BLINK_2 : LED_BLINK_1;
	}

	atc_led_write(led, led->blink);
	led_cdev->brightness = led_cdev->max_brightness;
	return 0;
}

static enum led_brightness atc_led_get(struct led_classdev *led_cdev)
{
	struct atc_led_data *led;
	unsigned int val;

	led = container_of(led_cdev, struct atc_led_data, cdev);
	if (regmap_read(led->regmap, led->addr, &val))
		return led_cdev->brightness;

	return (val & LED_CFG_MASK) ? led_cdev->max_brightness : LED_OFF;
}

static int atc_leds_probe(struct platform_device *pdev)
{
	struct atc_led_data *led;
	struct device_node *node;
	unsigned int base;
	u32 offset;
	int rc;
	uint val_reg;

	node = pdev->dev.of_node;
	if (node == NULL) {
		dev_err(&pdev->dev, "No atc led defined\n");
		return -ENODEV;
	}

	rc = of_property_read_u32(node, "reg", &base);
	if (rc < 0) {
		dev_err(&pdev->dev,
			"Couldn't find reg in node = %s rc = %d\n",
			pdev->dev.of_node->full_name, rc);
		return rc;
	}

	rc = of_property_read_u32(node, "qcom,ctrl-reg", &offset);
	if (rc < 0) {
		dev_err(&pdev->dev,
			"Failure reading ctrl-reg, rc = %d\n", rc);
		return rc;
	}

	led = devm_kzalloc(&pdev->dev, sizeof(struct atc_led_data), GFP_KERNEL);
	if (!led) {
		dev_err(&pdev->dev, "Unable to allocate memory\n");
		return -ENOMEM;
	}

	led->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!led->regmap) {
		dev_err(&pdev->dev, "Couldn't get parent's regmap\n");
		return -EINVAL;
	}

	led->pdev = pdev;
	led->addr = base + offset;

	rc = of_property_read_string(node, "linux,name", &led->cdev.name);
	if (rc < 0) {
		dev_err(&pdev->dev,
			"Failure reading led name, rc = %d\n", rc);
		return -ENODEV;
	}


	rc = regmap_read(led->regmap, led->addr, &val_reg);
	if (rc < 0) {
		dev_err(&pdev->dev,
			"Error reading address: %x(%d)\n", led->addr, rc);
		return rc;
	}
	
	led->cdev.brightness_set = atc_led_set;
	led->cdev.brightness_get = atc_led_get;
	led->cdev.blink_set = atc_led_blink_set;
	led->cdev.max_brightness = 1;
	led->cdev.brightness = (val_reg & LED_CFG_MASK) ? 1 : 0;

	rc = led_classdev_register(&pdev->dev, &led->cdev);
	if (rc) {
		dev_err(&pdev->dev, "unable to ATC led rc=%d\n", rc);
		return -ENODEV;
	}

	dev_set_drvdata(&pdev->dev, led);
	dev_info(&pdev->dev, "ATC LED at %#x, ctrl %#x\n", led->addr, val_reg);
	return 0;
}

static int atc_leds_remove(struct platform_device *pdev)
{
	struct atc_led_data *led  = dev_get_drvdata(&pdev->dev);

	if (NULL != led)
		led_classdev_unregister(&led->cdev);

	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id spmi_match_table[] = {
	{ .compatible = "qcom,leds-atc",},
	{ },
};
#else
#define spmi_match_table NULL
#endif

static struct platform_driver atc_leds_driver = {
	.driver		= {
		.name	= "qcom,leds-atc",
		.of_match_table = spmi_match_table,
	},
	.probe		= atc_leds_probe,
	.remove		= atc_leds_remove,
};

static int __init atc_led_init(void)
{
	return platform_driver_register(&atc_leds_driver);
}
module_init(atc_led_init);

static void __exit atc_led_exit(void)
{
	platform_driver_unregister(&atc_leds_driver);
}
module_exit(atc_led_exit);

MODULE_DESCRIPTION("ATC LED driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("leds:leds-atc");
