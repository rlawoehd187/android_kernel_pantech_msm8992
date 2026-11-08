/*
 * NXP PN547 NFC controller driver
 *
 * Exposes the controller to the NFC HAL as a character device: reads and
 * writes go to the chip over I2C, and an ioctl controls its power.
 *
 * Copyright (C) 2010 Trusted Logic S.A.
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
 */

#define pr_fmt(fmt) "pn547: " fmt

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/wakelock.h>

#define MAX_BUFFER_SIZE		512

#define PN547_MAGIC		0xE9
#define PN547_SET_PWR		_IOW(PN547_MAGIC, 0x01, unsigned int)

/* PN547_SET_PWR arguments */
#define PN547_PWR_OFF		0
#define PN547_PWR_ON		1
#define PN547_PWR_ON_FW_DL	2
#define PN547_RELEASE_READ	3
#define PN547_PWR_DOWN		0xFF

struct pn547_dev {
	wait_queue_head_t	read_wq;
	struct mutex		read_mutex;
	struct i2c_client	*client;
	struct miscdevice	misc;
	struct clk		*ref_clk;
	struct wake_lock	wake_lock;
	int			ven_gpio;
	int			firm_gpio;
	int			irq_gpio;
	spinlock_t		irq_enabled_lock;
	bool			irq_enabled;
	bool			irq_fired;
	bool			release_pending;
};

static void pn547_enable_irq(struct pn547_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_enabled_lock, flags);
	if (!dev->irq_enabled) {
		dev->irq_enabled = true;
		enable_irq(dev->client->irq);
	}
	spin_unlock_irqrestore(&dev->irq_enabled_lock, flags);
}

static void pn547_disable_irq(struct pn547_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_enabled_lock, flags);
	if (dev->irq_enabled) {
		disable_irq_nosync(dev->client->irq);
		dev->irq_enabled = false;
	}
	spin_unlock_irqrestore(&dev->irq_enabled_lock, flags);
}

static irqreturn_t pn547_dev_irq_handler(int irq, void *dev_id)
{
	struct pn547_dev *dev = dev_id;

	pn547_disable_irq(dev);
	dev->irq_fired = true;
	wake_up(&dev->read_wq);

	return IRQ_HANDLED;
}

static ssize_t pn547_dev_read(struct file *filp, char __user *buf,
			      size_t count, loff_t *offset)
{
	struct pn547_dev *dev = filp->private_data;
	char tmp[MAX_BUFFER_SIZE];
	int ret;

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	mutex_lock(&dev->read_mutex);

	if (!gpio_get_value(dev->irq_gpio)) {
		if (filp->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out_unlock;
		}

		dev->irq_fired = false;
		pn547_enable_irq(dev);
		ret = wait_event_interruptible(dev->read_wq, dev->irq_fired);
		pn547_disable_irq(dev);

		if (dev->release_pending) {
			dev->release_pending = false;
			ret = -EPERM;
			goto out_unlock;
		}
		if (ret)
			goto out_unlock;
	}

	ret = i2c_master_recv(dev->client, tmp, count);
	mutex_unlock(&dev->read_mutex);

	wake_lock_timeout(&dev->wake_lock, HZ);

	if (ret < 0) {
		pr_err("i2c_master_recv returned %d\n", ret);
		return ret;
	}
	if (ret > count) {
		pr_err("received too many bytes from i2c (%d)\n", ret);
		return -EIO;
	}
	if (copy_to_user(buf, tmp, ret))
		return -EFAULT;

	return ret;

out_unlock:
	mutex_unlock(&dev->read_mutex);
	return ret;
}

static ssize_t pn547_dev_write(struct file *filp, const char __user *buf,
			       size_t count, loff_t *offset)
{
	struct pn547_dev *dev = filp->private_data;
	char tmp[MAX_BUFFER_SIZE];
	int ret;

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	if (copy_from_user(tmp, buf, count))
		return -EFAULT;

	ret = i2c_master_send(dev->client, tmp, count);
	if (ret != count) {
		pr_err("i2c_master_send returned %d\n", ret);
		ret = -EIO;
	}

	return ret;
}

static int pn547_dev_open(struct inode *inode, struct file *filp)
{
	filp->private_data = container_of(filp->private_data,
					  struct pn547_dev, misc);
	return 0;
}

static long pn547_dev_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct pn547_dev *dev = filp->private_data;

	if (cmd != PN547_SET_PWR) {
		pr_err("bad ioctl %u\n", cmd);
		return -EINVAL;
	}

	switch (arg) {
	case PN547_PWR_ON_FW_DL:
		/* entering the firmware download mode needs a hardware reset */
		pr_info("power on with firmware\n");
		gpio_set_value(dev->ven_gpio, 1);
		gpio_set_value(dev->firm_gpio, 1);
		msleep(20);
		gpio_set_value(dev->ven_gpio, 0);
		msleep(60);
		gpio_set_value(dev->ven_gpio, 1);
		msleep(20);
		break;
	case PN547_PWR_ON:
		pr_info("power on\n");
		gpio_set_value(dev->firm_gpio, 0);
		gpio_set_value(dev->ven_gpio, 1);
		msleep(20);
		break;
	case PN547_PWR_OFF:
		pr_info("power off\n");
		gpio_set_value(dev->firm_gpio, 0);
		gpio_set_value(dev->ven_gpio, 0);
		msleep(60);
		break;
	case PN547_RELEASE_READ:
		pr_info("release pending read\n");
		dev->release_pending = true;
		dev->irq_fired = true;
		wake_up(&dev->read_wq);
		break;
	case PN547_PWR_DOWN:
		pr_info("power down\n");
		gpio_set_value(dev->ven_gpio, 0);
		gpio_set_value(dev->firm_gpio, 0);
		msleep(60);
		gpio_set_value(dev->ven_gpio, 1);
		msleep(20);
		gpio_set_value(dev->ven_gpio, 0);
		msleep(20);
		break;
	default:
		pr_err("bad arg %lu\n", arg);
		return -EINVAL;
	}

	return 0;
}

static const struct file_operations pn547_dev_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.read		= pn547_dev_read,
	.write		= pn547_dev_write,
	.open		= pn547_dev_open,
	.unlocked_ioctl	= pn547_dev_ioctl,
	.compat_ioctl	= pn547_dev_ioctl,
};

static int pn547_request_gpios(struct pn547_dev *dev)
{
	struct device *d = &dev->client->dev;
	struct device_node *np = d->of_node;
	int ret;

	dev->irq_gpio = of_get_named_gpio(np, "nxp,irq-gpio", 0);
	dev->ven_gpio = of_get_named_gpio(np, "nxp,ven-gpio", 0);
	dev->firm_gpio = of_get_named_gpio(np, "nxp,firm-gpio", 0);
	if (!gpio_is_valid(dev->irq_gpio) || !gpio_is_valid(dev->ven_gpio) ||
	    !gpio_is_valid(dev->firm_gpio)) {
		dev_err(d, "invalid gpios (ven, firm, irq)=(%d, %d, %d)\n",
			dev->ven_gpio, dev->firm_gpio, dev->irq_gpio);
		return -EINVAL;
	}

	ret = devm_gpio_request_one(d, dev->irq_gpio, GPIOF_IN,
				    "pn547_nfc_int");
	if (ret)
		return ret;
	ret = devm_gpio_request_one(d, dev->ven_gpio, GPIOF_OUT_INIT_LOW,
				    "pn547_nfc_ven");
	if (ret)
		return ret;
	return devm_gpio_request_one(d, dev->firm_gpio, GPIOF_OUT_INIT_LOW,
				     "pn547_nfc_firm");
}

static int pn547_probe(struct i2c_client *client,
		       const struct i2c_device_id *id)
{
	struct pn547_dev *dev;
	int ret;

	if (!client->dev.of_node)
		return -ENODEV;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "need I2C_FUNC_I2C\n");
		return -ENODEV;
	}

	dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->client = client;
	i2c_set_clientdata(client, dev);

	dev->ref_clk = devm_clk_get(&client->dev, "ref_clk");
	if (IS_ERR(dev->ref_clk)) {
		ret = PTR_ERR(dev->ref_clk);
		if (ret != -EPROBE_DEFER)
			dev_err(&client->dev, "no ref_clk: %d\n", ret);
		return ret;
	}

	ret = pn547_request_gpios(dev);
	if (ret)
		return ret;

	init_waitqueue_head(&dev->read_wq);
	mutex_init(&dev->read_mutex);
	spin_lock_init(&dev->irq_enabled_lock);
	wake_lock_init(&dev->wake_lock, WAKE_LOCK_SUSPEND, "pn547");

	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name = "pn547";
	dev->misc.fops = &pn547_dev_fops;
	ret = misc_register(&dev->misc);
	if (ret) {
		dev_err(&client->dev, "misc_register failed: %d\n", ret);
		goto err_wake_lock;
	}

	/* the line is high whenever the chip has data to be read */
	dev->irq_enabled = true;
	ret = request_irq(client->irq, pn547_dev_irq_handler,
			  IRQF_TRIGGER_HIGH, client->name, dev);
	if (ret) {
		dev_err(&client->dev, "request_irq failed: %d\n", ret);
		goto err_misc;
	}
	pn547_disable_irq(dev);

	ret = clk_prepare_enable(dev->ref_clk);
	if (ret) {
		dev_err(&client->dev, "failed to enable ref_clk: %d\n", ret);
		goto err_irq;
	}

	dev_info(&client->dev, "(ven, firm, irq)=(%d, %d, %d), irq %d\n",
		 dev->ven_gpio, dev->firm_gpio, dev->irq_gpio, client->irq);
	return 0;

err_irq:
	free_irq(client->irq, dev);
err_misc:
	misc_deregister(&dev->misc);
err_wake_lock:
	wake_lock_destroy(&dev->wake_lock);
	mutex_destroy(&dev->read_mutex);
	return ret;
}

static int pn547_remove(struct i2c_client *client)
{
	struct pn547_dev *dev = i2c_get_clientdata(client);

	clk_disable_unprepare(dev->ref_clk);
	free_irq(client->irq, dev);
	misc_deregister(&dev->misc);
	wake_lock_destroy(&dev->wake_lock);
	mutex_destroy(&dev->read_mutex);
	return 0;
}

static int pn547_suspend(struct device *d)
{
	struct pn547_dev *dev = dev_get_drvdata(d);

	if (gpio_get_value(dev->ven_gpio))
		irq_set_irq_wake(dev->client->irq, 1);
	return 0;
}

static int pn547_resume(struct device *d)
{
	struct pn547_dev *dev = dev_get_drvdata(d);

	if (gpio_get_value(dev->ven_gpio)) {
		if (gpio_get_value(dev->irq_gpio))
			wake_lock_timeout(&dev->wake_lock, HZ);
		irq_set_irq_wake(dev->client->irq, 0);
	}
	return 0;
}

static const struct dev_pm_ops pn547_pm_ops = {
	.suspend	= pn547_suspend,
	.resume		= pn547_resume,
};

static const struct of_device_id pn547_match_table[] = {
	{ .compatible = "nxp,pn547" },
	{ }
};
MODULE_DEVICE_TABLE(of, pn547_match_table);

static const struct i2c_device_id pn547_id[] = {
	{ "pn547", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pn547_id);

static struct i2c_driver pn547_driver = {
	.id_table	= pn547_id,
	.probe		= pn547_probe,
	.remove		= pn547_remove,
	.driver		= {
		.owner		= THIS_MODULE,
		.name		= "pn547",
		.of_match_table	= pn547_match_table,
		.pm		= &pn547_pm_ops,
	},
};
module_i2c_driver(pn547_driver);

MODULE_AUTHOR("Sylvain Fonteneau");
MODULE_DESCRIPTION("NXP PN547 NFC controller driver");
MODULE_LICENSE("GPL");
