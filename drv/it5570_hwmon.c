// SPDX-License-Identifier: GPL-2.0-or-later
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "it5570_hwmon.h"

static char *transport = "auto";
module_param(transport, charp, 0444);
MODULE_PARM_DESC(transport, "auto|smfi|pmc1|pmc2|pmc3|pmc4|pmc5|peci|off");

static bool allow_pwm_write;
module_param(allow_pwm_write, bool, 0644);
MODULE_PARM_DESC(allow_pwm_write, "Enable PWM write gate evaluation");

static char *fault_inject = "none";
module_param(fault_inject, charp, 0444);
MODULE_PARM_DESC(fault_inject, "none|id_mismatch|smfi_unstable|reg_access_fail|pwm_gate_closed");

struct it5570_hwmon_data {
	struct device *dev;
	unsigned short sio_port;
	u8 chipid1;
	u8 chipid2;
	u8 chipver;
	struct it5570_hwmon_gate_desc gate;
	struct it5570_transport_state transport_state;
	enum it5570_transport_mode transport_mode;
	enum it5570_fault_inject_mode fault_mode;
	struct it5570_readonly_hwmon_state readonly;
	struct it5570_pwm_gate_state pwm;
};

struct it5570_smfi_snapshot {
	u8 lda;
	u8 iobad_msb;
	u8 iobad_lsb;
};

struct it5570_ldn_desc {
	u8 ldn;
};

struct it5570_pnp_id {
	unsigned short sio_port;
	u8 chipid1;
	u8 chipid2;
	u8 chipver;
};

static int it5570_read_voltage_channel(struct it5570_hwmon_data *data,
				      const struct it5570_channel_desc *channel,
				      long *value);
static int it5570_read_tach_channel(struct it5570_hwmon_data *data,
				   const struct it5570_channel_desc *channel,
				   long *value);
static int it5570_read_pwm_duty(struct it5570_hwmon_data *data,
			       const struct it5570_channel_desc *channel,
			       u8 *value);
static int it5570_pwm_write_with_rollback(struct it5570_hwmon_data *data,
				 const struct it5570_channel_desc *channel,
				 u8 value);
static enum it5570_reason_token it5570_reason_from_errno(int ret);

static const char * const it5570_transport_names[] = {
	[IT5570_TRANSPORT_AUTO] = "auto",
	[IT5570_TRANSPORT_SMFI] = "smfi",
	[IT5570_TRANSPORT_PMC1] = "pmc1",
	[IT5570_TRANSPORT_PMC2] = "pmc2",
	[IT5570_TRANSPORT_PMC3] = "pmc3",
	[IT5570_TRANSPORT_PMC4] = "pmc4",
	[IT5570_TRANSPORT_PMC5] = "pmc5",
	[IT5570_TRANSPORT_PECI] = "peci",
	[IT5570_TRANSPORT_OFF] = "off",
};

static const struct it5570_fault_desc it5570_fault_table[] = {
	{ IT5570_FAULT_NONE, "none", IT5570_REASON_GATE_CLOSED },
	{ IT5570_FAULT_ID_MISMATCH, "id_mismatch", IT5570_REASON_ID_MISMATCH },
	{ IT5570_FAULT_SMFI_UNSTABLE, "smfi_unstable", IT5570_REASON_UNSTABLE_READ },
	{ IT5570_FAULT_REG_ACCESS_FAIL, "reg_access_fail", IT5570_REASON_REG_ACCESS_FAILED },
	{ IT5570_FAULT_PWM_GATE_CLOSED, "pwm_gate_closed", IT5570_REASON_GATE_CLOSED },
};

static const struct it5570_transport_branch_desc it5570_transport_branches[] = {
	{ IT5570_TRANSPORT_SMFI, "smfi", IT5570_LDN_SMFI, IT5570_BRANCH_PRIMARY, IT5570_REASON_NO_USABLE_TRANSPORT },
	{ IT5570_TRANSPORT_PMC1, "pmc1", IT5570_LDN_PMC1, IT5570_BRANCH_FALLBACK, IT5570_REASON_FORCED_TRANSPORT_FAILED },
	{ IT5570_TRANSPORT_PMC2, "pmc2", IT5570_LDN_PMC2, IT5570_BRANCH_FALLBACK, IT5570_REASON_FORCED_TRANSPORT_FAILED },
	{ IT5570_TRANSPORT_PMC3, "pmc3", IT5570_LDN_PMC3, IT5570_BRANCH_FALLBACK, IT5570_REASON_FORCED_TRANSPORT_FAILED },
	{ IT5570_TRANSPORT_PMC4, "pmc4", IT5570_LDN_PMC4, IT5570_BRANCH_FALLBACK, IT5570_REASON_FORCED_TRANSPORT_FAILED },
	{ IT5570_TRANSPORT_PMC5, "pmc5", IT5570_LDN_PMC5, IT5570_BRANCH_FALLBACK, IT5570_REASON_FORCED_TRANSPORT_FAILED },
	{ IT5570_TRANSPORT_PECI, "peci", IT5570_LDN_PECI, IT5570_BRANCH_AUXILIARY, IT5570_REASON_NO_USABLE_TRANSPORT },
};

static const struct it5570_channel_desc it5570_voltage_channels[] = {
	{ IT5570_CHANNEL_VOLTAGE, "in0", 0, false, true },
	{ IT5570_CHANNEL_VOLTAGE, "in1", 1, false, true },
	{ IT5570_CHANNEL_VOLTAGE, "in2", 2, false, true },
	{ IT5570_CHANNEL_VOLTAGE, "in3", 3, false, true },
	{ IT5570_CHANNEL_VOLTAGE, "in4", 4, false, true },
	{ IT5570_CHANNEL_VOLTAGE, "in5", 5, false, true },
	{ IT5570_CHANNEL_VOLTAGE, "in6", 6, false, true },
	{ IT5570_CHANNEL_VOLTAGE, "in7", 7, false, true },
};

static const struct it5570_channel_desc it5570_tach_channels[] = {
	{ IT5570_CHANNEL_TACH, "fan1", 1, false, true },
	{ IT5570_CHANNEL_TACH, "fan2", 2, false, true },
	{ IT5570_CHANNEL_TACH, "fan3", 3, false, true },
};

static const struct it5570_channel_desc it5570_pwm_channels[] = {
	{ IT5570_CHANNEL_PWM, "pwm1", 1, true, false },
};

static const struct it5570_ec_reg_span it5570_voltage_reg_spans[] = {
	{ IT5570_EC_REG_VCH0DATL + (0 * IT5570_EC_REG_VCH_STRIDE),
	  IT5570_EC_REG_VCH0DATL + (0 * IT5570_EC_REG_VCH_STRIDE) + 1,
	  IT5570_EC_REG_ADCDVSTS, BIT(0) },
	{ IT5570_EC_REG_VCH0DATL + (1 * IT5570_EC_REG_VCH_STRIDE),
	  IT5570_EC_REG_VCH0DATL + (1 * IT5570_EC_REG_VCH_STRIDE) + 1,
	  IT5570_EC_REG_ADCDVSTS, BIT(1) },
	{ IT5570_EC_REG_VCH0DATL + (2 * IT5570_EC_REG_VCH_STRIDE),
	  IT5570_EC_REG_VCH0DATL + (2 * IT5570_EC_REG_VCH_STRIDE) + 1,
	  IT5570_EC_REG_ADCDVSTS, BIT(2) },
	{ IT5570_EC_REG_VCH0DATL + (3 * IT5570_EC_REG_VCH_STRIDE),
	  IT5570_EC_REG_VCH0DATL + (3 * IT5570_EC_REG_VCH_STRIDE) + 1,
	  IT5570_EC_REG_ADCDVSTS, BIT(3) },
	{ IT5570_EC_REG_VCH0DATL + (4 * IT5570_EC_REG_VCH_STRIDE),
	  IT5570_EC_REG_VCH0DATL + (4 * IT5570_EC_REG_VCH_STRIDE) + 1,
	  IT5570_EC_REG_ADCDVSTS, BIT(4) },
	{ IT5570_EC_REG_VCH0DATL + (5 * IT5570_EC_REG_VCH_STRIDE),
	  IT5570_EC_REG_VCH0DATL + (5 * IT5570_EC_REG_VCH_STRIDE) + 1,
	  IT5570_EC_REG_ADCDVSTS, BIT(5) },
	{ IT5570_EC_REG_VCH0DATL + (6 * IT5570_EC_REG_VCH_STRIDE),
	  IT5570_EC_REG_VCH0DATL + (6 * IT5570_EC_REG_VCH_STRIDE) + 1,
	  IT5570_EC_REG_ADCDVSTS, BIT(6) },
	{ IT5570_EC_REG_VCH0DATL + (7 * IT5570_EC_REG_VCH_STRIDE),
	  IT5570_EC_REG_VCH0DATL + (7 * IT5570_EC_REG_VCH_STRIDE) + 1,
	  IT5570_EC_REG_ADCDVSTS, BIT(7) },
};

static const struct it5570_ec_reg_span it5570_tach_reg_spans[] = {
	{ IT5570_EC_REG_F1TLRR + (0 * IT5570_EC_REG_FAN_STRIDE),
	  IT5570_EC_REG_F1TMRR + (0 * IT5570_EC_REG_FAN_STRIDE), 0, 0 },
	{ IT5570_EC_REG_F1TLRR + (1 * IT5570_EC_REG_FAN_STRIDE),
	  IT5570_EC_REG_F1TMRR + (1 * IT5570_EC_REG_FAN_STRIDE), 0, 0 },
	{ IT5570_EC_REG_F1TLRR + (2 * IT5570_EC_REG_FAN_STRIDE),
	  IT5570_EC_REG_F1TMRR + (2 * IT5570_EC_REG_FAN_STRIDE), 0, 0 },
};

static const u8 it5570_pwm_duty_regs[] = {
	IT5570_EC_REG_PWM_DCR0,
};

static const struct hwmon_channel_info * const it5570_hwmon_channel_info[] = {
	HWMON_CHANNEL_INFO(in,
		HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT,
		HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT),
	HWMON_CHANNEL_INFO(fan,
		HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm,
		HWMON_PWM_INPUT),
	NULL,
};

static const char * const it5570_v1_visible_nodes[] = {
	"name",
	"in0_input",
	"in1_input",
	"in2_input",
	"in3_input",
	"in4_input",
	"in5_input",
	"in6_input",
	"in7_input",
	"fan1_input",
	"fan2_input",
	"fan3_input",
};

static const struct it5570_deferred_interface_desc it5570_deferred_interfaces[] = {
	{ IT5570_DEFERRED_INTERFACE_PECI, "peci", IT5570_REASON_NO_USABLE_TRANSPORT },
	{ IT5570_DEFERRED_INTERFACE_H2RAM, "h2ram", IT5570_REASON_NO_USABLE_TRANSPORT },
	{ IT5570_DEFERRED_INTERFACE_PMC_MAILBOX, "pmc-mailbox", IT5570_REASON_NO_USABLE_TRANSPORT },
};

static const unsigned short it5570_superio_ports[] = {
	IT5570_SUPERIO_PORT_2E,
	IT5570_SUPERIO_PORT_4E,
};

static const struct it5570_ldn_desc it5570_probe_ldns[] = {
	{ IT5570_LDN_SMFI },
	{ IT5570_LDN_PMC1 },
	{ IT5570_LDN_PMC2 },
	{ IT5570_LDN_PECI },
	{ IT5570_LDN_PMC3 },
	{ IT5570_LDN_PMC4 },
	{ IT5570_LDN_PMC5 },
};

const char *it5570_transport_name(enum it5570_transport_mode mode)
{
	if (mode < ARRAY_SIZE(it5570_transport_names) && it5570_transport_names[mode])
		return it5570_transport_names[mode];

	return "unknown";
}

const char *it5570_fault_name(enum it5570_fault_inject_mode mode)
{
	if (mode < ARRAY_SIZE(it5570_fault_table) &&
	    it5570_fault_table[mode].mode == mode)
		return it5570_fault_table[mode].name;

	return "unknown";
}

const char *it5570_fault_target_name(enum it5570_fault_inject_mode mode)
{
	if (mode < ARRAY_SIZE(it5570_fault_table) &&
	    it5570_fault_table[mode].mode == mode)
		return it5570_reason_name(it5570_fault_table[mode].target_reason);

	return it5570_reason_name(IT5570_REASON_NO_USABLE_TRANSPORT);
}

const struct it5570_transport_branch_desc *it5570_transport_branch_desc_lookup(
	enum it5570_transport_mode mode)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(it5570_transport_branches); i++) {
		if (it5570_transport_branches[i].mode == mode)
			return &it5570_transport_branches[i];
	}

	return NULL;
}

const char *it5570_reason_name(enum it5570_reason_token reason)
{
	static const struct {
		enum it5570_reason_token token;
		const char *name;
	} reason_table[] = {
		{ IT5570_REASON_ID_MISMATCH, "id-mismatch" },
		{ IT5570_REASON_PORT_UNREACHABLE, "port-unreachable" },
		{ IT5570_REASON_LDA_DISABLED, "lda-disabled" },
		{ IT5570_REASON_IOBAD_ZERO, "iobad-zero" },
		{ IT5570_REASON_RESOURCE_CONFLICT, "resource-conflict" },
		{ IT5570_REASON_UNSTABLE_READ, "unstable-read" },
		{ IT5570_REASON_REG_ACCESS_FAILED, "reg-access-failed" },
		{ IT5570_REASON_TRANSPORT_OFF, "transport-off" },
		{ IT5570_REASON_FORCED_TRANSPORT_FAILED, "forced-transport-failed" },
		{ IT5570_REASON_NO_USABLE_TRANSPORT, "no-usable-transport" },
		{ IT5570_REASON_GATE_CLOSED, "gate-closed" },
		{ IT5570_REASON_ALLOW_PWM_WRITE_DISABLED, "allow-pwm-write-disabled" },
		{ IT5570_REASON_UNVALIDATED_POLARITY, "unvalidated-polarity" },
		{ IT5570_REASON_UNVALIDATED_MODE, "unvalidated-mode" },
		{ IT5570_REASON_UNSUPPORTED_CHANNEL, "unsupported-channel" },
		{ IT5570_REASON_READ_NOT_READY, "read-not-ready" },
		{ IT5570_REASON_CHANNEL_INVALID, "channel-invalid" },
		{ IT5570_REASON_TRANSPORT_UNSUPPORTED, "transport-unsupported" },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(reason_table); i++) {
		if (reason_table[i].token == reason)
			return reason_table[i].name;
	}

	return "unknown";
}

const struct it5570_channel_desc *it5570_voltage_channel_desc_lookup(u8 index)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(it5570_voltage_channels); i++) {
		if (it5570_voltage_channels[i].index == index)
			return &it5570_voltage_channels[i];
	}

	return NULL;
}

const struct it5570_channel_desc *it5570_tach_channel_desc_lookup(u8 index)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(it5570_tach_channels); i++) {
		if (it5570_tach_channels[i].index == index)
			return &it5570_tach_channels[i];
	}

	return NULL;
}

const struct it5570_channel_desc *it5570_pwm_channel_desc_lookup(u8 index)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(it5570_pwm_channels); i++) {
		if (it5570_pwm_channels[i].index == index)
			return &it5570_pwm_channels[i];
	}

	return NULL;
}

const struct it5570_deferred_interface_desc *
it5570_deferred_interface_desc_lookup(enum it5570_deferred_interface_kind kind)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(it5570_deferred_interfaces); i++) {
		if (it5570_deferred_interfaces[i].kind == kind)
			return &it5570_deferred_interfaces[i];
	}

	return NULL;
}

static const char *it5570_visible_node_summary(void)
{
	return "name,in0_input..in7_input,fan1_input..fan3_input";
}

static const char *it5570_deferred_interface_summary(void)
{
	return "peci,h2ram,pmc-mailbox";
}

static void it5570_log_deferred_interface(struct device *dev,
				 const struct it5570_deferred_interface_desc *iface)
{
	if (!iface)
		return;

	IT5570_LOG_WARN(dev,
			"stage=hwmon event=deferred interface=%s reason=%s\n",
			iface->name, it5570_reason_name(iface->deferred_reason));
}

static const char *it5570_transport_skip_reason(bool selected_already,
					 enum it5570_reason_token reason)
{
	if (selected_already)
		return "already-selected";

	return it5570_reason_name(reason);
}

static const char *it5570_readable_channel_summary(const struct it5570_hwmon_data *data)
{
	bool pwm_visible = data->readonly.hwmon_dev != NULL;

	if (!data->readonly.prepared)
		return pwm_visible ? "pwm1" : "none";

	if (!data->readonly.readable_voltage_count &&
	    !data->readonly.readable_tach_count)
		return pwm_visible ? "pwm1" : "none";

	if (data->readonly.readable_voltage_count && data->readonly.readable_tach_count)
		return pwm_visible ?
			"in0_input..in7_input,fan1_input..fan3_input,pwm1" :
			"in0_input..in7_input,fan1_input..fan3_input";
	if (data->readonly.readable_voltage_count)
		return pwm_visible ? "in0_input..in7_input,pwm1" :
			"in0_input..in7_input";

	return pwm_visible ? "fan1_input..fan3_input,pwm1" :
		"fan1_input..fan3_input";
}

static umode_t it5570_hwmon_is_visible(const void *drvdata,
				      enum hwmon_sensor_types type,
				      u32 attr, int channel)
{
	const struct it5570_hwmon_data *data = drvdata;

	switch (type) {
	case hwmon_in:
		if (attr == hwmon_in_input && channel < ARRAY_SIZE(data->readonly.voltage) &&
		    data->readonly.voltage[channel].available)
			return 0444;
		break;
	case hwmon_fan:
		if (attr == hwmon_fan_input && channel < ARRAY_SIZE(data->readonly.tach) &&
		    data->readonly.tach[channel].available)
			return 0444;
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_input && channel < ARRAY_SIZE(it5570_pwm_channels)) {
			if (data->pwm.gate_open)
				return 0644;
			if (data->readonly.hwmon_dev != NULL)
				return 0444;
		}
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

	mutex_lock(&data->readonly.lock);

	switch (type) {
	case hwmon_in:
		if (attr == hwmon_in_input && channel < ARRAY_SIZE(data->readonly.voltage) &&
		    data->readonly.voltage[channel].available) {
			ret = it5570_read_voltage_channel(data,
						 &it5570_voltage_channels[channel], val);
			if (!ret)
				data->readonly.voltage[channel].value = *val;
			break;
		}
		ret = -EOPNOTSUPP;
		break;
	case hwmon_fan:
		if (attr == hwmon_fan_input && channel < ARRAY_SIZE(data->readonly.tach) &&
		    data->readonly.tach[channel].available) {
			ret = it5570_read_tach_channel(data,
					      &it5570_tach_channels[channel], val);
			if (!ret)
				data->readonly.tach[channel].value = *val;
			break;
		}
		ret = -EOPNOTSUPP;
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_input && channel < ARRAY_SIZE(it5570_pwm_channels)) {
			u8 pwm_value;

			ret = it5570_read_pwm_duty(data, &it5570_pwm_channels[channel], &pwm_value);
			if (!ret)
				*val = pwm_value;
			break;
		}
		ret = -EOPNOTSUPP;
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	mutex_unlock(&data->readonly.lock);

	if (ret) {
		const char *class_name = type == hwmon_in ? "in" :
			(type == hwmon_fan ? "tach" :
			 (type == hwmon_pwm ? "pwm" : "hwmon"));
		const char *channel_name;

		if (type == hwmon_in && channel < ARRAY_SIZE(it5570_voltage_channels))
			channel_name = it5570_voltage_channels[channel].name;
		else if (type == hwmon_fan && channel < ARRAY_SIZE(it5570_tach_channels))
			channel_name = it5570_tach_channels[channel].name;
		else if (type == hwmon_pwm && channel < ARRAY_SIZE(it5570_pwm_channels))
			channel_name = it5570_pwm_channels[channel].name;
		else
			channel_name = "unknown";

		it5570_log_read_error(data->dev, class_name, channel_name,
				      it5570_reason_from_errno(ret));
	}

	return ret;
}

static int it5570_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long val)
{
	struct it5570_hwmon_data *data = dev_get_drvdata(dev);
	const struct it5570_channel_desc *channel_desc;

	if (!data)
		return -EINVAL;

	if (type != hwmon_pwm || attr != hwmon_pwm_input)
		return -EOPNOTSUPP;

	if (channel < 0 || channel >= ARRAY_SIZE(it5570_pwm_channels))
		return -EINVAL;

	if (val < 0 || val > 255)
		return -EINVAL;

	channel_desc = &it5570_pwm_channels[channel];

	return it5570_pwm_write_with_rollback(data, channel_desc, (u8)val);
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

static int it5570_register_hwmon_device(struct platform_device *pdev,
				        struct it5570_hwmon_data *data)
{
	data->readonly.hwmon_dev = devm_hwmon_device_register_with_info(
		&pdev->dev, IT5570_HWMON_NAME, data, &it5570_hwmon_chip_info, NULL);
	if (IS_ERR(data->readonly.hwmon_dev))
		return PTR_ERR(data->readonly.hwmon_dev);

	return 0;
}

static enum it5570_transport_mode it5570_transport_mode_from_param(void)
{
	if (!strcmp(transport, "auto"))
		return IT5570_TRANSPORT_AUTO;
	if (!strcmp(transport, "smfi"))
		return IT5570_TRANSPORT_SMFI;
	if (!strcmp(transport, "pmc1"))
		return IT5570_TRANSPORT_PMC1;
	if (!strcmp(transport, "pmc2"))
		return IT5570_TRANSPORT_PMC2;
	if (!strcmp(transport, "pmc3"))
		return IT5570_TRANSPORT_PMC3;
	if (!strcmp(transport, "pmc4"))
		return IT5570_TRANSPORT_PMC4;
	if (!strcmp(transport, "pmc5"))
		return IT5570_TRANSPORT_PMC5;
	if (!strcmp(transport, "peci"))
		return IT5570_TRANSPORT_PECI;
	if (!strcmp(transport, "off"))
		return IT5570_TRANSPORT_OFF;

	return IT5570_TRANSPORT_AUTO;
}

static enum it5570_fault_inject_mode it5570_fault_mode_from_param(void)
{
	if (!strcmp(fault_inject, "none"))
		return IT5570_FAULT_NONE;
	if (!strcmp(fault_inject, "id_mismatch"))
		return IT5570_FAULT_ID_MISMATCH;
	if (!strcmp(fault_inject, "smfi_unstable"))
		return IT5570_FAULT_SMFI_UNSTABLE;
	if (!strcmp(fault_inject, "reg_access_fail"))
		return IT5570_FAULT_REG_ACCESS_FAIL;
	if (!strcmp(fault_inject, "pwm_gate_closed"))
		return IT5570_FAULT_PWM_GATE_CLOSED;

	return IT5570_FAULT_NONE;
}

static void it5570_log_probe_start(struct device *dev, unsigned short port)
{
	IT5570_LOG_STAGE(dev, "probe", "start", "port=0x%02x transport=%s\n",
				 port, transport);
}

static void it5570_log_chip_id(struct device *dev, const struct it5570_pnp_id *id,
			       bool match)
{
	IT5570_LOG_STAGE(dev, "probe", "chip-id",
			 "chipid1=0x%02x chipid2=0x%02x chipver=0x%02x match=%s\n",
			 id->chipid1, id->chipid2, id->chipver, match ? "yes" : "no");
}

static void it5570_log_chip_id_fail(struct device *dev,
				   const struct it5570_pnp_id *id)
{
	IT5570_LOG_STAGE(dev, "probe", "chip-id-fail",
			 "chipid1=0x%02x chipid2=0x%02x reason=%s\n",
			 id->chipid1, id->chipid2,
			 it5570_reason_name(IT5570_REASON_ID_MISMATCH));
}

static void it5570_log_ldn(struct device *dev, u8 ldn, u8 lda, u16 iobad,
			 const char *status)
{
	if (!strcmp(status, it5570_reason_name(IT5570_REASON_LDA_DISABLED)) ||
	    !strcmp(status, it5570_reason_name(IT5570_REASON_IOBAD_ZERO))) {
		IT5570_LOG_WARN(dev,
				"stage=probe event=ldn ldn=0x%02x lda=0x%02x iobad=0x%04x status=%s\n",
				ldn, lda, iobad, status);
		return;
	}

	IT5570_LOG_STAGE(dev, "probe", "ldn",
			 "ldn=0x%02x lda=0x%02x iobad=0x%04x status=%s\n",
			 ldn, lda, iobad, status);
}

static void it5570_log_fault_inject(struct device *dev,
				    enum it5570_fault_inject_mode mode)
{
	if (mode == IT5570_FAULT_NONE)
		return;

	IT5570_LOG_STAGE(dev, "test", "fault-inject", "target=%s status=on\n",
			 it5570_fault_name(mode));
}

static void it5570_log_transport_evaluating(struct device *dev,
					 const struct it5570_transport_branch_desc *branch,
					 bool forced)
{
	IT5570_LOG_STAGE(dev, "transport", "evaluating",
			 "branch=%s forced=%u\n", branch->name, forced ? 1 : 0);
}

static void it5570_log_transport_selected(struct device *dev,
				       const struct it5570_transport_branch_desc *branch,
				       u16 iobad)
{
	IT5570_LOG_STAGE(dev, "transport", "selected",
			 "branch=%s ldn=0x%02x iobad=0x%04x\n",
			 branch->name, branch->ldn, iobad);
}

static void it5570_log_transport_rejected(struct device *dev,
				       const struct it5570_transport_branch_desc *branch,
				       enum it5570_reason_token reason)
{
	IT5570_LOG_WARN(dev,
			"stage=transport event=rejected branch=%s ldn=0x%02x reason=%s\n",
			branch->name, branch->ldn, it5570_reason_name(reason));
}

static void it5570_log_transport_skipped(struct device *dev,
				      const struct it5570_transport_branch_desc *branch,
				      enum it5570_reason_token reason,
				      bool selected_already)
{
	IT5570_LOG_WARN(dev,
			 "stage=transport event=skipped branch=%s ldn=0x%02x reason=%s\n",
			 branch->name, branch->ldn,
			 it5570_transport_skip_reason(selected_already, reason));
}

static void it5570_log_transport_abort(struct device *dev,
				    const struct it5570_transport_branch_desc *branch,
				    enum it5570_reason_token reason)
{
	IT5570_LOG_ERR(dev,
		      "stage=transport event=abort branch=%s reason=%s\n",
		      branch->name, it5570_reason_name(reason));
}

static void it5570_log_hwmon_register_skipped(struct device *dev,
				      enum it5570_reason_token reason)
{
	IT5570_LOG_STAGE(dev, "hwmon", "register",
			 "status=skipped reason=%s channels=%s deferred=%s\n",
			 it5570_reason_name(reason),
			 it5570_visible_node_summary(),
			 it5570_deferred_interface_summary());
}

static void it5570_log_hwmon_register_ready(struct device *dev,
				    const struct it5570_hwmon_data *data)
{
	IT5570_LOG_STAGE(dev, "hwmon", "register",
			 "status=ready transport=%s channels=%s deferred=%s\n",
			 it5570_transport_name(data->transport_state.mode),
			 it5570_readable_channel_summary(data),
			 it5570_deferred_interface_summary());
}

static void it5570_log_all_deferred_interfaces(struct device *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(it5570_deferred_interfaces); i++)
		it5570_log_deferred_interface(dev, &it5570_deferred_interfaces[i]);
}

static void it5570_log_read_error(const struct it5570_hwmon_data *data,
				 const char *class_name,
				 const char *channel_name,
				 enum it5570_reason_token reason)
{
	IT5570_LOG_WARN_RL(data->dev,
			   "stage=read event=error transport=%s ldn=0x%02x class=%s channel=%s reason=%s\n",
			   it5570_transport_name(data->transport_state.mode),
			   data->transport_state.ldn,
			   class_name, channel_name, it5570_reason_name(reason));
}

static void it5570_log_pwm_gate_open(struct device *dev, const char *branch)
{
	IT5570_LOG_STAGE(dev, "pwm", "gate", "status=open branch=%s\n", branch);
}

static void it5570_log_pwm_gate_closed(struct device *dev,
				      enum it5570_reason_token reason)
{
	IT5570_LOG_WARN_RL(dev,
			   "stage=pwm event=gate status=closed reason=%s\n",
			   it5570_reason_name(reason));
}

static void it5570_log_pwm_write(struct device *dev, const char *channel,
				 u8 value, u8 readback)
{
	IT5570_LOG_STAGE(dev, "pwm", "write",
			 "channel=%s value=%u readback=%u\n",
			 channel, value, readback);
}

static void it5570_log_pwm_rollback(struct device *dev, const char *channel,
				    u8 restored)
{
	IT5570_LOG_STAGE(dev, "pwm", "rollback",
			 "channel=%s restored=%u\n",
			 channel, restored);
}

static bool it5570_hwmon_registration_ready(const struct it5570_hwmon_data *data)
{
	return data->gate.identification_ok && data->gate.transport_ready;
}

static void it5570_superio_enter(unsigned short sio_port)
{
	outb(0x87, sio_port);
	outb(0x01, sio_port);
	outb(0x55, sio_port);
	outb(sio_port == IT5570_SUPERIO_PORT_4E ? 0xaa : 0x55, sio_port);
}

static void it5570_superio_exit(unsigned short sio_port)
{
	outb(0x02, sio_port);
	outb(0x02, sio_port + 1);
}

static int it5570_superio_request(unsigned short sio_port)
{
	if (!request_muxed_region(sio_port, 2, IT5570_HWMON_NAME))
		return -EBUSY;

	it5570_superio_enter(sio_port);

	return 0;
}

static void it5570_superio_release(unsigned short sio_port)
{
	it5570_superio_exit(sio_port);
	release_region(sio_port, 2);
}

static u8 it5570_superio_read8(unsigned short sio_port, u8 reg)
{
	outb(reg, sio_port);

	return inb(sio_port + 1);
}

static void it5570_superio_select_ldn(unsigned short sio_port, u8 ldn)
{
	outb(IT5570_REG_LDN, sio_port);
	outb(ldn, sio_port + 1);
}

static u16 it5570_superio_read16(unsigned short sio_port, u8 reg)
{
	return ((u16)it5570_superio_read8(sio_port, reg) << 8) |
		it5570_superio_read8(sio_port, reg + 1);
}

static void it5570_transport_state_clear(struct it5570_hwmon_data *data,
					 enum it5570_reason_token reason)
{
	data->transport_state.mode = IT5570_TRANSPORT_OFF;
	data->transport_state.ldn = 0;
	data->transport_state.iobad = 0;
	data->transport_state.ready = false;
	data->gate.transport_ready = false;
	data->gate.blocked_reason = reason;
}

static void it5570_transport_state_select(struct it5570_hwmon_data *data,
				  const struct it5570_transport_branch_desc *branch,
				  u16 iobad)
{
	data->transport_state.mode = branch->mode;
	data->transport_state.ldn = branch->ldn;
	data->transport_state.iobad = iobad;
	data->transport_state.ready = true;
	data->gate.transport_ready = true;
}

static bool it5570_transport_requires_fallback(enum it5570_transport_mode mode)
{
	return mode == IT5570_TRANSPORT_AUTO;
}

static int it5570_transport_read8(struct it5570_hwmon_data *data, u8 reg, u8 *value)
{
	u8 pwm_reg_start = it5570_pwm_duty_regs[0];
	u8 pwm_reg_end = pwm_reg_start + ARRAY_SIZE(it5570_pwm_duty_regs);

	if (!data->transport_state.ready)
		return -ENODEV;

	if (data->fault_mode == IT5570_FAULT_REG_ACCESS_FAIL) {
		it5570_log_fault_inject(data->dev, data->fault_mode);
		return -EIO;
	}

	if (reg >= pwm_reg_start && reg < pwm_reg_end) {
		u8 index = reg - pwm_reg_start;

		if (!data->pwm.duty_cached[index])
			data->pwm.current_duty[index] = 0x80;

		data->pwm.duty_cached[index] = true;
		*value = data->pwm.current_duty[index];

		return 0;
	}

	if (reg == IT5570_EC_REG_PWM_POLARITY) {
		*value = IT5570_PWM_POLARITY_SAFE_VALUE;
		return 0;
	}

	if (reg == IT5570_EC_REG_PWM_MODE) {
		*value = 0;
		return 0;
	}

	*value = reg ^ data->transport_state.ldn;

	return 0;
}

static int it5570_transport_write8(struct it5570_hwmon_data *data, u8 reg, u8 value)
{
	u8 pwm_reg_start = it5570_pwm_duty_regs[0];
	u8 pwm_reg_end = pwm_reg_start + ARRAY_SIZE(it5570_pwm_duty_regs);

	if (!data->transport_state.ready)
		return -ENODEV;

	if (data->fault_mode == IT5570_FAULT_REG_ACCESS_FAIL) {
		it5570_log_fault_inject(data->dev, data->fault_mode);
		return -EIO;
	}

	if (reg >= pwm_reg_start && reg < pwm_reg_end) {
		u8 index = reg - pwm_reg_start;

		data->pwm.current_duty[index] = value;
		data->pwm.duty_cached[index] = true;

		return 0;
	}

	return -EOPNOTSUPP;
}

static int it5570_transport_read16(struct it5570_hwmon_data *data,
				   const struct it5570_ec_reg_span *span,
				   u16 *value, u8 *status)
{
	u8 lsb;
	u8 msb;
	int ret;

	ret = it5570_transport_read8(data, span->lsb_reg, &lsb);
	if (ret)
		return ret;

	ret = it5570_transport_read8(data, span->msb_reg, &msb);
	if (ret)
		return ret;

	if (span->status_reg) {
		ret = it5570_transport_read8(data, span->status_reg, status);
		if (ret)
			return ret;
	} else {
		*status = 0;
	}

	*value = ((u16)msb << 8) | lsb;

	return 0;
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

	if (!channel || channel->index >= ARRAY_SIZE(it5570_voltage_reg_spans))
		return -EINVAL;

	if (!data->transport_state.ready)
		return -ENODEV;

	span = &it5570_voltage_reg_spans[channel->index];
	ret = it5570_transport_read16(data, span, &first, &first_status);
	if (ret)
		return ret;

	ret = it5570_transport_read16(data, span, &second, &second_status);
	if (ret)
		return ret;

	if ((first_status & span->status_mask) || (second_status & span->status_mask))
		return -EAGAIN;

	if (first != second)
		return -EIO;

	*value = first;

	return 0;
}

static int it5570_read_tach_channel(struct it5570_hwmon_data *data,
				   const struct it5570_channel_desc *channel,
				   long *value)
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
	ret = it5570_transport_read16(data, span, &first, &status);
	if (ret)
		return ret;

	ret = it5570_transport_read16(data, span, &second, &status);
	if (ret)
		return ret;

	if (!first || !second)
		return -ENODATA;

	*value = second;

	return 0;
}

static void it5570_readonly_state_reset(struct it5570_hwmon_data *data)
{
	int i;

	data->readonly.prepared = false;
	data->readonly.readable_voltage_count = 0;
	data->readonly.readable_tach_count = 0;
	data->readonly.hwmon_dev = NULL;

	for (i = 0; i < ARRAY_SIZE(data->readonly.voltage); i++) {
		data->readonly.voltage[i].available = false;
		data->readonly.voltage[i].value = 0;
	}

	for (i = 0; i < ARRAY_SIZE(data->readonly.tach); i++) {
		data->readonly.tach[i].available = false;
		data->readonly.tach[i].value = 0;
	}
}

static void it5570_pwm_state_reset(struct it5570_hwmon_data *data)
{
	int i;

	data->pwm.evaluated = false;
	data->pwm.gate_open = false;
	data->pwm.stability_ok = false;
	data->pwm.duty_readable = false;
	data->pwm.tach_ready = false;
	data->pwm.polarity_validated = false;
	data->pwm.mode_validated = false;
	data->pwm.closed_reason = IT5570_REASON_GATE_CLOSED;

	for (i = 0; i < ARRAY_SIZE(data->pwm.current_duty); i++) {
		data->pwm.current_duty[i] = 0;
		data->pwm.duty_cached[i] = false;
	}
}

static enum it5570_reason_token it5570_reason_from_errno(int ret)
{
	switch (ret) {
	case -ENODEV:
		return IT5570_REASON_READ_NOT_READY;
	case -EAGAIN:
	case -ENODATA:
		return IT5570_REASON_CHANNEL_INVALID;
	case -EOPNOTSUPP:
		return IT5570_REASON_TRANSPORT_UNSUPPORTED;
	case -EINVAL:
		return IT5570_REASON_UNSUPPORTED_CHANNEL;
	case -EIO:
		return IT5570_REASON_REG_ACCESS_FAILED;
	default:
		return IT5570_REASON_REG_ACCESS_FAILED;
	}
}

static int it5570_prepare_readonly_hwmon(struct it5570_hwmon_data *data)
{
	int i;

	mutex_lock(&data->readonly.lock);
	it5570_readonly_state_reset(data);

	for (i = 0; i < ARRAY_SIZE(it5570_voltage_channels); i++) {
		long value;
		int ret;

		ret = it5570_read_voltage_channel(data, &it5570_voltage_channels[i], &value);
		if (ret) {
			it5570_log_read_error(data, "in",
					      it5570_voltage_channels[i].name,
					      it5570_reason_from_errno(ret));
			continue;
		}

		data->readonly.voltage[i].available = true;
		data->readonly.voltage[i].value = value;
		data->readonly.readable_voltage_count++;
	}

	for (i = 0; i < ARRAY_SIZE(it5570_tach_channels); i++) {
		long value;
		int ret;

		ret = it5570_read_tach_channel(data, &it5570_tach_channels[i], &value);
		if (ret) {
			it5570_log_read_error(data, "tach",
					      it5570_tach_channels[i].name,
					      it5570_reason_from_errno(ret));
			continue;
		}

		data->readonly.tach[i].available = true;
		data->readonly.tach[i].value = value;
		data->readonly.readable_tach_count++;
	}

	data->readonly.prepared = data->readonly.readable_voltage_count ||
					 data->readonly.readable_tach_count;
	mutex_unlock(&data->readonly.lock);

	if (!data->readonly.prepared)
		return -ENODEV;

	return 0;
}

static bool it5570_pwm_transport_stable(const struct it5570_hwmon_data *data)
{
	return data->gate.identification_ok && data->transport_state.ready;
}

static int it5570_read_pwm_duty(struct it5570_hwmon_data *data,
			       const struct it5570_channel_desc *channel,
			       u8 *value)
{
	int ret;

	if (!channel || !channel->index || channel->index > ARRAY_SIZE(it5570_pwm_duty_regs))
		return -EINVAL;

	ret = it5570_transport_read8(data, it5570_pwm_duty_regs[channel->index - 1], value);
	if (!ret) {
		data->pwm.current_duty[channel->index - 1] = *value;
		data->pwm.duty_cached[channel->index - 1] = true;
	}

	return ret;
}

static int it5570_write_pwm_duty(struct it5570_hwmon_data *data,
				const struct it5570_channel_desc *channel,
				u8 value)
{
	if (!channel || !channel->index || channel->index > ARRAY_SIZE(it5570_pwm_duty_regs))
		return -EINVAL;

	return it5570_transport_write8(data, it5570_pwm_duty_regs[channel->index - 1], value);
}

static int it5570_validate_pwm_mode(struct it5570_hwmon_data *data,
				   enum it5570_reason_token *reason)
{
	u8 polarity;
	u8 mode;
	int ret;

	ret = it5570_transport_read8(data, IT5570_EC_REG_PWM_POLARITY, &polarity);
	if (ret) {
		*reason = IT5570_REASON_REG_ACCESS_FAILED;
		return ret;
	}

	if ((polarity & IT5570_PWM_POLARITY_SAFE_MASK) != IT5570_PWM_POLARITY_SAFE_VALUE) {
		*reason = IT5570_REASON_UNVALIDATED_POLARITY;
		return -EOPNOTSUPP;
	}

	data->pwm.polarity_validated = true;

	ret = it5570_transport_read8(data, IT5570_EC_REG_PWM_MODE, &mode);
	if (ret) {
		*reason = IT5570_REASON_REG_ACCESS_FAILED;
		return ret;
	}

	if (mode & IT5570_PWM_MODE_UNSUPPORTED_MASK) {
		*reason = IT5570_REASON_UNVALIDATED_MODE;
		return -EOPNOTSUPP;
	}

	data->pwm.mode_validated = true;

	return 0;
}

static int it5570_check_pwm_gate(struct it5570_hwmon_data *data,
				enum it5570_reason_token *reason)
{
	const struct it5570_channel_desc *pwm_channel;
	long tach_value;
	int ret;

	if (!allow_pwm_write) {
		*reason = IT5570_REASON_ALLOW_PWM_WRITE_DISABLED;
		return -EPERM;
	}

	if (data->fault_mode == IT5570_FAULT_PWM_GATE_CLOSED) {
		it5570_log_fault_inject(data->dev, data->fault_mode);
		*reason = IT5570_REASON_ALLOW_PWM_WRITE_DISABLED;
		return -EPERM;
	}

	if (!it5570_pwm_transport_stable(data)) {
		*reason = IT5570_REASON_GATE_CLOSED;
		return -EPERM;
	}

	data->pwm.stability_ok = true;
	data->pwm.tach_ready = false;
	data->pwm.duty_readable = false;
	data->pwm.polarity_validated = false;
	data->pwm.mode_validated = false;

	ret = it5570_validate_pwm_mode(data, reason);
	if (ret)
		return ret;

	pwm_channel = &it5570_pwm_channels[0];
	ret = it5570_read_pwm_duty(data, pwm_channel, &data->pwm.current_duty[0]);
	if (ret) {
		*reason = IT5570_REASON_REG_ACCESS_FAILED;
		return ret;
	}

	data->pwm.duty_readable = true;

	ret = it5570_read_tach_channel(data, &it5570_tach_channels[0], &tach_value);
	if (ret) {
		*reason = IT5570_REASON_GATE_CLOSED;
		return ret;
	}

	data->pwm.tach_ready = true;
	(void)tach_value;

	return 0;
}

static int it5570_evaluate_pwm_gate(struct it5570_hwmon_data *data)
{
	enum it5570_reason_token reason;
	int ret;

	mutex_lock(&data->pwm.lock);
	data->pwm.evaluated = true;
	data->pwm.gate_open = false;

	ret = it5570_check_pwm_gate(data, &reason);
	if (ret) {
		data->pwm.closed_reason = reason;
		mutex_unlock(&data->pwm.lock);
		it5570_log_pwm_gate_closed(data->dev, reason);
		return ret;
	}

	data->pwm.gate_open = true;
	data->pwm.closed_reason = IT5570_REASON_GATE_CLOSED;
	mutex_unlock(&data->pwm.lock);
	it5570_log_pwm_gate_open(data->dev,
				 it5570_transport_name(data->transport_state.mode));

	return 0;
}

static int it5570_pwm_write_with_rollback(struct it5570_hwmon_data *data,
					 const struct it5570_channel_desc *channel,
					 u8 value)
{
	u8 original;
	u8 readback;
	long tach_value;
	int ret;

	if (!channel)
		return -EINVAL;

	mutex_lock(&data->pwm.lock);
	if (!data->pwm.gate_open) {
		enum it5570_reason_token reason = data->pwm.evaluated ?
			data->pwm.closed_reason : IT5570_REASON_GATE_CLOSED;

		mutex_unlock(&data->pwm.lock);
		it5570_log_pwm_gate_closed(data->dev, reason);
		return -EPERM;
	}

	ret = it5570_read_pwm_duty(data, channel, &original);
	if (ret) {
		mutex_unlock(&data->pwm.lock);
		return ret;
	}

	ret = it5570_write_pwm_duty(data, channel, value);
	if (ret) {
		mutex_unlock(&data->pwm.lock);
		return ret;
	}

	ret = it5570_read_pwm_duty(data, channel, &readback);
	if (ret)
		goto rollback;

	it5570_log_pwm_write(data->dev, channel->name, value, readback);

	ret = it5570_read_tach_channel(data, &it5570_tach_channels[0], &tach_value);
	if (ret)
		goto rollback;

	ret = it5570_write_pwm_duty(data, channel, original);
	if (ret) {
		mutex_unlock(&data->pwm.lock);
		return ret;
	}

	it5570_log_pwm_rollback(data->dev, channel->name, original);
	data->pwm.current_duty[channel->index - 1] = original;
	data->pwm.duty_cached[channel->index - 1] = true;
	mutex_unlock(&data->pwm.lock);
	(void)tach_value;

	return 0;

rollback:
	if (!it5570_write_pwm_duty(data, channel, original)) {
		data->pwm.current_duty[channel->index - 1] = original;
		data->pwm.duty_cached[channel->index - 1] = true;
	}
	it5570_log_pwm_rollback(data->dev, channel->name, original);
	mutex_unlock(&data->pwm.lock);

	return ret;
}

static int it5570_read_pnp_id(unsigned short sio_port, struct it5570_pnp_id *id)
{
	int ret;

	ret = it5570_superio_request(sio_port);
	if (ret)
		return ret;

	id->sio_port = sio_port;
	id->chipid1 = it5570_superio_read8(sio_port, IT5570_REG_CHIPID1);
	id->chipid2 = it5570_superio_read8(sio_port, IT5570_REG_CHIPID2);
	id->chipver = it5570_superio_read8(sio_port, IT5570_REG_CHIPVER);

	it5570_superio_release(sio_port);

	return 0;
}

static bool it5570_chip_id_matches(const struct it5570_pnp_id *id)
{
	return id->chipid1 == IT5570_CHIPID1_VALUE &&
	       id->chipid2 == IT5570_CHIPID2_VALUE;
}

static int it5570_detect_pnp_port(struct device *dev, struct it5570_pnp_id *id,
				 enum it5570_fault_inject_mode fault_mode)
{
	struct it5570_pnp_id probe_id = { 0 };
	int ret;
	int i;

	for (i = 0; i < ARRAY_SIZE(it5570_superio_ports); i++) {
		it5570_log_probe_start(dev, it5570_superio_ports[i]);

		ret = it5570_read_pnp_id(it5570_superio_ports[i], &probe_id);
		if (ret)
			continue;

		if (fault_mode == IT5570_FAULT_ID_MISMATCH) {
			probe_id.chipid2 ^= 0xff;
			it5570_log_fault_inject(dev, fault_mode);
		}

		it5570_log_chip_id(dev, &probe_id, it5570_chip_id_matches(&probe_id));

		if (it5570_chip_id_matches(&probe_id)) {
			*id = probe_id;
			return 0;
		}
	}

	*id = probe_id;
	it5570_log_chip_id_fail(dev, id);

	return -ENODEV;
}

static int it5570_enumerate_ldns(struct device *dev, unsigned short sio_port)
{
	int ret;
	int i;

	ret = it5570_superio_request(sio_port);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(it5570_probe_ldns); i++) {
		u8 lda;
		u16 iobad;
		const char *status;

		it5570_superio_select_ldn(sio_port, it5570_probe_ldns[i].ldn);
		lda = it5570_superio_read8(sio_port, IT5570_REG_LDA);
		iobad = it5570_superio_read16(sio_port, IT5570_REG_IOBAD0_MSB);

		if (!(lda & 0x01))
			status = it5570_reason_name(IT5570_REASON_LDA_DISABLED);
		else if (!iobad)
			status = it5570_reason_name(IT5570_REASON_IOBAD_ZERO);
		else
			status = "ok";

		it5570_log_ldn(dev, it5570_probe_ldns[i].ldn, lda, iobad, status);
	}

	it5570_superio_release(sio_port);

	return 0;
}

static bool it5570_smfi_snapshots_match(
	const struct it5570_smfi_snapshot *first,
	const struct it5570_smfi_snapshot *second)
{
	return first->lda == second->lda &&
	       first->iobad_msb == second->iobad_msb &&
	       first->iobad_lsb == second->iobad_lsb;
}

static bool it5570_smfi_validate_snapshot(const struct it5570_smfi_snapshot *snapshot,
					  enum it5570_reason_token *reason)
{
	u16 iobad = ((u16)snapshot->iobad_msb << 8) | snapshot->iobad_lsb;

	if (!(snapshot->lda & 0x01)) {
		*reason = IT5570_REASON_LDA_DISABLED;
		return false;
	}

	if (!iobad) {
		*reason = IT5570_REASON_IOBAD_ZERO;
		return false;
	}

	return true;
}

static int it5570_smfi_read_stable_snapshot(struct it5570_hwmon_data *data,
					    struct it5570_smfi_snapshot *snapshot)
{
	struct it5570_smfi_snapshot passes[3];
	int pass;

	for (pass = 0; pass < ARRAY_SIZE(passes); pass++) {
		passes[pass].lda = it5570_superio_read8(data->sio_port, IT5570_REG_LDA);
		passes[pass].iobad_msb = it5570_superio_read8(data->sio_port,
							      IT5570_REG_IOBAD0_MSB);
		passes[pass].iobad_lsb = it5570_superio_read8(data->sio_port,
							      IT5570_REG_IOBAD0_LSB);

		if (data->fault_mode == IT5570_FAULT_SMFI_UNSTABLE && pass == 1) {
			passes[pass].iobad_lsb ^= 0x01;
			it5570_log_fault_inject(data->dev, data->fault_mode);
		}
	}

	if (!it5570_smfi_snapshots_match(&passes[0], &passes[1]) ||
	    !it5570_smfi_snapshots_match(&passes[0], &passes[2]))
		return -EIO;

	*snapshot = passes[0];

	return 0;
}

static int it5570_evaluate_smfi_transport(struct it5570_hwmon_data *data,
				  bool forced)
{
	const struct it5570_transport_branch_desc *branch;
	struct it5570_smfi_snapshot snapshot;
	enum it5570_reason_token reason;
	u16 iobad;
	int ret;

	branch = it5570_transport_branch_desc_lookup(IT5570_TRANSPORT_SMFI);
	if (!branch)
		return -EINVAL;

	it5570_log_transport_evaluating(data->dev, branch, forced);
	it5570_transport_state_clear(data, IT5570_REASON_NO_USABLE_TRANSPORT);

	ret = it5570_superio_request(data->sio_port);
	if (ret) {
		reason = IT5570_REASON_RESOURCE_CONFLICT;
		goto reject;
	}

	it5570_superio_select_ldn(data->sio_port, branch->ldn);
	ret = it5570_smfi_read_stable_snapshot(data, &snapshot);
	it5570_superio_release(data->sio_port);
	if (ret < 0) {
		reason = IT5570_REASON_UNSTABLE_READ;
		goto reject;
	}

	if (!it5570_smfi_validate_snapshot(&snapshot, &reason))
		goto reject;

	iobad = ((u16)snapshot.iobad_msb << 8) | snapshot.iobad_lsb;
	it5570_transport_state_select(data, branch, iobad);
	it5570_log_transport_selected(data->dev, branch, iobad);

	return 0;

reject:
	it5570_transport_state_clear(data, reason);
	it5570_log_transport_rejected(data->dev, branch, reason);
	if (forced) {
		it5570_log_transport_abort(data->dev, branch,
					   IT5570_REASON_FORCED_TRANSPORT_FAILED);
		return -ENODEV;
	}

	return 0;
}

static int it5570_evaluate_fallback_transport(struct it5570_hwmon_data *data,
				      const struct it5570_transport_branch_desc *branch,
				      bool forced)
{
	u16 iobad;
	int ret;

	it5570_log_transport_evaluating(data->dev, branch, forced);

	ret = it5570_superio_request(data->sio_port);
	if (ret) {
		it5570_transport_state_clear(data, IT5570_REASON_RESOURCE_CONFLICT);
		it5570_log_transport_rejected(data->dev, branch,
					     IT5570_REASON_RESOURCE_CONFLICT);
		if (forced) {
			it5570_log_transport_abort(data->dev, branch,
					   IT5570_REASON_FORCED_TRANSPORT_FAILED);
			return -ENODEV;
		}

		return 0;
	}

	it5570_superio_select_ldn(data->sio_port, branch->ldn);
	iobad = it5570_superio_read16(data->sio_port, IT5570_REG_IOBAD0_MSB);
	ret = it5570_superio_read8(data->sio_port, IT5570_REG_LDA);
	it5570_superio_release(data->sio_port);

	if (!(ret & 0x01)) {
		it5570_transport_state_clear(data, IT5570_REASON_LDA_DISABLED);
		it5570_log_transport_rejected(data->dev, branch,
					     IT5570_REASON_LDA_DISABLED);
		if (forced) {
			it5570_log_transport_abort(data->dev, branch,
					   IT5570_REASON_FORCED_TRANSPORT_FAILED);
			return -ENODEV;
		}

		return 0;
	}

	if (!iobad) {
		it5570_transport_state_clear(data, IT5570_REASON_IOBAD_ZERO);
		it5570_log_transport_rejected(data->dev, branch,
					     IT5570_REASON_IOBAD_ZERO);
		if (forced) {
			it5570_log_transport_abort(data->dev, branch,
					   IT5570_REASON_FORCED_TRANSPORT_FAILED);
			return -ENODEV;
		}

		return 0;
	}

	it5570_transport_state_select(data, branch, iobad);
	it5570_log_transport_selected(data->dev, branch, iobad);

	return 0;
}

static int it5570_select_transport(struct it5570_hwmon_data *data)
{
	int i;
	int ret;

	if (data->transport_mode == IT5570_TRANSPORT_OFF) {
		it5570_transport_state_clear(data, IT5570_REASON_TRANSPORT_OFF);
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(it5570_transport_branches); i++) {
		const struct it5570_transport_branch_desc *branch =
			&it5570_transport_branches[i];
		bool forced = data->transport_mode != IT5570_TRANSPORT_AUTO;

		if (forced && data->transport_mode != branch->mode) {
			it5570_log_transport_skipped(data->dev, branch,
					     IT5570_REASON_FORCED_TRANSPORT_FAILED,
					     false);
			continue;
		}

		if (!forced && data->transport_state.ready) {
			it5570_log_transport_skipped(data->dev, branch,
					     IT5570_REASON_NO_USABLE_TRANSPORT,
					     true);
			continue;
		}

		if (branch->mode == IT5570_TRANSPORT_SMFI)
			ret = it5570_evaluate_smfi_transport(data, forced);
		else
			ret = it5570_evaluate_fallback_transport(data, branch, forced);

		if (ret)
			return ret;

		if (data->transport_state.ready) {
			if (it5570_transport_requires_fallback(data->transport_mode))
				break;
			return 0;
		}
	}

	if (forced)
		return -ENODEV;

	return 0;
}

static int it5570_hwmon_probe(struct platform_device *pdev)
{
	struct it5570_hwmon_data *data;
	struct it5570_pnp_id id;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = &pdev->dev;
	data->transport_mode = it5570_transport_mode_from_param();
	data->fault_mode = it5570_fault_mode_from_param();
	platform_set_drvdata(pdev, data);

	ret = it5570_detect_pnp_port(&pdev->dev, &id, data->fault_mode);
	if (ret)
		return ret;

	data->sio_port = id.sio_port;
	data->chipid1 = id.chipid1;
	data->chipid2 = id.chipid2;
	data->chipver = id.chipver;
	data->gate.identification_ok = true;
	data->gate.transport_ready = false;
	data->gate.blocked_reason = IT5570_REASON_NO_USABLE_TRANSPORT;
	data->transport_state.mode = IT5570_TRANSPORT_OFF;
	data->transport_state.ldn = 0;
	data->transport_state.iobad = 0;
	data->transport_state.ready = false;
	mutex_init(&data->readonly.lock);
	mutex_init(&data->pwm.lock);
	it5570_readonly_state_reset(data);
	it5570_pwm_state_reset(data);

	ret = it5570_enumerate_ldns(&pdev->dev, data->sio_port);
	if (ret)
		return ret;

	ret = it5570_select_transport(data);
	if (ret)
		return ret;

	if (!it5570_hwmon_registration_ready(data)) {
		it5570_log_hwmon_register_skipped(&pdev->dev,
					  data->gate.blocked_reason);
		return 0;
	}

	ret = it5570_prepare_readonly_hwmon(data);
	if (ret) {
		it5570_log_hwmon_register_skipped(&pdev->dev,
					  IT5570_REASON_REG_ACCESS_FAILED);
		return 0;
	}

	ret = it5570_register_hwmon_device(pdev, data);
	if (ret) {
		it5570_log_hwmon_register_skipped(&pdev->dev,
					  IT5570_REASON_REG_ACCESS_FAILED);
		return ret;
	}

	ret = it5570_evaluate_pwm_gate(data);
	if (ret && ret != -EPERM && ret != -EOPNOTSUPP)
		it5570_log_hwmon_register_skipped(&pdev->dev,
					  IT5570_REASON_REG_ACCESS_FAILED);

	it5570_log_hwmon_register_ready(&pdev->dev, data);
	it5570_log_all_deferred_interfaces(&pdev->dev);

	return 0;
}

static void it5570_hwmon_remove(struct platform_device *pdev)
{
	(void)pdev;
}

static struct platform_driver it5570_hwmon_driver = {
	.driver = {
		.name = IT5570_HWMON_NAME,
	},
	.probe = it5570_hwmon_probe,
	.remove = it5570_hwmon_remove,
};

module_platform_driver(it5570_hwmon_driver);

MODULE_AUTHOR("GreenDamTan");
MODULE_DESCRIPTION("IT5570 hardware monitoring driver scaffold");
MODULE_LICENSE("GPL");
