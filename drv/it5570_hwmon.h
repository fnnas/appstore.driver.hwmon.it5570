/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IT5570_HWMON_H
#define IT5570_HWMON_H

#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/moduleparam.h>

#define IT5570_HWMON_NAME "it5570_hwmon"

#define IT5570_SUPERIO_PORT_2E 0x2e
#define IT5570_SUPERIO_PORT_4E 0x4e

#define IT5570_REG_LDN 0x07
#define IT5570_REG_CHIPID1 0x20
#define IT5570_REG_CHIPID2 0x21
#define IT5570_REG_CHIPVER 0x22
#define IT5570_REG_LDA 0x30
#define IT5570_REG_IOBAD0_MSB 0x60
#define IT5570_REG_IOBAD0_LSB 0x61

#define IT5570_ADC_CHANNEL_COUNT 8
#define IT5570_TACH_CHANNEL_COUNT 3
#define IT5570_PWM_CHANNEL_COUNT 1

#define IT5570_PWM_FIRST_TEST_VALUE 64

#define IT5570_EC_REG_ADCDVSTS 0x01
#define IT5570_EC_REG_VCH0DATL 0x10
#define IT5570_EC_REG_VCH_STRIDE 0x02
#define IT5570_EC_REG_F1TLRR 0x30
#define IT5570_EC_REG_F1TMRR 0x31
#define IT5570_EC_REG_FAN_STRIDE 0x02
#define IT5570_EC_REG_PWM_DCR0 0x40
#define IT5570_EC_REG_PWM_POLARITY 0x48
#define IT5570_EC_REG_PWM_MODE 0x49

#define IT5570_PWM_POLARITY_SAFE_MASK BIT(0)
#define IT5570_PWM_POLARITY_SAFE_VALUE 0
#define IT5570_PWM_MODE_UNSUPPORTED_MASK BIT(7)

#define IT5570_CHIPID1_VALUE 0x55
#define IT5570_CHIPID2_VALUE 0x70

#define IT5570_LDN_SMFI 0x0f
#define IT5570_LDN_PMC1 0x11
#define IT5570_LDN_PMC2 0x12
#define IT5570_LDN_PECI 0x14
#define IT5570_LDN_PMC3 0x17
#define IT5570_LDN_PMC4 0x18
#define IT5570_LDN_PMC5 0x19

#define IT5570_LOG_STAGE(_dev, _stage, _event, _fmt, ...) \
	dev_info((_dev), "stage=%s event=%s " _fmt, (_stage), (_event), ##__VA_ARGS__)
#define IT5570_LOG_INFO(_dev, _fmt, ...) \
	dev_info((_dev), _fmt, ##__VA_ARGS__)
#define IT5570_LOG_WARN(_dev, _fmt, ...) \
	dev_warn((_dev), _fmt, ##__VA_ARGS__)
#define IT5570_LOG_WARN_RL(_dev, _fmt, ...) \
	dev_warn_ratelimited((_dev), _fmt, ##__VA_ARGS__)
#define IT5570_LOG_ERR(_dev, _fmt, ...) \
	dev_err((_dev), _fmt, ##__VA_ARGS__)
#define IT5570_LOG_DBG(_dev, _fmt, ...) \
	dev_dbg((_dev), _fmt, ##__VA_ARGS__)

enum it5570_transport_mode {
	IT5570_TRANSPORT_AUTO = 0,
	IT5570_TRANSPORT_SMFI,
	IT5570_TRANSPORT_PMC1,
	IT5570_TRANSPORT_PMC2,
	IT5570_TRANSPORT_PMC3,
	IT5570_TRANSPORT_PMC4,
	IT5570_TRANSPORT_PMC5,
	IT5570_TRANSPORT_PECI,
	IT5570_TRANSPORT_OFF,
};

enum it5570_fault_inject_mode {
	IT5570_FAULT_NONE = 0,
	IT5570_FAULT_ID_MISMATCH,
	IT5570_FAULT_SMFI_UNSTABLE,
	IT5570_FAULT_REG_ACCESS_FAIL,
	IT5570_FAULT_PWM_GATE_CLOSED,
};

enum it5570_reason_token {
	IT5570_REASON_ID_MISMATCH = 0,
	IT5570_REASON_PORT_UNREACHABLE,
	IT5570_REASON_LDA_DISABLED,
	IT5570_REASON_IOBAD_ZERO,
	IT5570_REASON_RESOURCE_CONFLICT,
	IT5570_REASON_UNSTABLE_READ,
	IT5570_REASON_REG_ACCESS_FAILED,
	IT5570_REASON_TRANSPORT_OFF,
	IT5570_REASON_FORCED_TRANSPORT_FAILED,
	IT5570_REASON_NO_USABLE_TRANSPORT,
	IT5570_REASON_GATE_CLOSED,
	IT5570_REASON_ALLOW_PWM_WRITE_DISABLED,
	IT5570_REASON_UNVALIDATED_POLARITY,
	IT5570_REASON_UNVALIDATED_MODE,
	IT5570_REASON_UNSUPPORTED_CHANNEL,
	IT5570_REASON_READ_NOT_READY,
	IT5570_REASON_CHANNEL_INVALID,
	IT5570_REASON_TRANSPORT_UNSUPPORTED,
};

enum it5570_transport_branch_role {
	IT5570_BRANCH_PRIMARY = 0,
	IT5570_BRANCH_FALLBACK,
	IT5570_BRANCH_AUXILIARY,
};

enum it5570_channel_kind {
	IT5570_CHANNEL_VOLTAGE = 0,
	IT5570_CHANNEL_TACH,
	IT5570_CHANNEL_PWM,
};

enum it5570_hwmon_visible_group {
	IT5570_HWMON_VISIBLE_NAME = 0,
	IT5570_HWMON_VISIBLE_VOLTAGE,
	IT5570_HWMON_VISIBLE_TACH,
	IT5570_HWMON_VISIBLE_PWM_DEFERRED,
};

enum it5570_deferred_interface_kind {
	IT5570_DEFERRED_INTERFACE_PECI = 0,
	IT5570_DEFERRED_INTERFACE_H2RAM,
	IT5570_DEFERRED_INTERFACE_PMC_MAILBOX,
};

enum it5570_hwmon_registration_gate {
	IT5570_HWMON_GATE_IDENTIFICATION = 0,
	IT5570_HWMON_GATE_TRANSPORT_READY,
};

struct it5570_transport_branch_desc {
	enum it5570_transport_mode mode;
	const char *name;
	u8 ldn;
	enum it5570_transport_branch_role role;
	enum it5570_reason_token reject_reason;
};

struct it5570_channel_desc {
	enum it5570_channel_kind kind;
	const char *name;
	u8 index;
	bool writable_by_default;
	bool deferred;
};

struct it5570_deferred_interface_desc {
	enum it5570_deferred_interface_kind kind;
	const char *name;
	enum it5570_reason_token deferred_reason;
};

struct it5570_hwmon_gate_desc {
	bool identification_ok;
	bool transport_ready;
	enum it5570_reason_token blocked_reason;
};

struct it5570_transport_state {
	enum it5570_transport_mode mode;
	u8 ldn;
	u16 iobad;
	bool ready;
};

struct it5570_transport_skip_state {
	bool selected_already;
};

struct it5570_fault_desc {
	enum it5570_fault_inject_mode mode;
	const char *name;
	enum it5570_reason_token target_reason;
};

struct it5570_ec_reg_span {
	u8 lsb_reg;
	u8 msb_reg;
	u8 status_reg;
	u8 status_mask;
};

struct it5570_channel_sample {
	bool available;
	long value;
};

struct it5570_readonly_hwmon_state {
	struct mutex lock;
	struct it5570_channel_sample voltage[IT5570_ADC_CHANNEL_COUNT];
	struct it5570_channel_sample tach[IT5570_TACH_CHANNEL_COUNT];
	bool prepared;
	unsigned int readable_voltage_count;
	unsigned int readable_tach_count;
	struct device *hwmon_dev;
};

struct it5570_pwm_gate_state {
	struct mutex lock;
	bool evaluated;
	bool gate_open;
	bool stability_ok;
	bool duty_readable;
	bool tach_ready;
	bool polarity_validated;
	bool mode_validated;
	enum it5570_reason_token closed_reason;
	u8 current_duty[IT5570_PWM_CHANNEL_COUNT];
	bool duty_cached[IT5570_PWM_CHANNEL_COUNT];
};

const char *it5570_transport_name(enum it5570_transport_mode mode);
const struct it5570_transport_branch_desc *it5570_transport_branch_desc_lookup(
	enum it5570_transport_mode mode);
const char *it5570_fault_name(enum it5570_fault_inject_mode mode);
const char *it5570_fault_target_name(enum it5570_fault_inject_mode mode);
const char *it5570_reason_name(enum it5570_reason_token reason);
const struct it5570_channel_desc *it5570_voltage_channel_desc_lookup(u8 index);
const struct it5570_channel_desc *it5570_tach_channel_desc_lookup(u8 index);
const struct it5570_channel_desc *it5570_pwm_channel_desc_lookup(u8 index);
const struct it5570_deferred_interface_desc *it5570_deferred_interface_desc_lookup(
	enum it5570_deferred_interface_kind kind);

#endif /* IT5570_HWMON_H */
