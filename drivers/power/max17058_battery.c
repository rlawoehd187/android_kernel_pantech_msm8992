/*
 * Maxim MAX17058 ModelGauge fuel gauge
 *
 * Reports the state of charge and cell voltage of the MAX17058 through a
 * power supply, so that a charger/BMS driver can use it as an external
 * gauge (see qcom,ext-soc-psy-name in the qpnp-fg binding).
 *
 * The SOC scaling and RCOMP temperature compensation follow the Pantech
 * EF78S stock kernel.
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

#define pr_fmt(fmt) "max17058: " fmt

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/of.h>

#define MAX17058_VCELL		0x02
#define MAX17058_SOC		0x04
#define MAX17058_VERSION	0x08
#define MAX17058_CONFIG		0x0C

/* stock calibration: 2.55% reads as empty, 97.7% as full */
#define MAX17058_SOC_EMPTY	2550	/* 1/1000 % */
#define MAX17058_SOC_FULL	97700	/* 1/1000 % */

#define MAX17058_RCOMP_DEFAULT	0x5E
#define MAX17058_CONFIG_LSB	0x1F	/* sleep off, alert at 1% */
#define MAX17058_TEMP_CO_HOT	675	/* -0.675 per degC above 20 degC */
#define MAX17058_TEMP_CO_COLD	(-5825)	/* -5.825 per degC below 20 degC */

#define MAX17058_POLL_MS	30000

struct max17058_chip {
	struct i2c_client	*client;
	struct power_supply	psy;
	struct delayed_work	work;
	int			soc;		/* % */
	int			vcell_uv;
	int			rcomp;
};

static enum power_supply_property max17058_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static int max17058_read_word(struct max17058_chip *chip, u8 reg, u16 *val)
{
	int rc, retry = 3;

	do {
		rc = i2c_smbus_read_word_swapped(chip->client, reg);
		if (rc >= 0) {
			*val = rc;
			return 0;
		}
		msleep(10);
	} while (--retry);

	dev_err(&chip->client->dev, "read 0x%02x failed: %d\n", reg, rc);
	return rc;
}

static int max17058_write_word(struct max17058_chip *chip, u8 reg, u16 val)
{
	int rc = i2c_smbus_write_word_swapped(chip->client, reg, val);

	if (rc < 0)
		dev_err(&chip->client->dev, "write 0x%02x failed: %d\n",
			reg, rc);
	return rc;
}

static int max17058_set_rcomp(struct max17058_chip *chip, int rcomp)
{
	int rc;

	if (rcomp == chip->rcomp)
		return 0;
	rc = max17058_write_word(chip, MAX17058_CONFIG,
				 (rcomp << 8) | MAX17058_CONFIG_LSB);
	if (!rc)
		chip->rcomp = rcomp;
	return rc;
}

/* temp in degC */
static int max17058_calc_rcomp(int temp)
{
	int rcomp = MAX17058_RCOMP_DEFAULT;

	if (temp > 20)
		rcomp -= (temp - 20) * MAX17058_TEMP_CO_HOT / 1000;
	else if (temp < 20)
		rcomp += (temp - 20) * MAX17058_TEMP_CO_COLD / 1000;

	return clamp(rcomp, 0, 255);
}

static void max17058_update_rcomp(struct max17058_chip *chip)
{
	struct power_supply *bms = power_supply_get_by_name("bms");
	union power_supply_propval temp = {0, };

	if (!bms || !bms->get_property ||
	    bms->get_property(bms, POWER_SUPPLY_PROP_TEMP, &temp))
		return;

	/* ignore readings from a missing or unsupported thermistor */
	if (temp.intval < -200 || temp.intval > 600)
		return;

	max17058_set_rcomp(chip, max17058_calc_rcomp(temp.intval / 10));
}

static int max17058_update(struct max17058_chip *chip)
{
	u16 vcell, soc;
	int rc, raw;

	rc = max17058_read_word(chip, MAX17058_VCELL, &vcell);
	if (rc)
		return rc;
	rc = max17058_read_word(chip, MAX17058_SOC, &soc);
	if (rc)
		return rc;

	/* 12-bit cell voltage in 1.25mV steps */
	chip->vcell_uv = (vcell >> 4) * 1250;

	/* SOC register counts 1/512 % */
	raw = soc * 1000 / 512;
	chip->soc = clamp((raw - MAX17058_SOC_EMPTY) * 100 /
			  (MAX17058_SOC_FULL - MAX17058_SOC_EMPTY), 0, 100);
	return 0;
}

static void max17058_work(struct work_struct *work)
{
	struct max17058_chip *chip = container_of(work, struct max17058_chip,
						  work.work);
	int old_soc = chip->soc;
	struct power_supply *batt;

	max17058_update_rcomp(chip);
	if (!max17058_update(chip) && chip->soc != old_soc) {
		power_supply_changed(&chip->psy);
		batt = power_supply_get_by_name("battery");
		if (batt)
			power_supply_changed(batt);
	}

	schedule_delayed_work(&chip->work, msecs_to_jiffies(MAX17058_POLL_MS));
}

static int max17058_get_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct max17058_chip *chip = container_of(psy, struct max17058_chip,
						  psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = chip->soc;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = chip->vcell_uv;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int max17058_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct max17058_chip *chip;
	u16 version;
	int rc;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_WORD_DATA))
		return -EIO;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;
	chip->rcomp = -1;
	i2c_set_clientdata(client, chip);

	rc = max17058_read_word(chip, MAX17058_VERSION, &version);
	if (rc)
		return rc;

	max17058_set_rcomp(chip, MAX17058_RCOMP_DEFAULT);
	rc = max17058_update(chip);
	if (rc)
		return rc;

	chip->psy.name = "max17058";
	chip->psy.type = POWER_SUPPLY_TYPE_BMS;
	chip->psy.properties = max17058_props;
	chip->psy.num_properties = ARRAY_SIZE(max17058_props);
	chip->psy.get_property = max17058_get_property;
	rc = power_supply_register(&client->dev, &chip->psy);
	if (rc) {
		dev_err(&client->dev, "power_supply_register failed: %d\n", rc);
		return rc;
	}

	INIT_DELAYED_WORK(&chip->work, max17058_work);
	schedule_delayed_work(&chip->work, msecs_to_jiffies(MAX17058_POLL_MS));

	dev_info(&client->dev, "version 0x%04x, soc %d%%, vcell %duV\n",
		 version, chip->soc, chip->vcell_uv);
	return 0;
}

static int max17058_remove(struct i2c_client *client)
{
	struct max17058_chip *chip = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&chip->work);
	power_supply_unregister(&chip->psy);
	return 0;
}

static int max17058_suspend(struct device *dev)
{
	struct max17058_chip *chip = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&chip->work);
	return 0;
}

static int max17058_resume(struct device *dev)
{
	struct max17058_chip *chip = dev_get_drvdata(dev);

	schedule_delayed_work(&chip->work, 0);
	return 0;
}

static SIMPLE_DEV_PM_OPS(max17058_pm_ops, max17058_suspend, max17058_resume);

static const struct of_device_id max17058_of_match[] = {
	{ .compatible = "maxim,max17058-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, max17058_of_match);

static const struct i2c_device_id max17058_id[] = {
	{ "max17058", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max17058_id);

static struct i2c_driver max17058_driver = {
	.driver = {
		.name		= "max17058",
		.owner		= THIS_MODULE,
		.of_match_table	= max17058_of_match,
		.pm		= &max17058_pm_ops,
	},
	.probe		= max17058_probe,
	.remove		= max17058_remove,
	.id_table	= max17058_id,
};
module_i2c_driver(max17058_driver);

MODULE_DESCRIPTION("Maxim MAX17058 fuel gauge");
MODULE_LICENSE("GPL v2");
