/*
 * TI LM3559 camera flash, driven over CCI through the msm led flash
 * i2c trigger framework.
 *
 * The register settings follow the Pantech EF78S stock kernel.
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
#include <linux/module.h>
#include <linux/export.h>
#include "msm_led_flash.h"

#define FLASH_NAME "ti,lm3559"

#define LM3559_ENABLE		0x10
#define LM3559_TORCH_BRIGHT	0xa0
#define LM3559_FLASH_BRIGHT	0xb0

/* LM3559_ENABLE: strobe and tx pins enabled, mode in bits 1:0 */
#define LM3559_EN_SHUTDOWN	0x00
#define LM3559_EN_STANDBY	0x18
#define LM3559_EN_TORCH		0x1a
#define LM3559_EN_FLASH		0x1b

static struct msm_led_flash_ctrl_t fctrl;

static struct msm_camera_i2c_reg_array lm3559_init_array[] = {
	{LM3559_ENABLE, LM3559_EN_STANDBY},
};

static struct msm_camera_i2c_reg_array lm3559_off_array[] = {
	{LM3559_ENABLE, LM3559_EN_SHUTDOWN},
};

static struct msm_camera_i2c_reg_array lm3559_release_array[] = {
	{LM3559_ENABLE, LM3559_EN_SHUTDOWN},
};

static struct msm_camera_i2c_reg_array lm3559_low_array[] = {
	{LM3559_ENABLE, LM3559_EN_TORCH},
	{LM3559_TORCH_BRIGHT, 0x1b},
};

static struct msm_camera_i2c_reg_array lm3559_high_array[] = {
	{LM3559_ENABLE, LM3559_EN_FLASH},
	{LM3559_FLASH_BRIGHT, 0x55},
};

#define LM3559_SETTING(name)					\
static struct msm_camera_i2c_reg_setting lm3559_##name##_setting = {	\
	.reg_setting = lm3559_##name##_array,			\
	.size = ARRAY_SIZE(lm3559_##name##_array),		\
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,			\
	.data_type = MSM_CAMERA_I2C_BYTE_DATA,			\
	.delay = 0,						\
}

LM3559_SETTING(init);
LM3559_SETTING(off);
LM3559_SETTING(release);
LM3559_SETTING(low);
LM3559_SETTING(high);

static struct msm_led_flash_reg_t lm3559_regs = {
	.init_setting = &lm3559_init_setting,
	.off_setting = &lm3559_off_setting,
	.low_setting = &lm3559_low_setting,
	.high_setting = &lm3559_high_setting,
	.release_setting = &lm3559_release_setting,
};

static struct msm_flash_fn_t lm3559_func_tbl = {
	.flash_get_subdev_id = msm_led_i2c_trigger_get_subdev_id,
	.flash_led_config = msm_led_i2c_trigger_config,
	.flash_led_init = msm_flash_led_init,
	.flash_led_release = msm_flash_led_release,
	.flash_led_off = msm_flash_led_off,
	.flash_led_low = msm_flash_led_low,
	.flash_led_high = msm_flash_led_high,
};

static struct msm_camera_i2c_client lm3559_i2c_client = {
	.addr_type = MSM_CAMERA_I2C_BYTE_ADDR,
};

static struct msm_led_flash_ctrl_t fctrl = {
	.flash_i2c_client = &lm3559_i2c_client,
	.reg_setting = &lm3559_regs,
	.func_tbl = &lm3559_func_tbl,
};

static const struct of_device_id lm3559_dt_match[] = {
	{.compatible = FLASH_NAME, .data = &fctrl},
	{}
};
MODULE_DEVICE_TABLE(of, lm3559_dt_match);

static int msm_flash_lm3559_platform_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;

	match = of_match_device(lm3559_dt_match, &pdev->dev);
	if (!match)
		return -EFAULT;
	return msm_flash_probe(pdev, match->data);
}

static struct platform_driver lm3559_platform_driver = {
	.probe = msm_flash_lm3559_platform_probe,
	.driver = {
		.name = FLASH_NAME,
		.owner = THIS_MODULE,
		.of_match_table = lm3559_dt_match,
	},
};

module_platform_driver(lm3559_platform_driver);

MODULE_DESCRIPTION("TI LM3559 camera flash");
MODULE_LICENSE("GPL v2");
