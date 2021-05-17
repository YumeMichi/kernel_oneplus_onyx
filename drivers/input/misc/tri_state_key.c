/*
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/gpio_keys.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>

#include <linux/switch.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <asm/uaccess.h>

#include <linux/regulator/consumer.h>

#include <linux/timer.h>

#define DRV_NAME	"tri-state-key"
/*
 *
 *					KEY1(GPIO1)	KEY2(GPIO92)
 *	pin1 connect to pin4	0				1		 | MUTE
 *	pin2 connect to pin5	1				1		 | Do Not Disturb
 *	pin4 connect to pin3	1				0		 | Normal
 */
typedef enum {
	MODE_UNKNOWN,
	MODE_MUTE,
	MODE_DO_NOT_DISTURB,
	MODE_NORMAL,
	MODE_MAX_NUM
} tri_mode_t;

typedef enum {
	hw_old,
	hw_new
} hw_type;

struct switch_dev_data {
	int irq_key3;
	int irq_key2;
	int irq_key1;
	int key1_gpio;
	int key2_gpio;
	int key3_gpio;

	struct regulator *vdd_io;

	struct work_struct work;
	struct switch_dev sdev;
	struct device *dev;

	struct timer_list s_timer;
};

static struct switch_dev_data *switch_data;
static DEFINE_MUTEX(sem);

static int hw_version;
static void switch_dev_work(struct work_struct *work)
{
	mutex_lock(&sem);
	if (hw_version == hw_old) {
		if (!gpio_get_value(switch_data->key2_gpio)) {
			switch_set_state(&switch_data->sdev, MODE_NORMAL);
		} else {
			if (gpio_get_value(switch_data->key1_gpio))
				switch_set_state(&switch_data->sdev, MODE_DO_NOT_DISTURB);
			else
				switch_set_state(&switch_data->sdev, MODE_MUTE);
		}
		printk("%s, tristate set to state(%d)\n", __func__, switch_data->sdev.state);
	} else if (hw_version == hw_new) {
		gpio_set_value(switch_data->key3_gpio,0);
		if (!gpio_get_value(switch_data->key2_gpio)) {
			switch_set_state(&switch_data->sdev, MODE_NORMAL);
		}
		if (!gpio_get_value(switch_data->key3_gpio)) {
			switch_set_state(&switch_data->sdev, MODE_DO_NOT_DISTURB);
		}
		if (!gpio_get_value(switch_data->key1_gpio)) {
			switch_set_state(&switch_data->sdev, MODE_MUTE);
		}
	}
	mutex_unlock(&sem);
}

static irqreturn_t switch_dev_interrupt(int irq, void *_dev)
{
	schedule_work(&switch_data->work);
	return IRQ_HANDLED;
}

static void timer_handle(unsigned long arg)
{
	schedule_work(&switch_data->work);
}

#ifdef CONFIG_OF
static int switch_dev_get_devtree_pdata(struct device *dev)
{
	struct device_node *node;

	node = dev->of_node;
	if (!node)
		return -EINVAL;

	switch_data->key3_gpio = of_get_named_gpio(node, "tristate,gpio_key3", 0);
	if (switch_data->key3_gpio != -EINVAL)
		hw_version = hw_new;
	else
		hw_version = hw_old;
	pr_err("@switch_data->key3_gpio=%d\n", switch_data->key3_gpio);

	switch_data->key2_gpio = of_get_named_gpio(node, "tristate,gpio_key2", 0);
	if ((!gpio_is_valid(switch_data->key2_gpio)))
		return -EINVAL;
	pr_err("@switch_data->key2_gpio=%d\n", switch_data->key2_gpio);

	switch_data->key1_gpio = of_get_named_gpio(node, "tristate,gpio_key1", 0);
	if ((!gpio_is_valid(switch_data->key1_gpio)))
		return -EINVAL;
	pr_err("@switch_data->key1_gpio=%d\n", switch_data->key1_gpio);

	return 0;
}
#else
static inline int
switch_dev_get_devtree_pdata(struct device *dev)
{
	return 0;
}
#endif

static int tristate_dev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int error = 0;

	switch_data = kzalloc(sizeof(struct switch_dev_data), GFP_KERNEL);
	if (!switch_data)
		return -ENOMEM;

	switch_data->dev = dev;

	switch_data->sdev.name = DRV_NAME;

	error = switch_dev_register(&switch_data->sdev);
	if (error < 0)
		goto err_switch_dev_register;

	error = switch_dev_get_devtree_pdata(dev);
	if (error) {
		dev_err(dev, "parse device tree fail!!!\n");
		goto err_request_gpio;
	}

	switch_data->irq_key1 = gpio_to_irq(switch_data->key1_gpio);
	if (switch_data->irq_key1 <= 0) {
		printk("%s, irq number is not specified, irq #= %d, int pin=%d\n\n", __func__, switch_data->irq_key1, switch_data->key1_gpio);
		goto err_detect_irq_num_failed;
	} else {
		error = gpio_request(switch_data->key1_gpio, "tristate_key1-int");
		if (error < 0) {
			printk(KERN_ERR "%s: gpio_request, err=%d", __func__, error);
			goto err_request_gpio;
		}
		error = gpio_direction_input(switch_data->key1_gpio);
		if (error < 0) {
			printk(KERN_ERR "%s: gpio_direction_input, err=%d", __func__, error);
			goto err_set_gpio_input;
		}
		pr_err("%s: %d: hw_version: %d\n", __func__, __LINE__, hw_version);

		if (hw_version == hw_old)
			error = request_irq(switch_data->irq_key1, switch_dev_interrupt,
				IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING, "tristate_key1", switch_data);
		else
			error = request_irq(switch_data->irq_key1, switch_dev_interrupt,
				IRQF_TRIGGER_FALLING, "tristate_key1", switch_data);

		if (error) {
			dev_err(dev, "request_irq %i failed.\n", switch_data->irq_key1);
			switch_data->irq_key1 = -EINVAL;
			goto err_request_irq;
		}
	}

	switch_data->irq_key2 = gpio_to_irq(switch_data->key2_gpio);
	if (switch_data->irq_key2 <= 0) {
		printk("%s, irq number is not specified, irq #= %d, int pin=%d\n\n", __func__, switch_data->irq_key2, switch_data->key2_gpio);
		goto err_detect_irq_num_failed;
	} else {
		error = gpio_request(switch_data->key2_gpio, "tristate_key2-int");
		if (error < 0) {
			printk(KERN_ERR "%s: gpio_request, err=%d", __func__, error);
			goto err_request_gpio;
		}
		error = gpio_direction_input(switch_data->key2_gpio);
		if (error < 0) {
			printk(KERN_ERR "%s: gpio_direction_input, err=%d", __func__, error);
			goto err_set_gpio_input;
		}

		if (hw_version == hw_old)
			error = request_irq(switch_data->irq_key2, switch_dev_interrupt,
				IRQF_TRIGGER_FALLING|IRQF_TRIGGER_RISING, "tristate_key2", switch_data);
		else
			error = request_irq(switch_data->irq_key2, switch_dev_interrupt,
				IRQF_TRIGGER_FALLING, "tristate_key2", switch_data);

		if (error) {
			dev_err(dev, "request_irq %i failed.\n", switch_data->irq_key2);
			switch_data->irq_key2 = -EINVAL;
			goto err_request_irq;
		}
	}

	if (hw_version == hw_new) {
		switch_data->irq_key3 = gpio_to_irq(switch_data->key3_gpio);
		if (switch_data->irq_key3 <= 0) {
			printk("%s, irq number is not specified, irq #= %d, int pin=%d\n\n", __func__, \
			switch_data->irq_key3, switch_data->key3_gpio);
			goto err_detect_irq_num_failed;
		} else {
			error = gpio_request(switch_data->key3_gpio,"tristate_key3-int");
			if (error < 0) {
				printk(KERN_ERR "%s: gpio_request, err=%d", __func__, error);
				goto err_request_gpio;
			}
			error = gpio_direction_input(switch_data->key3_gpio);
			if (error < 0) {
				printk(KERN_ERR "%s: gpio_direction_input, err=%d", __func__, error);
				goto err_set_gpio_input;
			}
			error = request_irq(switch_data->irq_key3, switch_dev_interrupt,
				IRQF_TRIGGER_FALLING, "tristate_key3", switch_data);

			if (error) {
				dev_err(dev, "request_irq %i failed.\n", switch_data->irq_key3);
				switch_data->irq_key3 = -EINVAL;
				goto err_request_irq;
			}
		}
	}

	INIT_WORK(&switch_data->work, switch_dev_work);

	init_timer(&switch_data->s_timer);
	switch_data->s_timer.function = &timer_handle;
	switch_data->s_timer.expires = jiffies + 2*HZ;

	add_timer(&switch_data->s_timer);

	enable_irq_wake(switch_data->irq_key1);
	enable_irq_wake(switch_data->irq_key2);
	if (hw_version == hw_new)
		enable_irq_wake(switch_data->irq_key3);

	return 0;

err_request_irq:
err_detect_irq_num_failed:
err_set_gpio_input:
	gpio_free(switch_data->key2_gpio);
	gpio_free(switch_data->key1_gpio);
	if (hw_version == hw_new)
		gpio_free(switch_data->key3_gpio);
err_request_gpio:
	switch_dev_unregister(&switch_data->sdev);
err_switch_dev_register:
	kfree(switch_data);

	return error;
}

static int tristate_dev_remove(struct platform_device *pdev)
{
	cancel_work_sync(&switch_data->work);
	gpio_free(switch_data->key1_gpio);
	gpio_free(switch_data->key2_gpio);
	if (hw_version == hw_new)
		gpio_free(switch_data->key3_gpio);
	switch_dev_unregister(&switch_data->sdev);
	kfree(switch_data);

	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id tristate_dev_of_match[] = {
	{ .compatible = "oneplus,tri-state-key", },
	{ },
};
MODULE_DEVICE_TABLE(of, tristate_dev_of_match);
#endif

static struct platform_driver tristate_dev_driver = {
	.probe	= tristate_dev_probe,
	.remove	= tristate_dev_remove,
	.driver	= {
		.name	= DRV_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = tristate_dev_of_match,
	},
};
static int __init oem_tristate_init(void)
{
	return platform_driver_register(&tristate_dev_driver);
}
module_init(oem_tristate_init);

static void __exit oem_tristate_exit(void)
{
	platform_driver_unregister(&tristate_dev_driver);
}
module_exit(oem_tristate_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("switch Profiles by this triple key driver");
