/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IT5570_HWMON_H
#define IT5570_HWMON_H

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#ifndef IT5570_DRIVER_REVISION
#define IT5570_DRIVER_REVISION "none"
#endif

#define IT5570_HWMON_NAME "it5570_hwmon"

#define IT5570_SUPERIO_PORT_2E 0x2e
#define IT5570_SUPERIO_PORT_4E 0x4e

#define IT5570_REG_LDN 0x07
#define IT5570_REG_CHIPID1 0x20
#define IT5570_REG_CHIPID2 0x21
#define IT5570_REG_CHIPVER 0x22
#define IT5570_REG_SIOCTRL 0x23
#define IT5570_REG_LDA 0x30
#define IT5570_REG_IOBAD0_MSB 0x60
#define IT5570_REG_IOBAD0_LSB 0x61
#define IT5570_REG_D2ADR 0x2e
#define IT5570_REG_D2DAT 0x2f
#define IT5570_D2_I2EC_ADDR_L 0x10
#define IT5570_D2_I2EC_ADDR_H 0x11
#define IT5570_D2_I2EC_DATA 0x12

#define IT5570_ADC_CHANNEL_COUNT 8
#define IT5570_TACH_CHANNEL_COUNT 3
#define IT5570_PWM_CHANNEL_COUNT 8

#define IT5570_HWMON_PWM_MAX 255UL
#define IT5570_SIOCTRL_SIOEN BIT(0)
#define IT5570_PWM0_POLARITY_BIT BIT(0)
#define IT5570_PWM_GROUP_COUNT 4
#define IT5570_PWM_GROUP_SELECT_MASK 0x3
#define IT5570_PWM1M_EXT_MASK 0x03
#define IT5570_FREQEC_HZ 9200000UL
#define IT5570_TACH_SAMPLING_DIV 128UL
#define IT5570_TACH_PULSES_PER_REV 2UL
#define IT5570_TACH_RPM_NUMERATOR \
	((60UL * IT5570_FREQEC_HZ) / \
	 (IT5570_TACH_SAMPLING_DIV * IT5570_TACH_PULSES_PER_REV))
#define IT5570_ADC_DEFAULT_REF_MV 3000UL
#define IT5570_ADC_RAW_MASK 0x03ff
#define IT5570_ADC_READ_RETRIES 3

/*
 * EC RAM register windows from the IT5570 datasheet:
 *   - 7.11.4 ADC EC view: result/status registers are based at 0x1900.
 *   - 7.12.4 PWM EC view: PWM and fan tachometer registers are based at
 *     0x1800.
 *
 * D2EC accesses EC memory through SuperIO D2ADR/D2DAT. For I2EC over D2EC,
 * write the EC memory address to offsets 0x11/0x10, then read/write data
 * through offset 0x12.
 */
#define IT5570_EC_ADC_BASE 0x1900
#define IT5570_EC_PWM_BASE 0x1800

/*
 * ADC data buffer offsets follow the EC view map in datasheet 7.11.4:
 * VCH1..VCH3 appear first at 0x07..0x0e, VCH0 is at 0x18..0x19, and
 * VCH4..VCH7 are at 0x39..0x43. ADCDVSTS at 0x44 contains one valid-data
 * bit per voltage channel.
 */
#define IT5570_EC_REG_VCH1DATL (IT5570_EC_ADC_BASE + 0x07)
#define IT5570_EC_REG_VCH1DATM (IT5570_EC_ADC_BASE + 0x08)
#define IT5570_EC_REG_VCH2DATL (IT5570_EC_ADC_BASE + 0x0a)
#define IT5570_EC_REG_VCH2DATM (IT5570_EC_ADC_BASE + 0x0b)
#define IT5570_EC_REG_VCH3DATL (IT5570_EC_ADC_BASE + 0x0d)
#define IT5570_EC_REG_VCH3DATM (IT5570_EC_ADC_BASE + 0x0e)
#define IT5570_EC_REG_VCH0DATL (IT5570_EC_ADC_BASE + 0x18)
#define IT5570_EC_REG_VCH0DATM (IT5570_EC_ADC_BASE + 0x19)
#define IT5570_EC_REG_VCH4DATM (IT5570_EC_ADC_BASE + 0x39)
#define IT5570_EC_REG_VCH4DATL (IT5570_EC_ADC_BASE + 0x3a)
#define IT5570_EC_REG_VCH5DATM (IT5570_EC_ADC_BASE + 0x3c)
#define IT5570_EC_REG_VCH5DATL (IT5570_EC_ADC_BASE + 0x3d)
#define IT5570_EC_REG_VCH6DATM (IT5570_EC_ADC_BASE + 0x3f)
#define IT5570_EC_REG_VCH6DATL (IT5570_EC_ADC_BASE + 0x40)
#define IT5570_EC_REG_VCH7DATM (IT5570_EC_ADC_BASE + 0x42)
#define IT5570_EC_REG_VCH7DATL (IT5570_EC_ADC_BASE + 0x43)
#define IT5570_EC_REG_ADCDVSTS (IT5570_EC_ADC_BASE + 0x44)

/*
 * IT5570 PWM EC view is based at 0x1800. The PWM block provides eight duty
 * registers and a small set of shared cycle/prescaler groups that channels
 * select through PCSSGL/PCSSGH. Group 0 uses CTR0/C0CPRS directly; once CTR1,
 * CTR2 or CTR3 is written the block enters 4-CTR mode and channel select bits
 * may route channels to CTR1/2/3 instead. The driver exposes all eight duty
 * channels through hwmon pwm1..pwm8 while keeping mode/clock/group registers
 * as raw debug nodes.
 */
#define IT5570_EC_REG_PWM_C0CPRS (IT5570_EC_PWM_BASE + 0x00)
#define IT5570_EC_REG_PWM_CTR0 (IT5570_EC_PWM_BASE + 0x01)
#define IT5570_EC_REG_PWM_DCR0 (IT5570_EC_PWM_BASE + 0x02)
#define IT5570_EC_REG_PWM_DCR1 (IT5570_EC_PWM_BASE + 0x03)
#define IT5570_EC_REG_PWM_DCR2 (IT5570_EC_PWM_BASE + 0x04)
#define IT5570_EC_REG_PWM_DCR3 (IT5570_EC_PWM_BASE + 0x05)
#define IT5570_EC_REG_PWM_DCR4 (IT5570_EC_PWM_BASE + 0x06)
#define IT5570_EC_REG_PWM_DCR5 (IT5570_EC_PWM_BASE + 0x07)
#define IT5570_EC_REG_PWM_DCR6 (IT5570_EC_PWM_BASE + 0x08)
#define IT5570_EC_REG_PWM_DCR7 (IT5570_EC_PWM_BASE + 0x09)
#define IT5570_EC_REG_PWM_POLARITY (IT5570_EC_PWM_BASE + 0x0a)
#define IT5570_EC_REG_PWM_PCFSR (IT5570_EC_PWM_BASE + 0x0b)
#define IT5570_EC_REG_PWM_PCSSGL (IT5570_EC_PWM_BASE + 0x0c)
#define IT5570_EC_REG_PWM_PCSSGH (IT5570_EC_PWM_BASE + 0x0d)
#define IT5570_EC_REG_PWM_PCSGR (IT5570_EC_PWM_BASE + 0x0f)
#define IT5570_EC_REG_F1TLRR (IT5570_EC_PWM_BASE + 0x1e)
#define IT5570_EC_REG_F1TMRR (IT5570_EC_PWM_BASE + 0x1f)
#define IT5570_EC_REG_F2TLRR (IT5570_EC_PWM_BASE + 0x20)
#define IT5570_EC_REG_F2TMRR (IT5570_EC_PWM_BASE + 0x21)
#define IT5570_EC_REG_ZINTSCR (IT5570_EC_PWM_BASE + 0x22)
#define IT5570_EC_REG_PWM_ZTIER (IT5570_EC_PWM_BASE + 0x23)
#define IT5570_EC_REG_PWM_C4CPRS (IT5570_EC_PWM_BASE + 0x27)
#define IT5570_EC_REG_PWM_C4MCPRS (IT5570_EC_PWM_BASE + 0x28)
#define IT5570_EC_REG_PWM_C6CPRS (IT5570_EC_PWM_BASE + 0x2b)
#define IT5570_EC_REG_PWM_C6MCPRS (IT5570_EC_PWM_BASE + 0x2c)
#define IT5570_EC_REG_PWM_C7CPRS (IT5570_EC_PWM_BASE + 0x2d)
#define IT5570_EC_REG_PWM_C7MCPRS (IT5570_EC_PWM_BASE + 0x2e)
#define IT5570_EC_REG_PWM_CLK6MSEL (IT5570_EC_PWM_BASE + 0x40)
#define IT5570_EC_REG_PWM_CTR1 (IT5570_EC_PWM_BASE + 0x41)
#define IT5570_EC_REG_PWM_CTR2 (IT5570_EC_PWM_BASE + 0x42)
#define IT5570_EC_REG_PWM_CTR3 (IT5570_EC_PWM_BASE + 0x43)
#define IT5570_EC_REG_PWM5TOCTRL (IT5570_EC_PWM_BASE + 0x44)
#define IT5570_EC_REG_F3TLRR (IT5570_EC_PWM_BASE + 0x45)
#define IT5570_EC_REG_F3TMRR (IT5570_EC_PWM_BASE + 0x46)
#define IT5570_EC_REG_TSWCTLR2 (IT5570_EC_PWM_BASE + 0x4f)
#define IT5570_EC_REG_TSWCTLR (IT5570_EC_PWM_BASE + 0x48)
#define IT5570_EC_REG_PWMODENR (IT5570_EC_PWM_BASE + 0x49)
#define IT5570_EC_REG_BLDR (IT5570_EC_PWM_BASE + 0x4c)
#define IT5570_EC_REG_PWM0LHE (IT5570_EC_PWM_BASE + 0x50)
#define IT5570_EC_REG_PWM0LCR1 (IT5570_EC_PWM_BASE + 0x51)
#define IT5570_EC_REG_PWM0LCR2 (IT5570_EC_PWM_BASE + 0x52)
#define IT5570_EC_REG_PWM1LHE (IT5570_EC_PWM_BASE + 0x53)
#define IT5570_EC_REG_PWM1LCR1 (IT5570_EC_PWM_BASE + 0x54)
#define IT5570_EC_REG_PWM1LCR2 (IT5570_EC_PWM_BASE + 0x55)
#define IT5570_EC_REG_PWMLCCR (IT5570_EC_PWM_BASE + 0x5a)
#define IT5570_EC_REG_PWM_CTR1M (IT5570_EC_PWM_BASE + 0x5b)
#define IT5570_EC_REG_PWM_DCR2M (IT5570_EC_PWM_BASE + 0x5c)
#define IT5570_EC_REG_PWM_DCR3M (IT5570_EC_PWM_BASE + 0x5d)

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
	IT5570_TRANSPORT_D2EC,
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
	IT5570_FAULT_REG_ACCESS_FAIL,
};

enum it5570_reason_token {
	IT5570_REASON_ID_MISMATCH = 0,
	IT5570_REASON_PORT_UNREACHABLE,
	IT5570_REASON_LDA_DISABLED,
	IT5570_REASON_IOBAD_ZERO,
	IT5570_REASON_SIO_DISABLED,
	IT5570_REASON_RESOURCE_CONFLICT,
	IT5570_REASON_UNSTABLE_READ,
	IT5570_REASON_REG_ACCESS_FAILED,
	IT5570_REASON_TRANSPORT_OFF,
	IT5570_REASON_FORCED_TRANSPORT_FAILED,
	IT5570_REASON_NO_USABLE_TRANSPORT,
	IT5570_REASON_UNSUPPORTED_CHANNEL,
	IT5570_REASON_READ_NOT_READY,
	IT5570_REASON_CHANNEL_NO_DATA,
	IT5570_REASON_CHANNEL_NOT_READY,
	IT5570_REASON_TRANSPORT_UNSUPPORTED,
	IT5570_REASON_PERMISSION_DENIED,
};

enum it5570_transport_branch_role {
	IT5570_BRANCH_PRIMARY = 0,
	IT5570_BRANCH_FALLBACK,
	IT5570_BRANCH_AUXILIARY,
};

struct it5570_transport_branch_desc {
	enum it5570_transport_mode mode;
	const char *name;
	u8 ldn;
	enum it5570_transport_branch_role role;
	enum it5570_reason_token reject_reason;
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

struct it5570_channel_sample {
	bool available;
	long value;
};

struct it5570_sensor_state {
	struct mutex lock;
	struct it5570_channel_sample voltage[IT5570_ADC_CHANNEL_COUNT];
	struct it5570_channel_sample tach[IT5570_TACH_CHANNEL_COUNT];
	bool prepared;
	unsigned int readable_voltage_count;
	unsigned int readable_tach_count;
	struct device *hwmon_dev;
};

struct it5570_hwmon_data {
	struct device *dev;
	unsigned short sio_port;
	u8 chipid1;
	u8 chipid2;
	u8 chipver;
	struct mutex io_lock;
	struct it5570_hwmon_gate_desc gate;
	struct it5570_transport_state transport_state;
	enum it5570_transport_mode transport_mode;
	enum it5570_fault_inject_mode fault_mode;
	struct it5570_sensor_state sensor;
};

struct it5570_pnp_id {
	unsigned short sio_port;
	u8 chipid1;
	u8 chipid2;
	u8 chipver;
};

extern const struct it5570_transport_branch_desc it5570_transport_branches[];
extern const unsigned int it5570_transport_branch_count;
extern const unsigned short it5570_superio_ports[];
extern const unsigned int it5570_superio_port_count;

const char *it5570_transport_name(enum it5570_transport_mode mode);
const char *it5570_fault_name(enum it5570_fault_inject_mode mode);
const char *it5570_reason_name(enum it5570_reason_token reason);
const struct it5570_transport_branch_desc *it5570_transport_branch_desc_lookup(
	enum it5570_transport_mode mode);

void it5570_log_fault_inject(struct device *dev, enum it5570_fault_inject_mode mode);
void it5570_log_probe_start(struct device *dev, const char *transport,
			    unsigned short port);
void it5570_log_chip_id(struct device *dev, const struct it5570_pnp_id *id,
			bool match);
void it5570_log_chip_id_fail(struct device *dev, const struct it5570_pnp_id *id);
void it5570_log_ldn(struct device *dev, u8 ldn, u8 lda, u16 iobad,
		    const char *status);
void it5570_log_transport_evaluating(struct device *dev,
				     const struct it5570_transport_branch_desc *branch,
				     bool forced);
void it5570_log_transport_selected(struct device *dev,
				   const struct it5570_transport_branch_desc *branch,
				   u16 iobad);
void it5570_log_transport_rejected(struct device *dev,
				   const struct it5570_transport_branch_desc *branch,
				   enum it5570_reason_token reason);
void it5570_log_transport_skipped(struct device *dev,
				  const struct it5570_transport_branch_desc *branch,
				  enum it5570_reason_token reason,
				  bool selected_already);
void it5570_log_transport_abort(struct device *dev,
				const struct it5570_transport_branch_desc *branch,
				enum it5570_reason_token reason);
void it5570_log_hwmon_register_skipped(struct device *dev,
				       enum it5570_reason_token reason);
void it5570_log_hwmon_register_ready(struct device *dev,
				     const struct it5570_hwmon_data *data);
void it5570_log_all_deferred_interfaces(struct device *dev);
void it5570_log_io_error(const struct it5570_hwmon_data *data, const char *op,
			 const char *class_name, const char *channel_name,
			 enum it5570_reason_token reason);

void it5570_superio_enter(unsigned short sio_port);
void it5570_superio_exit(unsigned short sio_port);
int it5570_superio_request(unsigned short sio_port);
void it5570_superio_release(unsigned short sio_port);
u8 it5570_superio_read8(unsigned short sio_port, u8 reg);
u16 it5570_superio_read16(unsigned short sio_port, u8 reg);
void it5570_superio_select_ldn(unsigned short sio_port, u8 ldn);
int it5570_transport_read8(struct it5570_hwmon_data *data, u16 reg, u8 *value);
int it5570_transport_write8(struct it5570_hwmon_data *data, u16 reg, u8 value);
void it5570_transport_state_clear(struct it5570_hwmon_data *data,
				  enum it5570_reason_token reason);
void it5570_transport_state_select(struct it5570_hwmon_data *data,
				   const struct it5570_transport_branch_desc *branch,
				   u16 iobad);

int it5570_prepare_sensor_hwmon(struct it5570_hwmon_data *data);
int it5570_register_hwmon_device(struct platform_device *pdev,
				 struct it5570_hwmon_data *data);
void it5570_sensor_state_reset(struct it5570_hwmon_data *data);
enum it5570_reason_token it5570_read_reason_from_errno(int ret);
enum it5570_reason_token it5570_write_reason_from_errno(int ret);

void it5570_probe_acpi_sensors(struct device *dev, bool enabled);

#endif /* IT5570_HWMON_H */
