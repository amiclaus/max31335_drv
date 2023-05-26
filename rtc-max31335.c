// SPDX-License-Identifier: GPL-2.0
/*
 * RTC driver for the MAX31335
 *
 * Copyright (C) 2023 Analog Devices
 *
 * Antoniu Miclaus <antoniu.miclaus@analog.com>
 *
 */

#include <asm-generic/unaligned.h>
#include <linux/bcd.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/util_macros.h>

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

#define MAX31335_TRICKLE		GENMASK(3, 1)
#define MAX31335_EN_TRICKLE		BIT(0)

#define MAX31335_HRS_F_AM_PM		BIT(5)
#define MAX31335_HRS_F_12_24		BIT(6)
#define MAX31335_MONTH_CENTURY		BIT(7)

#define MAX31335_ENCLKO			BIT(2)

#define clk_hw_to_max31335(_hw) container_of(_hw, struct max31335_data, clkout)

struct max31335_data {
	struct regmap *regmap;
	struct rtc_device *rtc;
	struct clk_hw clkout;
};

static const int max31335_clkout_freq[] = { 1, 64, 1024, 32768 };

static u16 max31335_trickle_resistors[] = {3000, 6000, 11000};

static bool max31335_volatile_reg(struct device *dev, unsigned int reg)
{
	/* time keeping registers */
	if (reg >= MAX31335_SECONDS && reg < MAX31335_SECONDS + MAX31335_TIME_SIZE)
		return true;

	/* interrupt status register */
	if (reg == MAX31335_INT_EN1_A1IE)
		return true;

	/* temperature registers */
	if (reg == MAX31335_TEMP_DATA_MSB || MAX31335_TEMP_DATA_LSB)
		return true;
}

static const struct regmap_config regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x5F,
	.volatile_reg = max31335_volatile_reg,
};

static int max31335_get_hour(u8 hour_reg)
{
	int hour;

	/* 24Hr mode */
	if (!FIELD_GET(MAX31335_HRS_F_12_24, hour_reg))
		return bcd2bin(hour_reg & 0x3f);

	/* 12Hr mode */
	hour = bcd2bin(hour_reg & 0x1f);
	if (hour == 12)
		hour = 0;

	if (FIELD_GET(MAX31335_HRS_F_AM_PM, hour_reg))
		hour += 12;

	return hour;
}

static int max31335_read_time(struct device *dev, struct rtc_time *tm)
{
	struct max31335_data *max31335 = dev_get_drvdata(dev);
	u8 date[7];
	int ret;

	ret = regmap_bulk_read(max31335->regmap, MAX31335_SECONDS, date,
			       sizeof(date));
	if (ret)
		return ret;

	tm->tm_sec  = bcd2bin(date[0] & 0x7f);
	tm->tm_min  = bcd2bin(date[1] & 0x7f);
	tm->tm_hour = max31335_get_hour(date[2]);
	tm->tm_wday = bcd2bin(date[3] & 0x7) - 1;
	tm->tm_mday = bcd2bin(date[4] & 0x3f);
	tm->tm_mon  = bcd2bin(date[5] & 0x1f) - 1;
	tm->tm_year = bcd2bin(date[6]) + 100;

	if (FIELD_GET(MAX31335_MONTH_CENTURY, date[5]))
		tm->tm_year += 100;

	return 0;
}

static int max31335_set_time(struct device *dev, struct rtc_time *tm)
{
	struct max31335_data *max31335 = dev_get_drvdata(dev);
	u8 date[7];

	date[0] = bin2bcd(tm->tm_sec);
	date[1] = bin2bcd(tm->tm_min);
	date[2] = bin2bcd(tm->tm_hour);
	date[3] = bin2bcd(tm->tm_wday + 1);
	date[4] = bin2bcd(tm->tm_mday);
	date[5] = bin2bcd(tm->tm_mon + 1);
	date[6] = bin2bcd(tm->tm_year % 100);

	if (tm->tm_year >= 200)
		date[5] |= FIELD_PREP(MAX31335_MONTH_CENTURY, 1);

	return regmap_bulk_write(max31335->regmap, MAX31335_SECONDS, date,
				 sizeof(date));
}

static int max31335_read_offset(struct device *dev, long *offset)
{
	struct max31335_data *max31335 = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = regmap_read(max31335->regmap, MAX31335_AGING_OFFSET, &value);
	if (ret)
		return ret;

	*offset = value;

	return 0;
}

static int max31335_set_offset(struct device *dev, long offset)
{
	struct max31335_data *max31335 = dev_get_drvdata(dev);

	return regmap_write(max31335->regmap, MAX31335_AGING_OFFSET, offset);
}

static int max31335_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct max31335_data *max31335 = dev_get_drvdata(dev);
	int ret, ctrl, status;
	struct rtc_time time;
	u8 regs[6];

	ret = regmap_bulk_read(max31335->regmap, MAX31335_ALM1_SEC, regs, sizeof(regs));
	if (ret)
		return ret;

	alrm->time.tm_sec  = bcd2bin(regs[0] & 0x7f);
	alrm->time.tm_min  = bcd2bin(regs[1] & 0x7f);
	alrm->time.tm_hour = bcd2bin(regs[2] & 0x3f);
	alrm->time.tm_mday = bcd2bin(regs[3] & 0x3f);
	alrm->time.tm_mon  = bcd2bin(regs[4] & 0x1f) - 1;
	alrm->time.tm_year = bcd2bin(regs[5]) + 100;

	ret = max31335_read_time(dev, &time);
	if (ret)
		return ret;

	if (time.tm_year >= 200)
		alrm->time.tm_year += 100;

	ret = regmap_read(max31335->regmap, MAX31335_INT_EN, &ctrl);
	if (ret)
		return ret;

	ret = regmap_read(max31335->regmap, MAX31335_STATUS1, &status);
	if (ret)
		return ret;

	alrm->enabled = FIELD_GET(MAX31335_INT_EN1_A1IE, ctrl);
	alrm->pending = FIELD_GET(MAX31335_STATUS1_A1F, status);

	return 0;
}

static int max31335_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct max31335_data *max31335 = dev_get_drvdata(dev);
	unsigned int reg;
	u8 regs[6];
	int ret;

	regs[0] = bin2bcd(alrm->time.tm_sec);
	regs[1] = bin2bcd(alrm->time.tm_min);
	regs[2] = bin2bcd(alrm->time.tm_hour);
	regs[3] = bin2bcd(alrm->time.tm_mday);
	regs[4] = bin2bcd(alrm->time.tm_mon + 1);
	regs[5] = bin2bcd(alrm->time.tm_year % 100);

	ret = regmap_bulk_write(max31335->regmap, MAX31335_ALM1_SEC,
				regs, sizeof(regs));
	if (ret)
		return ret;

	reg = FIELD_PREP(MAX31335_INT_EN1_A1IE, alrm->enabled);
	return regmap_update_bits(max31335->regmap, MAX31335_INT_EN,
				  MAX31335_INT_EN1_A1IE, reg);
	if (ret)
		return ret;

	return regmap_update_bits(max31335->regmap, MAX31335_STATUS1,
				 MAX31335_STATUS1_A1F, 0);
}

static int max31335_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct max31335_data *max31335 = dev_get_drvdata(dev);

	return regmap_update_bits(max31335->regmap, MAX31335_INT_EN,
				  MAX31335_INT_EN1_A1IE, enabled);
}

static irqreturn_t max31335_handle_irq(int irq, void *dev_id)
{
	struct max31335_data *max31335 = dev_id;
	struct mutex *lock = &max31335->rtc->ops_lock;
	int ret, status;

	mutex_lock(lock);

	ret = regmap_read(max31335->regmap, MAX31335_STATUS1, &status);
	if (ret)
		goto exit;

	if (FIELD_GET(MAX31335_STATUS1_A1F, status)) {
		ret = regmap_update_bits(max31335->regmap, MAX31335_STATUS1,
					 MAX31335_STATUS1_A1F, 0);
		if (ret)
			goto exit;

		rtc_update_irq(max31335->rtc, 1, RTC_AF | RTC_IRQF);
	}

exit:
	mutex_unlock(lock);

	return IRQ_HANDLED;
}

static const struct rtc_class_ops max31335_rtc_ops = {
	.read_time = max31335_read_time,
	.set_time = max31335_set_time,
	.read_offset = max31335_read_offset,
	.set_offset = max31335_set_offset,
	.read_alarm = max31335_read_alarm,
	.set_alarm = max31335_set_alarm,
	.alarm_irq_enable = max31335_alarm_irq_enable,
};

static int max31335_trickle_charger_setup(struct device *dev, struct max31335_data *max31335)
{
	u32 ohms;
	bool diode = false;
	int i;

	if (device_property_read_u32(dev, "trickle-resistor-ohms", &ohms))
		return 0;

	if (device_property_read_bool(dev, "trickle-diode-enable"))
		diode = true;

	for (i = 0; i < ARRAY_SIZE(max31335_trickle_resistors); i++)
		if (ohms == max31335_trickle_resistors[i])
			break;

	if (i >= ARRAY_SIZE(max31335_trickle_resistors)) {
		dev_warn(dev, "invalid trickle resistor value\n");

		return 0;
	}

	if (diode)
		i = i + 4;
	else
		i = i + 1;

	return regmap_write(max31335->regmap, MAX31335_TRICKLE_REG,
			    FIELD_PREP(MAX31335_TRICKLE, i) | MAX31335_EN_TRICKLE);
}

static unsigned long max31335_clkout_recalc_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct max31335_data *max31335 = clk_hw_to_max31335(hw);
	unsigned int freq_mask;
	unsigned int reg;
	int ret;

	ret = regmap_read(max31335->regmap, MAX31335_RTC_CONFIG2, &reg);
	if (ret)
		return 0;

	freq_mask = __roundup_pow_of_two(ARRAY_SIZE(max31335_clkout_freq)) - 1;

	return max31335_clkout_freq[reg & freq_mask];
}

static long max31335_clkout_round_rate(struct clk_hw *hw, unsigned long rate,
				       unsigned long *prate)
{
	int index;

	index = find_closest(rate, max31335_clkout_freq,
			     ARRAY_SIZE(max31335_clkout_freq));

	return max31335_clkout_freq[index];
}

static int max31335_clkout_set_rate(struct clk_hw *hw, unsigned long rate,
				    unsigned long parent_rate)
{
	struct max31335_data *max31335 = clk_hw_to_max31335(hw);
	unsigned int freq_mask;
	int index;

	index = find_closest(rate, max31335_clkout_freq, ARRAY_SIZE(max31335_clkout_freq));
	freq_mask = __roundup_pow_of_two(ARRAY_SIZE(max31335_clkout_freq)) - 1;

	return regmap_update_bits(max31335->regmap, MAX31335_RTC_CONFIG2,
				  freq_mask, index);
}

static int max31335_clkout_enable(struct clk_hw *hw)
{
	struct max31335_data *max31335 = clk_hw_to_max31335(hw);

	return regmap_set_bits(max31335->regmap, MAX31335_RTC_CONFIG2, MAX31335_ENCLKO);
}

static void max31335_clkout_disable(struct clk_hw *hw)
{
	struct max31335_data *max31335 = clk_hw_to_max31335(hw);

	regmap_clear_bits(max31335->regmap, MAX31335_RTC_CONFIG2, MAX31335_ENCLKO);
}

static int max31335_clkout_is_enabled(struct clk_hw *hw)
{
	struct max31335_data *max31335 = clk_hw_to_max31335(hw);
	unsigned int reg;
	int ret;

	ret = regmap_read(max31335->regmap, MAX31335_RTC_CONFIG2, &reg);
	if (ret)
		return ret;

	return !!(reg & MAX31335_ENCLKO);
}

static const struct clk_ops max31335_clkout_ops = {
	.recalc_rate = max31335_clkout_recalc_rate,
	.round_rate = max31335_clkout_round_rate,
	.set_rate = max31335_clkout_set_rate,
	.enable = max31335_clkout_enable,
	.disable = max31335_clkout_disable,
	.is_enabled = max31335_clkout_is_enabled,
};

struct clk_init_data max31335_clk_init = {
	.name = "max31335-clkout",
	.ops = &max31335_clkout_ops,
};

static int max31335_clkout_register(struct device *dev)
{
	struct max31335_data *max31335 = dev_get_drvdata(dev);
	int ret;

	if (!device_property_present(dev, "#clock-cells"))
		return 0;

	max31335->clkout.init = &max31335_clk_init;

	ret = devm_clk_hw_register(dev, &max31335->clkout);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register clock\n");

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
					  &max31335->clkout);
	if (ret)
		return dev_err_probe(dev, ret, "cannot add hw provider\n");

	ret = clk_prepare_enable(rtc->clkout.clk);
	if (ret)
		return dev_err_probe(dev, ret, "cannot enable clkout\n");

	return 0;
}

static int max31335_probe(struct i2c_client *client)
{
	struct max31335_data *max31335;
	int ret, status;

	max31335 = devm_kzalloc(&client->dev, sizeof(struct max31335_data),
				GFP_KERNEL);
	if (!max31335)
		return -ENOMEM;

	max31335->regmap = devm_regmap_init_i2c(client, &regmap_config);
	if (IS_ERR(max31335->regmap))
		return PTR_ERR(max31335->regmap);

	i2c_set_clientdata(client, max31335);

	ret = regmap_read(max31335->regmap, MAX31335_STATUS1, &status);
	if (ret < 0)
		return ret;

	max31335->rtc = devm_rtc_allocate_device(&client->dev);
	if (IS_ERR(max31335->rtc))
		return PTR_ERR(max31335->rtc);

	max31335->rtc->ops = &max31335_rtc_ops;
	max31335->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	max31335->rtc->range_max = RTC_TIMESTAMP_END_2199;

	ret = devm_rtc_register_device(max31335->rtc);
	if (ret)
		return ret;

	ret = max31335_clkout_register(&client->dev);
	if (ret)
		return ret;

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, max31335_handle_irq,
						IRQF_ONESHOT,
						"max31335", max31335);
		if (ret) {
			dev_warn(&client->dev,
				 "unable to request IRQ, alarm max31335 disabled\n");
			client->irq = 0;
		}
	}

	if (!client->irq)
		clear_bit(RTC_FEATURE_ALARM, max31335->rtc->features);

	max31335_trickle_charger_setup(&client->dev, max31335);

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

MODULE_AUTHOR("Antoniu Miclaus <antoniu.miclaus@analog.com>");
MODULE_DESCRIPTION("MAX31335 RTC driver");
MODULE_LICENSE("GPL v2");
