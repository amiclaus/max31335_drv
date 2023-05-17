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

#define MAX31335_STATUS1		0x00
#define MAX31335_INT_EN			0x01
#define MAX31335_STATUS2		0x02
#define MAX31335_INT_EN2		0x03
#define MAX31335_RTC_RESET		0x04
#define MAX31335_RTC_CONFIG		0x05
#define MAX31335_RTC_CONFIG2		0x06
#define MAX31335_TIMESTAMP_CONFIG	0x07
#define MAX31335_TIMER_CONFIG		0x08
#define MAX31335_SECONDS_1_128		0x09
#define MAX31335_SECONDS		0x0A
#define MAX31335_MINUTES		0x0B
#define MAX31335_HOURS			0x0C
#define MAX31335_DAY			0x0D
#define MAX31335_DATE			0x0E
#define MAX31335_MONTH			0x0F
#define MAX31335_YEAR			0x0F
#define MAX31335_ALM1_SEC		0x11
#define MAX31335_ALM1_MIN		0x12
#define MAX31335_ALM1_HRS		0x13
#define MAX31335_ALM1_DAY_DATE		0x14
#define MAX31335_ALM1_MON		0x15
#define MAX31335_ALM1_YEAR		0x16
#define MAX31335_ALM2_MIN		0x17
#define MAX31335_ALM2_HRS		0x18
#define MAX31335_ALM2_DAY_DATE		0x19
#define MAX31335_TIMER_COUNT		0x1A
#define MAX31335_TIMER_INIT		0x1B
#define MAX31335_PWR_MGMT		0x1C
#define MAX31335_TRICKLE_REG		0x1D
#define MAX31335_AGING_OFFSET		0x1E
#define MAX31335_TS_CONFIG		0x30
#define MAX31335_TEMP_ALARM_HIGH_MSB	0x31
#define MAX31335_TEMP_ALARM_HIGH_LSB	0x32
#define MAX31335_TEMP_ALARM_LOW_MSB	0x33
#define MAX31335_TEMP_ALARM_LOW_LSB	0x34
#define MAX31335_TEMP_DATA_MSB		0x35
#define MAX31335_TEMP_DATA_LSB		0x36
#define MAX31335_TS0_SEC_1_128		0x40
#define MAX31335_TS0_SEC		0x41
#define MAX31335_TS0_MIN		0x42
#define MAX31335_TS0_HOUR		0x43
#define MAX31335_TS0_DATE		0x44
#define MAX31335_TS0_MONTH		0x45
#define MAX31335_TS0_YEAR		0x46
#define MAX31335_TS0_FLAGS		0x47
#define MAX31335_TS1_SEC_1_128		0x48
#define MAX31335_TS1_SEC		0x49
#define MAX31335_TS1_MIN		0x4A
#define MAX31335_TS1_HOUR		0x4B
#define MAX31335_TS1_DATE		0x4C
#define MAX31335_TS1_MONTH		0x4D
#define MAX31335_TS1_YEAR		0x4E
#define MAX31335_TS1_FLAGS		0x4F
#define MAX31335_TS2_SEC_1_128		0x50
#define MAX31335_TS2_SEC		0x51
#define MAX31335_TS2_MIN		0x52
#define MAX31335_TS2_HOUR		0x53
#define MAX31335_TS2_DATE		0x54
#define MAX31335_TS2_MONTH		0x55
#define MAX31335_TS2_YEAR		0x56
#define MAX31335_TS2_FLAGS		0x57
#define MAX31335_TS3_SEC_1_128		0x58
#define MAX31335_TS3_SEC		0x59
#define MAX31335_TS3_MIN		0x5A
#define MAX31335_TS3_HOUR		0x5B
#define MAX31335_TS3_DATE		0x5C
#define MAX31335_TS3_MONTH		0x5D
#define MAX31335_TS3_YEAR		0x5E
#define MAX31335_TS3_FLAGS		0x5F

#define MAX31335_STATUS1_A1F		BIT(0)
#define MAX31335_INT_EN1_A1IE		BIT(0)

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