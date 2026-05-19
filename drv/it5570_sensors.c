// SPDX-License-Identifier: GPL-2.0-or-later
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "it5570_hwmon.h"

enum it5570_channel_kind {
	IT5570_CHANNEL_VOLTAGE = 0,
	IT5570_CHANNEL_TACH,
	IT5570_CHANNEL_PWM,
};

struct it5570_channel_desc {
	enum it5570_channel_kind kind;
	const char *name;
	u8 index;
};

struct it5570_ec_reg_span {
	u16 lsb_reg;
	u16 msb_reg;
	u16 status_reg;
	u8 status_mask;
};

struct it5570_raw_attr_desc {
	const char *name;
	u16 reg;
	enum it5570_channel_kind kind;
	u8 index;
};

static const struct it5570_channel_desc it5570_voltage_channels[] = {
	{ IT5570_CHANNEL_VOLTAGE, "in0", 0 },
	{ IT5570_CHANNEL_VOLTAGE, "in1", 1 },
	{ IT5570_CHANNEL_VOLTAGE, "in2", 2 },
	{ IT5570_CHANNEL_VOLTAGE, "in3", 3 },
	{ IT5570_CHANNEL_VOLTAGE, "in4", 4 },
	{ IT5570_CHANNEL_VOLTAGE, "in5", 5 },
	{ IT5570_CHANNEL_VOLTAGE, "in6", 6 },
	{ IT5570_CHANNEL_VOLTAGE, "in7", 7 },
};

static const struct it5570_channel_desc it5570_tach_channels[] = {
	{ IT5570_CHANNEL_TACH, "fan1", 1 },
	{ IT5570_CHANNEL_TACH, "fan2", 2 },
	{ IT5570_CHANNEL_TACH, "fan3", 3 },
};

static const struct it5570_channel_desc it5570_pwm_channels[] = {
	{ IT5570_CHANNEL_PWM, "pwm1", 1 },
	{ IT5570_CHANNEL_PWM, "pwm2", 2 },
	{ IT5570_CHANNEL_PWM, "pwm3", 3 },
	{ IT5570_CHANNEL_PWM, "pwm4", 4 },
	{ IT5570_CHANNEL_PWM, "pwm5", 5 },
	{ IT5570_CHANNEL_PWM, "pwm6", 6 },
	{ IT5570_CHANNEL_PWM, "pwm7", 7 },
	{ IT5570_CHANNEL_PWM, "pwm8", 8 },
};

/*
 * Datasheet 7.11.4 names the ADC result bytes as VCHnDATL/VCHnDATM and
 * ADCDVSTS. The status register mirrors channel numbering in its bit number,
 * so bit 0 validates VCH0DATL/M, bit 1 validates VCH1DATL/M, and so on.
 */
static const struct it5570_ec_reg_span it5570_voltage_reg_spans[] = {
	{ IT5570_EC_REG_VCH0DATL, IT5570_EC_REG_VCH0DATM,
	  IT5570_EC_REG_ADCDVSTS, BIT(0) },
	{ IT5570_EC_REG_VCH1DATL, IT5570_EC_REG_VCH1DATM,
	  IT5570_EC_REG_ADCDVSTS, BIT(1) },
	{ IT5570_EC_REG_VCH2DATL, IT5570_EC_REG_VCH2DATM,
	  IT5570_EC_REG_ADCDVSTS, BIT(2) },
	{ IT5570_EC_REG_VCH3DATL, IT5570_EC_REG_VCH3DATM,
	  IT5570_EC_REG_ADCDVSTS, BIT(3) },
	{ IT5570_EC_REG_VCH4DATL, IT5570_EC_REG_VCH4DATM,
	  IT5570_EC_REG_ADCDVSTS, BIT(4) },
	{ IT5570_EC_REG_VCH5DATL, IT5570_EC_REG_VCH5DATM,
	  IT5570_EC_REG_ADCDVSTS, BIT(5) },
	{ IT5570_EC_REG_VCH6DATL, IT5570_EC_REG_VCH6DATM,
	  IT5570_EC_REG_ADCDVSTS, BIT(6) },
	{ IT5570_EC_REG_VCH7DATL, IT5570_EC_REG_VCH7DATM,
	  IT5570_EC_REG_ADCDVSTS, BIT(7) },
};

/*
 * Datasheet 7.12.4.15/.17/.29 notes that each tachometer is exposed as an
 * LSB/MSB register pair. It also warns that D2EC/I2EC reads are not guaranteed
 * atomic, so runtime reads sample the pair twice while preserving the
 * documented zero-count stopped-fan state.
 */
static const struct it5570_ec_reg_span it5570_tach_reg_spans[] = {
	{ IT5570_EC_REG_F1TLRR, IT5570_EC_REG_F1TMRR, 0, 0 },
	{ IT5570_EC_REG_F2TLRR, IT5570_EC_REG_F2TMRR, 0, 0 },
	{ IT5570_EC_REG_F3TLRR, IT5570_EC_REG_F3TMRR, 0, 0 },
};

static const u16 it5570_pwm_duty_regs[] = {
	IT5570_EC_REG_PWM_DCR0,
	IT5570_EC_REG_PWM_DCR1,
	IT5570_EC_REG_PWM_DCR2,
	IT5570_EC_REG_PWM_DCR3,
	IT5570_EC_REG_PWM_DCR4,
	IT5570_EC_REG_PWM_DCR5,
	IT5570_EC_REG_PWM_DCR6,
	IT5570_EC_REG_PWM_DCR7,
};

/*
 * Raw nodes intentionally expose EC register values next to converted hwmon
 * values so board-specific routing and PWM grouping can be diagnosed without
 * opening up the corresponding control registers for writes.
 */
static const struct it5570_raw_attr_desc it5570_raw_attrs[] = {
	{ "fan1_tach_raw", IT5570_EC_REG_F1TLRR, IT5570_CHANNEL_TACH, 1 },
	{ "fan2_tach_raw", IT5570_EC_REG_F2TLRR, IT5570_CHANNEL_TACH, 2 },
	{ "fan3_tach_raw", IT5570_EC_REG_F3TLRR, IT5570_CHANNEL_TACH, 3 },
	{ "pwm1_cpr_raw", IT5570_EC_REG_PWM_C0CPRS, IT5570_CHANNEL_PWM, 0 },
	{ "pwm1_ctr_raw", IT5570_EC_REG_PWM_CTR0, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_group1_ctr_raw", IT5570_EC_REG_PWM_CTR1, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_group1_ctr_msb_raw", IT5570_EC_REG_PWM_CTR1M, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_group2_ctr_raw", IT5570_EC_REG_PWM_CTR2, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_group3_ctr_raw", IT5570_EC_REG_PWM_CTR3, IT5570_CHANNEL_PWM, 0 },
	{ "pwm1_dcr_raw", IT5570_EC_REG_PWM_DCR0, IT5570_CHANNEL_PWM, 0 },
	{ "pwm2_dcr_raw", IT5570_EC_REG_PWM_DCR1, IT5570_CHANNEL_PWM, 0 },
	{ "pwm3_dcr_raw", IT5570_EC_REG_PWM_DCR2, IT5570_CHANNEL_PWM, 0 },
	{ "pwm4_dcr_raw", IT5570_EC_REG_PWM_DCR3, IT5570_CHANNEL_PWM, 0 },
	{ "pwm5_dcr_raw", IT5570_EC_REG_PWM_DCR4, IT5570_CHANNEL_PWM, 0 },
	{ "pwm6_dcr_raw", IT5570_EC_REG_PWM_DCR5, IT5570_CHANNEL_PWM, 0 },
	{ "pwm7_dcr_raw", IT5570_EC_REG_PWM_DCR6, IT5570_CHANNEL_PWM, 0 },
	{ "pwm8_dcr_raw", IT5570_EC_REG_PWM_DCR7, IT5570_CHANNEL_PWM, 0 },
	{ "pwm3_dcr_msb_raw", IT5570_EC_REG_PWM_DCR2M, IT5570_CHANNEL_PWM, 0 },
	{ "pwm4_dcr_msb_raw", IT5570_EC_REG_PWM_DCR3M, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_polarity_raw", IT5570_EC_REG_PWM_POLARITY, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_clock_freq_sel_raw", IT5570_EC_REG_PWM_PCFSR, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_clock_src_sel_low_raw", IT5570_EC_REG_PWM_PCSSGL, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_clock_src_sel_high_raw", IT5570_EC_REG_PWM_PCSSGH, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_clock_gating_raw", IT5570_EC_REG_PWM_PCSGR, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_clock_control_raw", IT5570_EC_REG_PWM_ZTIER, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_group4_cpr_raw", IT5570_EC_REG_PWM_C4CPRS, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_group4_cpr_msb_raw", IT5570_EC_REG_PWM_C4MCPRS, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_group6_cpr_raw", IT5570_EC_REG_PWM_C6CPRS, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_group6_cpr_msb_raw", IT5570_EC_REG_PWM_C6MCPRS, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_group7_cpr_raw", IT5570_EC_REG_PWM_C7CPRS, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_group7_cpr_msb_raw", IT5570_EC_REG_PWM_C7MCPRS, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_6mhz_mode_raw", IT5570_EC_REG_PWM_CLK6MSEL, IT5570_CHANNEL_PWM, 0 },
	{ "pwm5_timeout_ctrl_raw", IT5570_EC_REG_PWM5TOCTRL, IT5570_CHANNEL_PWM, 0 },
	{ "backlight_duty_raw", IT5570_EC_REG_BLDR, IT5570_CHANNEL_PWM, 0 },
	{ "tach_zone_int_status_raw", IT5570_EC_REG_ZINTSCR, IT5570_CHANNEL_PWM, 0 },
	{ "tach_switch2_raw", IT5570_EC_REG_TSWCTLR2, IT5570_CHANNEL_PWM, 0 },
	{ "tach_switch_raw", IT5570_EC_REG_TSWCTLR, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_open_drain_raw", IT5570_EC_REG_PWMODENR, IT5570_CHANNEL_PWM, 0 },
	{ "pwm1_led_enable_raw", IT5570_EC_REG_PWM0LHE, IT5570_CHANNEL_PWM, 0 },
	{ "pwm1_led_ctrl1_raw", IT5570_EC_REG_PWM0LCR1, IT5570_CHANNEL_PWM, 0 },
	{ "pwm1_led_ctrl2_raw", IT5570_EC_REG_PWM0LCR2, IT5570_CHANNEL_PWM, 0 },
	{ "pwm2_led_enable_raw", IT5570_EC_REG_PWM1LHE, IT5570_CHANNEL_PWM, 0 },
	{ "pwm2_led_ctrl1_raw", IT5570_EC_REG_PWM1LCR1, IT5570_CHANNEL_PWM, 0 },
	{ "pwm2_led_ctrl2_raw", IT5570_EC_REG_PWM1LCR2, IT5570_CHANNEL_PWM, 0 },
	{ "pwm_load_counter_ctrl_raw", IT5570_EC_REG_PWMLCCR, IT5570_CHANNEL_PWM, 0 },
};

static const struct hwmon_channel_info * const it5570_hwmon_channel_info[] = {
	HWMON_CHANNEL_INFO(in,
		HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT,
		HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT),
	HWMON_CHANNEL_INFO(fan,
		HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm,
		HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT,
		HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT),
	NULL,
};

static int it5570_read_tach_raw_channel(struct it5570_hwmon_data *data,
					const struct it5570_channel_desc *channel,
					u16 *value);
static int it5570_read_voltage_channel(struct it5570_hwmon_data *data,
				       const struct it5570_channel_desc *channel,
				       long *value);
static int it5570_read_tach_channel(struct it5570_hwmon_data *data,
				    const struct it5570_channel_desc *channel,
				    long *value);
static int it5570_read_pwm_hwmon(struct it5570_hwmon_data *data,
				 const struct it5570_channel_desc *channel,
				 long *value);
static int it5570_read8_logged(struct it5570_hwmon_data *data, const char *class_name,
			       const char *channel_name, u16 reg, u8 *value);

static const struct it5570_raw_attr_desc *it5570_raw_attr_desc_lookup(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(it5570_raw_attrs); i++) {
		if (!strcmp(name, it5570_raw_attrs[i].name))
			return &it5570_raw_attrs[i];
	}

	return NULL;
}

static ssize_t tach_raw_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct it5570_hwmon_data *data = dev_get_drvdata(dev);
	const struct it5570_raw_attr_desc *desc;
	const struct it5570_channel_desc *channel;
	u16 raw;
	int ret;

	if (!data)
		return -EINVAL;

	desc = it5570_raw_attr_desc_lookup(attr->attr.name);
	if (!desc || desc->kind != IT5570_CHANNEL_TACH || !desc->index ||
	    desc->index > ARRAY_SIZE(it5570_tach_channels))
		return -EINVAL;

	channel = &it5570_tach_channels[desc->index - 1];
	ret = it5570_read_tach_raw_channel(data, channel, &raw);
	if (ret) {
		it5570_log_io_error(data, "read", "tach_raw", channel->name,
				    it5570_read_reason_from_errno(ret));
		return ret;
	}

	return sysfs_emit(buf, "%u\n", raw);
}

static ssize_t pwm_raw_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct it5570_hwmon_data *data = dev_get_drvdata(dev);
	const struct it5570_raw_attr_desc *desc;
	u8 value;
	int ret;

	if (!data)
		return -EINVAL;

	desc = it5570_raw_attr_desc_lookup(attr->attr.name);
	if (!desc || desc->kind != IT5570_CHANNEL_PWM)
		return -EINVAL;

	ret = it5570_read8_logged(data, "pwm_raw", desc->name, desc->reg, &value);
	if (ret) {
		it5570_log_io_error(data, "read", "pwm_raw", desc->name,
				    it5570_read_reason_from_errno(ret));
		return ret;
	}

	return sysfs_emit(buf, "%u\n", value);
}


#define IT5570_TACH_RAW_ATTR(_name) \
	static DEVICE_ATTR(_name, 0444, tach_raw_show, NULL)
#define IT5570_PWM_RAW_ATTR(_name) \
	static DEVICE_ATTR(_name, 0444, pwm_raw_show, NULL)

IT5570_TACH_RAW_ATTR(fan1_tach_raw);
IT5570_TACH_RAW_ATTR(fan2_tach_raw);
IT5570_TACH_RAW_ATTR(fan3_tach_raw);
IT5570_PWM_RAW_ATTR(pwm1_cpr_raw);
IT5570_PWM_RAW_ATTR(pwm1_ctr_raw);
IT5570_PWM_RAW_ATTR(pwm_group1_ctr_raw);
IT5570_PWM_RAW_ATTR(pwm_group1_ctr_msb_raw);
IT5570_PWM_RAW_ATTR(pwm_group2_ctr_raw);
IT5570_PWM_RAW_ATTR(pwm_group3_ctr_raw);
IT5570_PWM_RAW_ATTR(pwm1_dcr_raw);
IT5570_PWM_RAW_ATTR(pwm2_dcr_raw);
IT5570_PWM_RAW_ATTR(pwm3_dcr_raw);
IT5570_PWM_RAW_ATTR(pwm4_dcr_raw);
IT5570_PWM_RAW_ATTR(pwm5_dcr_raw);
IT5570_PWM_RAW_ATTR(pwm6_dcr_raw);
IT5570_PWM_RAW_ATTR(pwm7_dcr_raw);
IT5570_PWM_RAW_ATTR(pwm8_dcr_raw);
IT5570_PWM_RAW_ATTR(pwm3_dcr_msb_raw);
IT5570_PWM_RAW_ATTR(pwm4_dcr_msb_raw);
IT5570_PWM_RAW_ATTR(pwm_polarity_raw);
IT5570_PWM_RAW_ATTR(pwm_clock_freq_sel_raw);
IT5570_PWM_RAW_ATTR(pwm_clock_src_sel_low_raw);
IT5570_PWM_RAW_ATTR(pwm_clock_src_sel_high_raw);
IT5570_PWM_RAW_ATTR(pwm_clock_gating_raw);
IT5570_PWM_RAW_ATTR(pwm_clock_control_raw);
IT5570_PWM_RAW_ATTR(pwm_group4_cpr_raw);
IT5570_PWM_RAW_ATTR(pwm_group4_cpr_msb_raw);
IT5570_PWM_RAW_ATTR(pwm_group6_cpr_raw);
IT5570_PWM_RAW_ATTR(pwm_group6_cpr_msb_raw);
IT5570_PWM_RAW_ATTR(pwm_group7_cpr_raw);
IT5570_PWM_RAW_ATTR(pwm_group7_cpr_msb_raw);
IT5570_PWM_RAW_ATTR(pwm_6mhz_mode_raw);
IT5570_PWM_RAW_ATTR(pwm5_timeout_ctrl_raw);
IT5570_PWM_RAW_ATTR(backlight_duty_raw);
IT5570_PWM_RAW_ATTR(tach_zone_int_status_raw);
IT5570_PWM_RAW_ATTR(tach_switch2_raw);
IT5570_PWM_RAW_ATTR(tach_switch_raw);
IT5570_PWM_RAW_ATTR(pwm_open_drain_raw);
IT5570_PWM_RAW_ATTR(pwm1_led_enable_raw);
IT5570_PWM_RAW_ATTR(pwm1_led_ctrl1_raw);
IT5570_PWM_RAW_ATTR(pwm1_led_ctrl2_raw);
IT5570_PWM_RAW_ATTR(pwm2_led_enable_raw);
IT5570_PWM_RAW_ATTR(pwm2_led_ctrl1_raw);
IT5570_PWM_RAW_ATTR(pwm2_led_ctrl2_raw);
IT5570_PWM_RAW_ATTR(pwm_load_counter_ctrl_raw);

static umode_t it5570_extra_attr_is_visible(struct kobject *kobj,
					    struct attribute *attr, int index)
{
	struct device *dev = kobj_to_dev(kobj);
	struct it5570_hwmon_data *data = dev_get_drvdata(dev);
	const struct it5570_raw_attr_desc *desc;

	if (!data)
		return 0;

	desc = it5570_raw_attr_desc_lookup(attr->name);
	if (!desc)
		return 0;

	if (desc->kind == IT5570_CHANNEL_PWM)
		return data->transport_state.mode == IT5570_TRANSPORT_D2EC &&
			data->transport_state.ready ? attr->mode : 0;

	if (desc->kind != IT5570_CHANNEL_TACH || !desc->index ||
	    desc->index > ARRAY_SIZE(data->sensor.tach))
		return 0;

	return data->sensor.tach[desc->index - 1].available ? attr->mode : 0;
}

static struct attribute *it5570_extra_attrs[] = {
	&dev_attr_fan1_tach_raw.attr,
	&dev_attr_fan2_tach_raw.attr,
	&dev_attr_fan3_tach_raw.attr,
	&dev_attr_pwm1_cpr_raw.attr,
	&dev_attr_pwm1_ctr_raw.attr,
	&dev_attr_pwm_group1_ctr_raw.attr,
	&dev_attr_pwm_group1_ctr_msb_raw.attr,
	&dev_attr_pwm_group2_ctr_raw.attr,
	&dev_attr_pwm_group3_ctr_raw.attr,
	&dev_attr_pwm1_dcr_raw.attr,
	&dev_attr_pwm2_dcr_raw.attr,
	&dev_attr_pwm3_dcr_raw.attr,
	&dev_attr_pwm4_dcr_raw.attr,
	&dev_attr_pwm5_dcr_raw.attr,
	&dev_attr_pwm6_dcr_raw.attr,
	&dev_attr_pwm7_dcr_raw.attr,
	&dev_attr_pwm8_dcr_raw.attr,
	&dev_attr_pwm3_dcr_msb_raw.attr,
	&dev_attr_pwm4_dcr_msb_raw.attr,
	&dev_attr_pwm_polarity_raw.attr,
	&dev_attr_pwm_clock_freq_sel_raw.attr,
	&dev_attr_pwm_clock_src_sel_low_raw.attr,
	&dev_attr_pwm_clock_src_sel_high_raw.attr,
	&dev_attr_pwm_clock_gating_raw.attr,
	&dev_attr_pwm_clock_control_raw.attr,
	&dev_attr_pwm_group4_cpr_raw.attr,
	&dev_attr_pwm_group4_cpr_msb_raw.attr,
	&dev_attr_pwm_group6_cpr_raw.attr,
	&dev_attr_pwm_group6_cpr_msb_raw.attr,
	&dev_attr_pwm_group7_cpr_raw.attr,
	&dev_attr_pwm_group7_cpr_msb_raw.attr,
	&dev_attr_pwm_6mhz_mode_raw.attr,
	&dev_attr_pwm5_timeout_ctrl_raw.attr,
	&dev_attr_backlight_duty_raw.attr,
	&dev_attr_tach_zone_int_status_raw.attr,
	&dev_attr_tach_switch2_raw.attr,
	&dev_attr_tach_switch_raw.attr,
	&dev_attr_pwm_open_drain_raw.attr,
	&dev_attr_pwm1_led_enable_raw.attr,
	&dev_attr_pwm1_led_ctrl1_raw.attr,
	&dev_attr_pwm1_led_ctrl2_raw.attr,
	&dev_attr_pwm2_led_enable_raw.attr,
	&dev_attr_pwm2_led_ctrl1_raw.attr,
	&dev_attr_pwm2_led_ctrl2_raw.attr,
	&dev_attr_pwm_load_counter_ctrl_raw.attr,
	NULL,
};

static const struct attribute_group it5570_extra_group = {
	.attrs = it5570_extra_attrs,
	.is_visible = it5570_extra_attr_is_visible,
};

static const struct attribute_group *it5570_extra_groups[] = {
	&it5570_extra_group,
	NULL,
};

static const char *it5570_hwmon_channel_name(enum hwmon_sensor_types type, int channel)
{
	if (type == hwmon_in && channel < ARRAY_SIZE(it5570_voltage_channels))
		return it5570_voltage_channels[channel].name;

	if (type == hwmon_fan && channel < ARRAY_SIZE(it5570_tach_channels))
		return it5570_tach_channels[channel].name;

	if (type == hwmon_pwm && channel < ARRAY_SIZE(it5570_pwm_channels))
		return it5570_pwm_channels[channel].name;

	return "unknown";
}

static void it5570_log_ec_reg_error(const struct it5570_hwmon_data *data,
					    const char *op, const char *class_name,
					    const char *channel_name, u16 reg,
					    enum it5570_reason_token reason, int ret)
{
	IT5570_LOG_WARN_RL(data->dev,
			   "stage=%s event=reg-error transport=%s ldn=0x%02x "
			   "class=%s channel=%s reg=0x%04x ret=%d reason=%s\n",
			   op, it5570_transport_name(data->transport_state.mode),
			   data->transport_state.ldn, class_name, channel_name, reg, ret,
			   it5570_reason_name(reason));
}

static int it5570_read8_logged(struct it5570_hwmon_data *data, const char *class_name,
				       const char *channel_name, u16 reg, u8 *value)
{
	enum it5570_reason_token reason;
	int ret;

	ret = it5570_transport_read8(data, reg, value);
	if (ret) {
		reason = it5570_read_reason_from_errno(ret);
		it5570_log_ec_reg_error(data, "read", class_name, channel_name, reg,
					       reason, ret);
	}

	return ret;
}

static int it5570_write8_logged(struct it5570_hwmon_data *data, const char *class_name,
					const char *channel_name, u16 reg, u8 value)
{
	enum it5570_reason_token reason;
	int ret;

	ret = it5570_transport_write8(data, reg, value);
	if (ret) {
		reason = it5570_write_reason_from_errno(ret);
		it5570_log_ec_reg_error(data, "write", class_name, channel_name, reg,
					       reason, ret);
	}

	return ret;
}

static umode_t it5570_pwm_visible_mode(const struct it5570_hwmon_data *data)
{
	if (data->transport_state.mode != IT5570_TRANSPORT_D2EC ||
	    !data->transport_state.ready)
		return 0;

	return 0444;
}

static umode_t it5570_hwmon_is_visible(const void *drvdata,
				      enum hwmon_sensor_types type,
				      u32 attr, int channel)
{
	const struct it5570_hwmon_data *data = drvdata;

	switch (type) {
	case hwmon_in:
		if (attr == hwmon_in_input && channel < ARRAY_SIZE(data->sensor.voltage) &&
		    data->sensor.voltage[channel].available)
			return 0444;
		break;
	case hwmon_fan:
		if (attr == hwmon_fan_input && channel < ARRAY_SIZE(data->sensor.tach) &&
		    data->sensor.tach[channel].available)
			return 0444;
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_input && channel < ARRAY_SIZE(it5570_pwm_channels))
			return it5570_pwm_visible_mode(data);
		break;
	default:
		break;
	}

	return 0;
}

static int it5570_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	struct it5570_hwmon_data *data = dev_get_drvdata(dev);
	int ret = -EOPNOTSUPP;

	if (!data || !val)
		return -EINVAL;

	mutex_lock(&data->sensor.lock);

	switch (type) {
	case hwmon_in:
		if (attr == hwmon_in_input && channel < ARRAY_SIZE(data->sensor.voltage) &&
		    data->sensor.voltage[channel].available) {
			ret = it5570_read_voltage_channel(data,
						 &it5570_voltage_channels[channel], val);
			if (!ret)
				data->sensor.voltage[channel].value = *val;
			else if (ret == -EAGAIN) {
				*val = data->sensor.voltage[channel].value;
				ret = 0;
			}
			break;
		}
		ret = -EOPNOTSUPP;
		break;
	case hwmon_fan:
		if (attr == hwmon_fan_input && channel < ARRAY_SIZE(data->sensor.tach) &&
		    data->sensor.tach[channel].available) {
			ret = it5570_read_tach_channel(data,
					      &it5570_tach_channels[channel], val);
			if (!ret)
				data->sensor.tach[channel].value = *val;
			break;
		}
		ret = -EOPNOTSUPP;
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_input && channel < ARRAY_SIZE(it5570_pwm_channels)) {
			ret = it5570_read_pwm_hwmon(data, &it5570_pwm_channels[channel], val);
			break;
		}
		ret = -EOPNOTSUPP;
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	mutex_unlock(&data->sensor.lock);

	if (ret) {
		const char *class_name = type == hwmon_in ? "in" :
			(type == hwmon_fan ? "tach" :
			 (type == hwmon_pwm ? "pwm" : "hwmon"));

		it5570_log_io_error(data, "read", class_name,
				    it5570_hwmon_channel_name(type, channel),
				    it5570_read_reason_from_errno(ret));
	}

	return ret;
}

static int it5570_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long val)
{
	return -EOPNOTSUPP;
}

static const struct hwmon_ops it5570_hwmon_ops = {
	.is_visible = it5570_hwmon_is_visible,
	.read = it5570_hwmon_read,
	.write = it5570_hwmon_write,
};

static const struct hwmon_chip_info it5570_hwmon_chip_info = {
	.ops = &it5570_hwmon_ops,
	.info = it5570_hwmon_channel_info,
};

int it5570_register_hwmon_device(struct platform_device *pdev,
				 struct it5570_hwmon_data *data)
{
	data->sensor.hwmon_dev = devm_hwmon_device_register_with_info(
		&pdev->dev, IT5570_HWMON_NAME, data, &it5570_hwmon_chip_info,
		it5570_extra_groups);
	if (IS_ERR(data->sensor.hwmon_dev))
		return PTR_ERR(data->sensor.hwmon_dev);

	return 0;
}

static int it5570_transport_read16(struct it5570_hwmon_data *data,
				   const struct it5570_ec_reg_span *span,
				   const char *class_name,
				   const char *channel_name,
				   u16 *value, u8 *status)
{
	u8 lsb;
	u8 msb;
	int ret;

	ret = it5570_read8_logged(data, class_name, channel_name, span->lsb_reg, &lsb);
	if (ret)
		return ret;

	ret = it5570_read8_logged(data, class_name, channel_name, span->msb_reg, &msb);
	if (ret)
		return ret;

	if (span->status_reg) {
		ret = it5570_read8_logged(data, class_name, channel_name,
					       span->status_reg, status);
		if (ret)
			return ret;
	} else {
		*status = 0;
	}

	*value = ((u16)msb << 8) | lsb;

	return 0;
}

static int it5570_clear_voltage_channel_status(struct it5570_hwmon_data *data,
						 const struct it5570_channel_desc *channel,
						 const struct it5570_ec_reg_span *span)
{
	if (!span->status_reg || !span->status_mask)
		return 0;

	/* ADCDVSTS is R/WC; write only this channel bit after consuming the sample. */
	return it5570_write8_logged(data, "in", channel->name, span->status_reg,
					  span->status_mask);
}

static int it5570_read_voltage_channel(struct it5570_hwmon_data *data,
				      const struct it5570_channel_desc *channel,
				      long *value)
{
	const struct it5570_ec_reg_span *span;
	u16 first;
	u16 second;
	u8 first_status;
	u8 second_status;
	int ret;
	int attempt;

	if (!channel || channel->index >= ARRAY_SIZE(it5570_voltage_reg_spans))
		return -EINVAL;

	if (!data->transport_state.ready)
		return -ENODEV;

	span = &it5570_voltage_reg_spans[channel->index];
	for (attempt = 0; attempt < IT5570_ADC_READ_RETRIES; attempt++) {
		ret = it5570_transport_read16(data, span, "in", channel->name,
					       &first, &first_status);
		if (ret)
			return ret;

		ret = it5570_transport_read16(data, span, "in", channel->name,
					       &second, &second_status);
		if (ret)
			return ret;

		/*
		 * ADCDVSTS reports which voltage-channel data buffers contain
		 * a valid conversion. The EC can briefly clear a bit while a new
		 * conversion is being latched, so require two valid reads but
		 * retry a few times before treating the channel as not ready.
		 */
		if ((first_status & span->status_mask) &&
		    (second_status & span->status_mask))
			break;
	}

	if (attempt == IT5570_ADC_READ_RETRIES)
		return -EAGAIN;

	/*
	 * VCHnDATM contributes bits 9..8 and VCHnDATL contributes bits 7..0
	 * of the 10-bit ADC sample. Report millivolts using the documented
	 * default 3.0 V ADC reference.
	 */
	second &= IT5570_ADC_RAW_MASK;
	*value = DIV_ROUND_CLOSEST((unsigned long)second *
				   IT5570_ADC_DEFAULT_REF_MV,
				   IT5570_ADC_RAW_MASK);

	return it5570_clear_voltage_channel_status(data, channel, span);
}

static int it5570_read_tach_raw_channel(struct it5570_hwmon_data *data,
				      const struct it5570_channel_desc *channel,
				      u16 *value)
{
	const struct it5570_ec_reg_span *span;
	u16 first;
	u16 second;
	u8 status;
	int ret;

	if (!channel || !channel->index || channel->index > ARRAY_SIZE(it5570_tach_reg_spans))
		return -EINVAL;

	if (!data->transport_state.ready)
		return -ENODEV;

	span = &it5570_tach_reg_spans[channel->index - 1];
	ret = it5570_transport_read16(data, span, "tach", channel->name,
					       &first, &status);
	if (ret)
		return ret;

	ret = it5570_transport_read16(data, span, "tach", channel->name,
					       &second, &status);
	if (ret)
		return ret;

	/* D2EC/I2EC tach pair reads are not atomic; zero is still a valid stopped-fan count. */
	(void)first;

	*value = second;

	return 0;
}

static int it5570_read_tach_channel(struct it5570_hwmon_data *data,
				   const struct it5570_channel_desc *channel,
				   long *value)
{
	u16 raw;
	int ret;

	ret = it5570_read_tach_raw_channel(data, channel, &raw);
	if (ret)
		return ret;

	/*
	 * IT5570 datasheet 7.12.3.2 Manual Fan Control Mode:
	 *   RPM = 60 * (FreqEC / 128) / (tach_count * P)
	 * and tach_count 0 means the fan is stopped.
	 */
	if (!raw) {
		*value = 0;
		return 0;
	}

	*value = DIV_ROUND_CLOSEST(IT5570_TACH_RPM_NUMERATOR, raw);

	return 0;
}

void it5570_sensor_state_reset(struct it5570_hwmon_data *data)
{
	int i;

	data->sensor.prepared = false;
	data->sensor.readable_voltage_count = 0;
	data->sensor.readable_tach_count = 0;
	data->sensor.hwmon_dev = NULL;

	for (i = 0; i < ARRAY_SIZE(data->sensor.voltage); i++) {
		data->sensor.voltage[i].available = false;
		data->sensor.voltage[i].value = 0;
	}

	for (i = 0; i < ARRAY_SIZE(data->sensor.tach); i++) {
		data->sensor.tach[i].available = false;
		data->sensor.tach[i].value = 0;
	}
}

enum it5570_reason_token it5570_read_reason_from_errno(int ret)
{
	switch (ret) {
	case -ENODEV:
		return IT5570_REASON_READ_NOT_READY;
	case -EAGAIN:
		return IT5570_REASON_CHANNEL_NOT_READY;
	case -ENODATA:
		return IT5570_REASON_CHANNEL_NO_DATA;
	case -EOPNOTSUPP:
		return IT5570_REASON_TRANSPORT_UNSUPPORTED;
	case -EINVAL:
		return IT5570_REASON_UNSUPPORTED_CHANNEL;
	case -EIO:
		return IT5570_REASON_REG_ACCESS_FAILED;
	case -EACCES:
	case -EPERM:
		return IT5570_REASON_PERMISSION_DENIED;
	default:
		return IT5570_REASON_REG_ACCESS_FAILED;
	}
}

enum it5570_reason_token it5570_write_reason_from_errno(int ret)
{
	return it5570_read_reason_from_errno(ret);
}

static bool it5570_channel_probe_absent(int ret)
{
	return ret == -EAGAIN || ret == -ENODATA;
}

int it5570_prepare_sensor_hwmon(struct it5570_hwmon_data *data)
{
	bool pwm_visible = data->transport_state.mode == IT5570_TRANSPORT_D2EC &&
			   data->transport_state.ready;
	int i;

	mutex_lock(&data->sensor.lock);
	it5570_sensor_state_reset(data);

	for (i = 0; i < ARRAY_SIZE(it5570_voltage_channels); i++) {
		long value;
		int ret;

		ret = it5570_read_voltage_channel(data, &it5570_voltage_channels[i], &value);
		if (ret) {
			if (!it5570_channel_probe_absent(ret))
				it5570_log_io_error(data, "read", "in",
						    it5570_voltage_channels[i].name,
						    it5570_read_reason_from_errno(ret));
			continue;
		}

		data->sensor.voltage[i].available = true;
		data->sensor.voltage[i].value = value;
		data->sensor.readable_voltage_count++;
	}

	for (i = 0; i < ARRAY_SIZE(it5570_tach_channels); i++) {
		long value;
		int ret;

		ret = it5570_read_tach_channel(data, &it5570_tach_channels[i], &value);
		if (ret) {
			if (!it5570_channel_probe_absent(ret))
				it5570_log_io_error(data, "read", "tach",
						    it5570_tach_channels[i].name,
						    it5570_read_reason_from_errno(ret));
			continue;
		}

		data->sensor.tach[i].available = true;
		data->sensor.tach[i].value = value;
		data->sensor.readable_tach_count++;
	}

	data->sensor.prepared = data->sensor.readable_voltage_count ||
				  data->sensor.readable_tach_count ||
				  pwm_visible;
	mutex_unlock(&data->sensor.lock);

	if (!data->sensor.prepared)
		return -ENODEV;

	return 0;
}

struct it5570_pwm_runtime_cfg {
	u8 selector;
	bool ctrmode;
	bool duty_msb_enabled;
};

static bool it5570_pwm_channel_has_duty_msb(const struct it5570_channel_desc *channel)
{
	return channel && (channel->index == 3 || channel->index == 4);
}

static u16 it5570_pwm_channel_duty_msb_reg(const struct it5570_channel_desc *channel)
{
	return channel->index == 3 ? IT5570_EC_REG_PWM_DCR2M : IT5570_EC_REG_PWM_DCR3M;
}

static u8 it5570_pwm_channel_polarity_mask(const struct it5570_channel_desc *channel)
{
	return BIT(channel->index - 1);
}

static int it5570_read_pwm_runtime_cfg(struct it5570_hwmon_data *data,
				       const struct it5570_channel_desc *channel,
				       struct it5570_pwm_runtime_cfg *cfg)
{
	u8 clk6msel;
	u8 pcss;
	u8 shift;
	u16 reg;
	int ret;

	if (!channel || !channel->index || channel->index > ARRAY_SIZE(it5570_pwm_channels) ||
	    !cfg)
		return -EINVAL;

	reg = channel->index <= 4 ? IT5570_EC_REG_PWM_PCSSGL : IT5570_EC_REG_PWM_PCSSGH;
	shift = ((channel->index - 1) % 4) * 2;

	ret = it5570_read8_logged(data, "pwm", channel->name, reg, &pcss);
	if (ret)
		return ret;

	ret = it5570_read8_logged(data, "pwm", channel->name,
					 IT5570_EC_REG_PWM_CLK6MSEL, &clk6msel);
	if (ret)
		return ret;

	cfg->selector = (pcss >> shift) & IT5570_PWM_GROUP_SELECT_MASK;
	cfg->ctrmode = !!(clk6msel & BIT(4));
	cfg->duty_msb_enabled = cfg->ctrmode && cfg->selector == 1 &&
		it5570_pwm_channel_has_duty_msb(channel);

	return 0;
}

static int it5570_read_pwm_cycle_raw(struct it5570_hwmon_data *data,
				     const struct it5570_channel_desc *channel,
				     const struct it5570_pwm_runtime_cfg *cfg,
				     u16 *cycle_raw)
{
	u8 ctr1m;
	u8 ctr;
	int ret;

	if (!cfg || !cycle_raw)
		return -EINVAL;

	if (!cfg->ctrmode || !cfg->selector) {
		/* PCSSG selects both the prescaler group and the active CTR source. */
		ret = it5570_read8_logged(data, "pwm", channel->name,
					 IT5570_EC_REG_PWM_CTR0, &ctr);
		if (ret)
			return ret;

		*cycle_raw = ctr;
		return 0;
	}

	switch (cfg->selector) {
	case 1:
		ret = it5570_read8_logged(data, "pwm", channel->name,
					 IT5570_EC_REG_PWM_CTR1, &ctr);
		if (ret)
			return ret;

		ret = it5570_read8_logged(data, "pwm", channel->name,
					 IT5570_EC_REG_PWM_CTR1M, &ctr1m);
		if (ret)
			return ret;

		*cycle_raw = ((u16)(ctr1m & IT5570_PWM1M_EXT_MASK) << 8) | ctr;
		break;
	case 2:
		ret = it5570_read8_logged(data, "pwm", channel->name,
					 IT5570_EC_REG_PWM_CTR2, &ctr);
		if (ret)
			return ret;

		*cycle_raw = ctr;
		break;
	default:
		ret = it5570_read8_logged(data, "pwm", channel->name,
					 IT5570_EC_REG_PWM_CTR3, &ctr);
		if (ret)
			return ret;

		*cycle_raw = ctr;
		break;
	}

	return 0;
}

static int it5570_read_pwm_duty_raw(struct it5570_hwmon_data *data,
				    const struct it5570_channel_desc *channel,
				    const struct it5570_pwm_runtime_cfg *cfg,
				    u16 *duty_raw)
{
	u8 duty;
	u8 duty_msb;
	int ret;

	if (!channel || !cfg || !duty_raw || !channel->index ||
	    channel->index > ARRAY_SIZE(it5570_pwm_duty_regs))
		return -EINVAL;

	ret = it5570_read8_logged(data, "pwm", channel->name,
					 it5570_pwm_duty_regs[channel->index - 1], &duty);
	if (ret)
		return ret;

	*duty_raw = duty;

	if (!cfg->duty_msb_enabled)
		return 0;

	ret = it5570_read8_logged(data, "pwm", channel->name,
					 it5570_pwm_channel_duty_msb_reg(channel), &duty_msb);
	if (ret)
		return ret;

	*duty_raw |= (u16)(duty_msb & IT5570_PWM1M_EXT_MASK) << 8;

	return 0;
}

static int it5570_pwm_channel_is_inverted(struct it5570_hwmon_data *data,
					const struct it5570_channel_desc *channel,
					bool *inverted)
{
	u8 polarity;
	int ret;

	if (!channel || !inverted)
		return -EINVAL;

	ret = it5570_read8_logged(data, "pwm", channel->name,
					 IT5570_EC_REG_PWM_POLARITY, &polarity);
	if (ret)
		return ret;

	*inverted = !!(polarity & it5570_pwm_channel_polarity_mask(channel));
	return 0;
}

static int it5570_read_pwm_hwmon(struct it5570_hwmon_data *data,
				 const struct it5570_channel_desc *channel,
				 long *value)
{
	struct it5570_pwm_runtime_cfg cfg;
	bool inverted;
	u16 cycle_raw;
	u16 duty_raw;
	int ret;

	if (!value)
		return -EINVAL;

	ret = it5570_read_pwm_runtime_cfg(data, channel, &cfg);
	if (ret)
		return ret;

	ret = it5570_read_pwm_duty_raw(data, channel, &cfg, &duty_raw);
	if (ret)
		return ret;

	ret = it5570_read_pwm_cycle_raw(data, channel, &cfg, &cycle_raw);
	if (ret)
		return ret;

	ret = it5570_pwm_channel_is_inverted(data, channel, &inverted);
	if (ret)
		return ret;

	if (!duty_raw || duty_raw > cycle_raw) {
		/* DCR > CTR holds the output low per the PWM timing definition. */
		*value = 0;
	} else if (duty_raw == cycle_raw) {
		/* DCR == CTR holds the output high, which maps to full-scale duty. */
		*value = IT5570_HWMON_PWM_MAX;
	} else {
		*value = DIV_ROUND_CLOSEST((unsigned long)duty_raw * IT5570_HWMON_PWM_MAX,
					   (unsigned long)cycle_raw + 1);
	}

	/* Export effective duty semantics even when PWMPOL inverts the raw output. */
	if (inverted)
		*value = IT5570_HWMON_PWM_MAX - *value;

	return 0;
}

