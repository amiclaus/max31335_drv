// SPDX-License-Identifier: GPL-2.0
/*
 * RTC driver for the MAX31335
 *
 * Copyright (C) 2023 Analog Devices
 *
 * Antoniu Miclaus <antoniu.miclaus@analog.com>
 *
 */

#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/rtc.h>


struct max31335_data {
	struct regmap *regmap;
	struct rtc_device *rtc;
}

static const struct regmap_config regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x5F,
};

static const struct rtc_class_ops max31335_rtc_ops = {
	.read_time = max31335_get_time,
	.set_time = max31335_set_time,
	.read_offset = max31335_read_offset,
	.set_offset = max31335_set_offset,
	.ioctl = max31335_ioctl,
	.read_alarm = max31335_get_alarm,
	.set_alarm = max31335_set_alarm,
	.alarm_irq_enable = max31335_alarm_irq_enable,
};

static int max31335_probe(struct i2c_client *client)
{
	struct max31335_data *max31335;
	int ret;

	max31335 = devm_kzalloc(&client->dev, sizeof(struct max31335_data),
			      GFP_KERNEL);
	if (!max31335)
		return -ENOMEM;

	max31335->regmap = devm_regmap_init_i2c(client, &regmap_config);
	if (IS_ERR(max31335->regmap))
		return PTR_ERR(max31335->regmap);

	i2c_set_clientdata(client, max31335);

	ret = regmap_read(max31335->regmap, max31335_STATUS1, &status);
	if (ret < 0)
		return ret;

	max31335->rtc = devm_rtc_allocate_device(&client->dev);
	if (IS_ERR(max31335->rtc))
		return PTR_ERR(max31335->rtc);

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, max31335_handle_irq,
						IRQF_TRIGGER_LOW | IRQF_ONESHOT,
						"max31335", max31335);
		if (ret) {
			dev_warn(&client->dev, "unable to request IRQ, alarmrv3032s disabled\n");
			client->irq = 0;
		}
	}

	if (!client->irq)
		clear_bit(RTC_FEATURE_ALARM, max31335->rtc->features);

	max31335->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	max31335->rtc->range_max = RTC_TIMESTAMP_END_2099;
	max31335->rtc->ops = &max31335_rtc_ops;
	ret = devm_rtc_register_device(max31335->rtc);
	if (ret)
		return ret;

	return 0;
}

static const __maybe_unused struct of_device_id max31335_of_match[] = {
	{ .compatible = "adi,max31335", },
	{ }
};
MODULE_DEVICE_TABLE(of, max31335_of_match);

static struct i2c_driver max31335_driver = {
	.driver = {
		.name = "rtc-max31335",
		.of_match_table = of_match_ptr(max31335_of_match),
	},
	.probe_new = max31335_probe,
};
module_i2c_driver(max31335_driver);

MODULE_AUTHOR("Alexandre Belloni <alexandre.belloni@bootlin.com>");
MODULE_DESCRIPTION("Micro Crystal max31335 RTC driver");
MODULE_LICENSE("GPL v2");