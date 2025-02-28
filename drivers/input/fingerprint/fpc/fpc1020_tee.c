/*
 * FPC1020 Fingerprint sensor device driver
 *
 * This driver will control the platform resources that the FPC fingerprint
 * sensor needs to operate. The major things are probing the sensor to check
 * that it is actually connected and let the Kernel know this and with that also
 * enabling and disabling of regulators, enabling and disabling of platform
 * clocks, controlling GPIOs such as SPI chip select, sensor reset line, sensor
 * IRQ line, MISO and MOSI lines.
 *
 * The driver will expose most of its available functionality in sysfs which
 * enables dynamic control of these features from eg. a user space process.
 *
 * The sensor's IRQ events will be pushed to Kernel's event handling system and
 * are exposed in the drivers event node. This makes it possible for a user
 * space process to poll the input node and receive IRQ events easily. Usually
 * this node is available under /dev/input/eventX where 'X' is a number given by
 * the event system. A user space process will need to traverse all the event
 * nodes and ask for its parent's name (through EVIOCGNAME) which should match
 * the value in device tree named input-device-name.
 *
 * This driver will NOT send any SPI commands to the sensor it only controls the
 * electrical parts.
 *
 *
 * Copyright (c) 2015 Fingerprint Cards AB <tech@fingerprints.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version 2
 * as published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <soc/qcom/scm.h>

#include <linux/wakelock.h>
#include <linux/input.h>

#ifdef CONFIG_FB
#include <linux/fb.h>
#include <linux/notifier.h>
#endif

#include <linux/project_info.h>
#include "../fingerprint_detect/fingerprint_detect.h"

static unsigned int ignor_home_for_ESD = 0;
module_param(ignor_home_for_ESD, uint, S_IRUGO | S_IWUSR);

#define FPC1020_RESET_LOW_US 1000
#define FPC1020_RESET_HIGH1_US 100
#define FPC1020_RESET_HIGH2_US 1250
#define FPC_TTW_HOLD_TIME 1000

/* Unused key value to avoid interfering with active keys */
#define KEY_FINGERPRINT 0x2ee

#define ONEPLUS_EDIT  //Onplus modify for msm8996 platform and 15801 HW

struct fpc1020_data {
	struct device *dev;
    struct wake_lock ttw_wl;
	int irq_gpio;
	int rst_gpio;
	int irq_num;
	struct mutex lock;
	bool prepared;

	struct pinctrl         *ts_pinctrl;
	struct pinctrl_state   *gpio_state_active;
	struct pinctrl_state   *gpio_state_suspend;

	#ifdef ONEPLUS_EDIT
	int EN_VDD_gpio;
	int id0_gpio;
	int id1_gpio;
	int id2_gpio;
	struct input_dev *input_dev;
	int screen_state; //1: on 0:off
	/*int sensor_version;*/ //0x01:fpc1245 0x02:fpc1263
	int project_version;
	#endif
	#if defined(CONFIG_FB)
	struct notifier_block fb_notif;
    #endif
	struct work_struct pm_work;
	int proximity_state; /* 0:far 1:near */
	bool irq_enabled;
	spinlock_t irq_lock;
	struct completion irq_sent;
};

static int fpc1020_request_named_gpio(struct fpc1020_data *fpc1020,
		const char *label, int *gpio)
{
	struct device *dev = fpc1020->dev;
	struct device_node *np = dev->of_node;
	int rc = of_get_named_gpio(np, label, 0);
	if (rc < 0) {
		dev_err(dev, "failed to get '%s'\n", label);
		*gpio = rc;
		return rc;
	}
	*gpio = rc;
	rc = devm_gpio_request(dev, *gpio, label);
	if (rc) {
		dev_err(dev, "failed to request gpio %d\n", *gpio);
		return rc;
	}
	dev_info(dev, "%s - gpio: %d\n", label, *gpio);
	return 0;
}
#ifndef ONEPLUS_EDIT
/* -------------------------------------------------------------------- */
static int fpc1020_pinctrl_init(struct fpc1020_data *fpc1020)
{
	int ret = 0;
	struct device *dev = fpc1020->dev;

	fpc1020->ts_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(fpc1020->ts_pinctrl)) {
		dev_err(dev, "Target does not use pinctrl\n");
		ret = PTR_ERR(fpc1020->ts_pinctrl);
		goto err;
	}

	fpc1020->gpio_state_active =
		pinctrl_lookup_state(fpc1020->ts_pinctrl, "pmx_fp_active");
	if (IS_ERR_OR_NULL(fpc1020->gpio_state_active)) {
		dev_err(dev, "Cannot get active pinstate\n");
		ret = PTR_ERR(fpc1020->gpio_state_active);
		goto err;
	}

	fpc1020->gpio_state_suspend =
		pinctrl_lookup_state(fpc1020->ts_pinctrl, "pmx_fp_suspend");
	if (IS_ERR_OR_NULL(fpc1020->gpio_state_suspend)) {
		dev_err(dev, "Cannot get sleep pinstate\n");
		ret = PTR_ERR(fpc1020->gpio_state_suspend);
		goto err;
	}

	return 0;
err:
	fpc1020->ts_pinctrl = NULL;
	fpc1020->gpio_state_active = NULL;
	fpc1020->gpio_state_suspend = NULL;
	return ret;
}

/* -------------------------------------------------------------------- */
static int fpc1020_pinctrl_select(struct fpc1020_data *fpc1020, bool on)
{
	int ret = 0;
	struct pinctrl_state *pins_state;
	struct device *dev = fpc1020->dev;

	pins_state = on ? fpc1020->gpio_state_active : fpc1020->gpio_state_suspend;
	if (!IS_ERR_OR_NULL(pins_state)) {
		ret = pinctrl_select_state(fpc1020->ts_pinctrl, pins_state);
		if (ret) {
			dev_err(dev, "can not set %s pins\n",
				on ? "pmx_ts_active" : "pmx_ts_suspend");
			return ret;
		}
	} else {
		dev_err(dev, "not a valid '%s' pinstate\n",
			on ? "pmx_ts_active" : "pmx_ts_suspend");
	}

	return ret;
}
#endif
/*
static int hw_reset(struct  fpc1020_data *fpc1020)
{
	int irq_gpio;
	struct device *dev = fpc1020->dev;

	int rc = select_pin_ctl(fpc1020, "fpc1020_reset_active");
	if (rc)
		goto exit;
	usleep_range(FPC1020_RESET_HIGH1_US, FPC1020_RESET_HIGH1_US + 100);

	rc = select_pin_ctl(fpc1020, "fpc1020_reset_reset");
	if (rc)
		goto exit;
	usleep_range(FPC1020_RESET_LOW_US, FPC1020_RESET_LOW_US + 100);

	rc = select_pin_ctl(fpc1020, "fpc1020_reset_active");
	if (rc)
		goto exit;
	usleep_range(FPC1020_RESET_HIGH1_US, FPC1020_RESET_HIGH1_US + 100);

	irq_gpio = gpio_get_value(fpc1020->irq_gpio);
	dev_info(dev, "IRQ after reset %d\n", irq_gpio);
exit:
	return rc;
}

static int hw_reset(struct  fpc1020_data *fpc1020)
{
    int irq_gpio;
	struct device *dev = fpc1020->dev;
	int counter = 2;

	gpio_set_value(fpc1020->EN_VDD_gpio, 0);
    mdelay(3);
	gpio_set_value(fpc1020->EN_VDD_gpio, 1);
	mdelay(3);
	//gpio_direction_output(fpc1020->EN_VDD_gpio,1);

	while (counter) {
		counter--;

		gpio_set_value(fpc1020->rst_gpio, 1);
		udelay(FPC1020_RESET_HIGH1_US);

		gpio_set_value(fpc1020->rst_gpio, 0);
		udelay(FPC1020_RESET_LOW_US);

		gpio_set_value(fpc1020->rst_gpio, 1);
		udelay(FPC1020_RESET_HIGH2_US);

		irq_gpio = gpio_get_value(fpc1020->irq_gpio);
		dev_err(dev, "IRQ after reset %d\n", irq_gpio);
		if (irq_gpio) {
			//printk(KERN_INFO "%s OK !\n", __func__);
			counter = 0;
		} else {
			dev_err(dev, "%s timed out,retrying ...\n",
				__func__);

			udelay(1250);
		}
	}
	return 0;
}
static ssize_t hw_reset_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int rc;
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strncmp(buf, "reset", strlen("reset")))
		rc = hw_reset(fpc1020);
	else
		return -EINVAL;
	return rc ? rc : count;
}
static DEVICE_ATTR(hw_reset, S_IWUSR, NULL, hw_reset_set);

*/
/**
 * sysf node to check the interrupt status of the sensor, the interrupt
 * handler should perform sysf_notify to allow userland to poll the node.
 */
static ssize_t irq_get(struct device* device,
			     struct device_attribute* attribute,
			     char* buffer)
{
	struct fpc1020_data* fpc1020 = dev_get_drvdata(device);
	bool irq_enabled;
	int irq;
	ssize_t count;

	spin_lock(&fpc1020->irq_lock);
	irq_enabled = fpc1020->irq_enabled;
	spin_unlock(&fpc1020->irq_lock);

	irq = irq_enabled && gpio_get_value(fpc1020->irq_gpio);
	count = scnprintf(buffer, PAGE_SIZE, "%i\n", irq);

	complete(&fpc1020->irq_sent);

	return count;
}


/**
 * writing to the irq node will just drop a printk message
 * and return success, used for latency measurement.
 */
static ssize_t irq_ack(struct device* device,
			     struct device_attribute* attribute,
			     const char* buffer, size_t count)
{
	struct fpc1020_data* fpc1020 = dev_get_drvdata(device);
	dev_dbg(fpc1020->dev, "%s\n", __func__);
	return count;
}
static DEVICE_ATTR(irq, S_IRUSR | S_IWUSR, irq_get, irq_ack);


static void set_fpc_irq(struct fpc1020_data *fpc1020, bool enable)
{
	bool irq_enabled;

	spin_lock(&fpc1020->irq_lock);
	irq_enabled = fpc1020->irq_enabled;
	fpc1020->irq_enabled = enable;
	spin_unlock(&fpc1020->irq_lock);

	if (enable == irq_enabled)
		return;

	if (enable)
		enable_irq(gpio_to_irq(fpc1020->irq_gpio));
	else
		disable_irq(gpio_to_irq(fpc1020->irq_gpio));
}

static ssize_t report_home_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);
    //unsigned long time;

	if(ignor_home_for_ESD)
		return -EINVAL;
	if (!strncmp(buf, "down", strlen("down")))
	{
            input_report_key(fpc1020->input_dev,
                            KEY_LEFTMETA, 1);
            input_sync(fpc1020->input_dev);
	}
	else if (!strncmp(buf, "up", strlen("up")))
	{
            input_report_key(fpc1020->input_dev,
                            KEY_LEFTMETA, 0);
            input_sync(fpc1020->input_dev);
	}
    else if (!strncmp(buf, "timeout", strlen("timeout")))
    {
      input_report_key(fpc1020->input_dev,KEY_F2,1);
      input_sync(fpc1020->input_dev);
      input_report_key(fpc1020->input_dev,KEY_F2,0);
      input_sync(fpc1020->input_dev);
    }
	else
		return -EINVAL;

	return count;
}
static DEVICE_ATTR(report_home, S_IWUSR, NULL, report_home_set);

static ssize_t report_key_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct input_event *ev = (struct input_event *)buf;
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	input_event(fpc1020->input_dev, ev->type, ev->code, ev->value);
	return count;
}
static DEVICE_ATTR(report_key, S_IWUSR, NULL, report_key_set);
static ssize_t update_info_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	//struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strncmp(buf, "n", strlen("n")))
	{
		push_component_info(FINGERPRINTS,"N/A" , "N/A");
	}

	return count;
}
static DEVICE_ATTR(update_info, S_IWUSR, NULL, update_info_set);

static ssize_t screen_state_get(struct device* device,
			     struct device_attribute* attribute,
			     char* buffer)
{
	struct fpc1020_data* fpc1020 = dev_get_drvdata(device);
	return scnprintf(buffer, PAGE_SIZE, "%i\n", fpc1020->screen_state);
}

static DEVICE_ATTR(screen_state, S_IRUSR , screen_state_get, NULL);

static ssize_t sensor_version_get(struct device* device,
			     struct device_attribute* attribute,
			     char* buffer)
{
	return scnprintf(buffer, PAGE_SIZE, "%i\n", fp_version);
}

static DEVICE_ATTR(sensor_version, S_IRUSR , sensor_version_get, NULL);

static ssize_t proximity_state_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int rc, val;

	rc = kstrtoint(buf, 10, &val);
	if (rc)
		return -EINVAL;

	fpc1020->proximity_state = !!val;

	if (!fpc1020->screen_state)
		set_fpc_irq(fpc1020, !fpc1020->proximity_state);

	return count;
}

static DEVICE_ATTR(proximity_state, S_IWUSR, NULL, proximity_state_set);

static struct attribute *attributes[] = {
	//&dev_attr_hw_reset.attr,
	&dev_attr_irq.attr,
	&dev_attr_report_home.attr,
	&dev_attr_update_info.attr,
	&dev_attr_screen_state.attr,
	&dev_attr_sensor_version.attr,
	&dev_attr_report_key.attr,
	&dev_attr_proximity_state.attr,
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

int fpc1020_input_init(struct fpc1020_data *fpc1020)
{
	int error = 0;


	dev_dbg(fpc1020->dev, "%s\n", __func__);

	fpc1020->input_dev = input_allocate_device();

	if (!fpc1020->input_dev) {
		dev_err(fpc1020->dev, "Input_allocate_device failed.\n");
		error  = -ENOMEM;
	}

	if (!error) {
		fpc1020->input_dev->name = "fpc1020";

		/* Set event bits according to what events we are generating */
		set_bit(EV_KEY, fpc1020->input_dev->evbit);
		set_bit(EV_SYN, fpc1020->input_dev->evbit);
		set_bit(EV_ABS, fpc1020->input_dev->evbit);

		set_bit(KEY_POWER, fpc1020->input_dev->keybit);
		set_bit(KEY_F2, fpc1020->input_dev->keybit);
		set_bit(KEY_LEFTMETA, fpc1020->input_dev->keybit);
		set_bit(KEY_FINGERPRINT, fpc1020->input_dev->keybit);

		/*
		set_bit(BTN_A, fpc1020->input_dev->keybit);
		set_bit(BTN_C, fpc1020->input_dev->keybit);
		set_bit(BTN_B, fpc1020->input_dev->keybit);
		set_bit(ABS_Z, fpc1020->input_dev->keybit);
		set_bit(KEY_UP, fpc1020->input_dev->keybit);
		set_bit(KEY_DOWN, fpc1020->input_dev->keybit);
		set_bit(KEY_LEFT, fpc1020->input_dev->keybit);
		set_bit(KEY_RIGHT, fpc1020->input_dev->keybit);
		*/

		/* Register the input device */
		error = input_register_device(fpc1020->input_dev);


		if (error) {
			dev_err(fpc1020->dev, "Input_register_device failed.\n");
			input_free_device(fpc1020->input_dev);
			fpc1020->input_dev = NULL;
		}
	}

	return error;
}

/* -------------------------------------------------------------------- */
void fpc1020_input_destroy(struct fpc1020_data *fpc1020)
{
	dev_dbg(fpc1020->dev, "%s\n", __func__);

	if (fpc1020->input_dev != NULL)
		input_free_device(fpc1020->input_dev);
}

static void fpc1020_suspend_resume(struct work_struct *work)
{
	struct fpc1020_data *fpc1020 =
		container_of(work, typeof(*fpc1020), pm_work);

	if (fpc1020->screen_state) {
		set_fpc_irq(fpc1020, true);
	}

	sysfs_notify(&fpc1020->dev->kobj, NULL,
				dev_attr_screen_state.attr.name);
}

#if defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
	struct fpc1020_data *fpc1020 = container_of(self, struct fpc1020_data, fb_notif);
	struct fb_event *evdata = data;
	int *blank = evdata->data;

	if (event != FB_EARLY_EVENT_BLANK)
		return 0;

	if (*blank == FB_BLANK_UNBLANK) {
		fpc1020->screen_state = 1;
		queue_work(system_highpri_wq, &fpc1020->pm_work);
	} else if (*blank == FB_BLANK_POWERDOWN) {
		fpc1020->screen_state = 0;
		queue_work(system_highpri_wq, &fpc1020->pm_work);
	}

	return 0;
}
#endif

static irqreturn_t fpc1020_irq_handler(int irq, void *handle)
{
	struct fpc1020_data *fpc1020 = handle;

	sysfs_notify(&fpc1020->dev->kobj, NULL, dev_attr_irq.attr.name);

	reinit_completion(&fpc1020->irq_sent);
	wait_for_completion_timeout(&fpc1020->irq_sent, msecs_to_jiffies(100));

	if (fpc1020->screen_state)
		return IRQ_HANDLED;

	wake_lock_timeout(&fpc1020->ttw_wl, msecs_to_jiffies(FPC_TTW_HOLD_TIME));

	/* Report button input to trigger CPU boost */
	input_report_key(fpc1020->input_dev, KEY_FINGERPRINT, 1);
	input_sync(fpc1020->input_dev);
	input_report_key(fpc1020->input_dev, KEY_FINGERPRINT, 0);
	input_sync(fpc1020->input_dev);

	return IRQ_HANDLED;
}

static int fpc1020_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc = 0;
	unsigned long irqf;
	struct device_node *np;
	struct fpc1020_data *fpc1020;

	pr_info("%s: fp version %x\n", __func__, fp_version);
	if ((fp_version != 0x01) && (fp_version != 0x02))
		return 0;

	np = dev->of_node;
	fpc1020 = devm_kzalloc(dev, sizeof(*fpc1020),
			GFP_KERNEL);
	if (!fpc1020) {
		dev_err(dev,
			"failed to allocate memory for struct fpc1020_data\n");
		rc = -ENOMEM;
		goto exit;
	}

	printk(KERN_INFO "%s\n", __func__);

	fpc1020->dev = dev;
	dev_set_drvdata(dev, fpc1020);

	if (!np) {
		dev_err(dev, "no of node found\n");
		rc = -EINVAL;
		goto exit;
	}

	if (of_property_read_bool(fpc1020->dev->of_node, "oem,dumpling"))
		fpc1020->project_version = 0x02;
	else
		fpc1020->project_version = 0x01;

	rc = fpc1020_request_named_gpio(fpc1020, "fpc,irq-gpio",
			&fpc1020->irq_gpio);
	if (rc)
		goto exit;

	rc = gpio_direction_input(fpc1020->irq_gpio);
	
	if (rc) {
		dev_err(fpc1020->dev,
			"gpio_direction_input (irq) failed.\n");
		goto exit;
	}
    /*  in tz
	rc = fpc1020_request_named_gpio(fpc1020, "fpc,reset-gpio",
			&fpc1020->rst_gpio);
	if (rc)
		goto exit;*/

    #ifdef ONEPLUS_EDIT
    /* in xbl
	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_1V8_EN",
			&fpc1020->EN_VDD_gpio);
	if (rc)
		goto exit;
    gpio_direction_output(fpc1020->EN_VDD_gpio,1);*/

    #else
	rc = fpc1020_pinctrl_init(fpc1020);
	if (rc)
		goto exit;

	rc = fpc1020_pinctrl_select(fpc1020, true);
	if (rc)
		goto exit;
		
    #endif
    rc = fpc1020_input_init(fpc1020);
    if (rc)
		goto exit;

	INIT_WORK(&fpc1020->pm_work, fpc1020_suspend_resume);

    #if defined(CONFIG_FB)
	fpc1020->fb_notif.notifier_call = fb_notifier_callback;
	rc = fb_register_client(&fpc1020->fb_notif);
	if(rc)
		dev_err(fpc1020->dev, "Unable to register fb_notifier: %d\n", rc);
    fpc1020->screen_state = 1;
    #endif

	spin_lock_init(&fpc1020->irq_lock);
	fpc1020->irq_enabled = true;

	irqf = IRQF_TRIGGER_RISING | IRQF_ONESHOT;
	mutex_init(&fpc1020->lock);
	init_completion(&fpc1020->irq_sent);
	rc = devm_request_threaded_irq(dev, gpio_to_irq(fpc1020->irq_gpio),
			NULL, fpc1020_irq_handler, irqf,
			dev_name(dev), fpc1020);
	if (rc) {
		dev_err(dev, "could not request irq %d\n",
				gpio_to_irq(fpc1020->irq_gpio));
		goto exit;
	}
	dev_info(dev, "requested irq %d\n", gpio_to_irq(fpc1020->irq_gpio));

	/* Request that the interrupt should not be wakeable */
	//disable_irq_wake( gpio_to_irq( fpc1020->irq_gpio ) );

	enable_irq_wake( gpio_to_irq( fpc1020->irq_gpio ) );
	wake_lock_init(&fpc1020->ttw_wl, WAKE_LOCK_SUSPEND, "fpc_ttw_wl");
	device_init_wakeup(fpc1020->dev, 1);

	rc = sysfs_create_group(&dev->kobj, &attribute_group);
	if (rc) {
		dev_err(dev, "could not create sysfs\n");
		goto exit;
	}
	
    #if 0 //changhua remove HW reset here,move to HAL,after spi cs pin become high
	rc = gpio_direction_output(fpc1020->rst_gpio, 1);

	if (rc) {
		dev_err(fpc1020->dev,
			"gpio_direction_output (reset) failed.\n");
		goto exit;
	}

	gpio_set_value(fpc1020->rst_gpio, 1);
	udelay(FPC1020_RESET_HIGH1_US);
	
	gpio_set_value(fpc1020->rst_gpio, 0);
	udelay(FPC1020_RESET_LOW_US);
	
	gpio_set_value(fpc1020->rst_gpio, 1);
	udelay(FPC1020_RESET_HIGH2_US);
    #endif
    /**
    *           ID0(GPIO39)   ID1(GPIO41)   ID1(GPIO63)
    *   fpc1245
    *   O-film   1            1             1
    *   Primax   1            0             0
    *   truly    0            0             1
    *
    *   fpc1263
    *   O-film   1            1             0
    *   Primax   0            0             0
    *   truly    0            1             1
    *fingerchip/
    *   qtech    0            1             0
    *   Goodix   1            0             1
    *   
    */
	dev_info(dev, "%s: ok\n", __func__);
exit:
	return rc;
}


static struct of_device_id fpc1020_of_match[] = {
	{ .compatible = "fpc,fpc1020", },
	{}
};
MODULE_DEVICE_TABLE(of, fpc1020_of_match);

static struct platform_driver fpc1020_driver = {
	.driver = {
		.name		= "fpc1020",
		.owner		= THIS_MODULE,
		.of_match_table = fpc1020_of_match,
	},
	.probe = fpc1020_probe,
};
module_platform_driver(fpc1020_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Aleksej Makarov");
MODULE_AUTHOR("Henrik Tillman <henrik.tillman@fingerprints.com>");
MODULE_DESCRIPTION("FPC1020 Fingerprint sensor device driver.");
