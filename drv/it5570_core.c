// SPDX-License-Identifier: GPL-2.0-or-later
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/err.h>
#ifdef CONFIG_ACPI
#include <linux/acpi.h>
#endif
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "it5570_hwmon.h"

struct it5570_fault_desc {
	enum it5570_fault_inject_mode mode;
	const char *name;
};

struct it5570_deferred_interface_desc {
	const char *name;
	enum it5570_reason_token deferred_reason;
};

static const char * const it5570_transport_names[] = {
	[IT5570_TRANSPORT_AUTO] = "auto",
	[IT5570_TRANSPORT_SMFI] = "smfi",
	[IT5570_TRANSPORT_PMC1] = "pmc1",
	[IT5570_TRANSPORT_PMC2] = "pmc2",
	[IT5570_TRANSPORT_PMC3] = "pmc3",
	[IT5570_TRANSPORT_PMC4] = "pmc4",
	[IT5570_TRANSPORT_PMC5] = "pmc5",
	[IT5570_TRANSPORT_PECI] = "peci",
	[IT5570_TRANSPORT_D2EC] = "d2ec",
	[IT5570_TRANSPORT_OFF] = "off",
};

static const struct it5570_fault_desc it5570_fault_table[] = {
	{ IT5570_FAULT_NONE, "none" },
	{ IT5570_FAULT_ID_MISMATCH, "id_mismatch" },
	{ IT5570_FAULT_SMFI_UNSTABLE, "smfi_unstable" },
	{ IT5570_FAULT_REG_ACCESS_FAIL, "reg_access_fail" },
};

static const char *it5570_visible_node_summary(void)
{
	return "name,in0_input..in7_input,fan1_input..fan3_input,pwm1..pwm8";
}

static const char *it5570_deferred_interface_summary(void)
{
	return "peci,h2ram,pmc-mailbox";
}

const struct it5570_transport_branch_desc it5570_transport_branches[] = {
	{ IT5570_TRANSPORT_D2EC, "d2ec", 0, IT5570_BRANCH_PRIMARY, IT5570_REASON_NO_USABLE_TRANSPORT },
	{ IT5570_TRANSPORT_SMFI, "smfi", IT5570_LDN_SMFI, IT5570_BRANCH_PRIMARY, IT5570_REASON_NO_USABLE_TRANSPORT },
	{ IT5570_TRANSPORT_PMC1, "pmc1", IT5570_LDN_PMC1, IT5570_BRANCH_FALLBACK, IT5570_REASON_FORCED_TRANSPORT_FAILED },
	{ IT5570_TRANSPORT_PMC2, "pmc2", IT5570_LDN_PMC2, IT5570_BRANCH_FALLBACK, IT5570_REASON_FORCED_TRANSPORT_FAILED },
	{ IT5570_TRANSPORT_PMC3, "pmc3", IT5570_LDN_PMC3, IT5570_BRANCH_FALLBACK, IT5570_REASON_FORCED_TRANSPORT_FAILED },
	{ IT5570_TRANSPORT_PMC4, "pmc4", IT5570_LDN_PMC4, IT5570_BRANCH_FALLBACK, IT5570_REASON_FORCED_TRANSPORT_FAILED },
	{ IT5570_TRANSPORT_PMC5, "pmc5", IT5570_LDN_PMC5, IT5570_BRANCH_FALLBACK, IT5570_REASON_FORCED_TRANSPORT_FAILED },
	{ IT5570_TRANSPORT_PECI, "peci", IT5570_LDN_PECI, IT5570_BRANCH_AUXILIARY, IT5570_REASON_NO_USABLE_TRANSPORT },
};

const unsigned int it5570_transport_branch_count = ARRAY_SIZE(it5570_transport_branches);

static const struct it5570_deferred_interface_desc it5570_deferred_interfaces[] = {
	{ "peci", IT5570_REASON_NO_USABLE_TRANSPORT },
	{ "h2ram", IT5570_REASON_NO_USABLE_TRANSPORT },
	{ "pmc-mailbox", IT5570_REASON_NO_USABLE_TRANSPORT },
};

const unsigned short it5570_superio_ports[] = {
	IT5570_SUPERIO_PORT_2E,
	IT5570_SUPERIO_PORT_4E,
};

const unsigned int it5570_superio_port_count = ARRAY_SIZE(it5570_superio_ports);

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
		{ IT5570_REASON_UNSUPPORTED_CHANNEL, "unsupported-channel" },
		{ IT5570_REASON_READ_NOT_READY, "read-not-ready" },
		{ IT5570_REASON_CHANNEL_NO_DATA, "channel-no-data" },
		{ IT5570_REASON_CHANNEL_NOT_READY, "channel-not-ready" },
		{ IT5570_REASON_TRANSPORT_UNSUPPORTED, "transport-unsupported" },
		{ IT5570_REASON_PERMISSION_DENIED, "permission-denied" },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(reason_table); i++) {
		if (reason_table[i].token == reason)
			return reason_table[i].name;
	}

	return "unknown";
}

void it5570_log_probe_start(struct device *dev, const char *transport,
			    unsigned short port)
{
	IT5570_LOG_STAGE(dev, "probe", "start",
			 "port=0x%02x transport=%s revision=%s\n",
			 port, transport, IT5570_DRIVER_REVISION);
}

void it5570_log_chip_id(struct device *dev, const struct it5570_pnp_id *id,
			bool match)
{
	IT5570_LOG_STAGE(dev, "probe", "chip-id",
			 "chipid1=0x%02x chipid2=0x%02x chipver=0x%02x match=%s\n",
			 id->chipid1, id->chipid2, id->chipver, match ? "yes" : "no");
}

void it5570_log_chip_id_fail(struct device *dev, const struct it5570_pnp_id *id)
{
	IT5570_LOG_STAGE(dev, "probe", "chip-id-fail",
			 "chipid1=0x%02x chipid2=0x%02x reason=%s\n",
			 id->chipid1, id->chipid2,
			 it5570_reason_name(IT5570_REASON_ID_MISMATCH));
}

void it5570_log_ldn(struct device *dev, u8 ldn, u8 lda, u16 iobad,
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

void it5570_log_fault_inject(struct device *dev,
			     enum it5570_fault_inject_mode mode)
{
	if (mode == IT5570_FAULT_NONE)
		return;

	IT5570_LOG_STAGE(dev, "test", "fault-inject", "target=%s status=on\n",
			 it5570_fault_name(mode));
}

void it5570_log_transport_evaluating(struct device *dev,
				     const struct it5570_transport_branch_desc *branch,
				     bool forced)
{
	IT5570_LOG_STAGE(dev, "transport", "evaluating",
			 "branch=%s forced=%u\n", branch->name, forced ? 1 : 0);
}

void it5570_log_transport_selected(struct device *dev,
				   const struct it5570_transport_branch_desc *branch,
				   u16 iobad)
{
	IT5570_LOG_STAGE(dev, "transport", "selected",
			 "branch=%s ldn=0x%02x iobad=0x%04x\n",
			 branch->name, branch->ldn, iobad);
}

void it5570_log_transport_rejected(struct device *dev,
				   const struct it5570_transport_branch_desc *branch,
				   enum it5570_reason_token reason)
{
	IT5570_LOG_WARN(dev,
			"stage=transport event=rejected branch=%s ldn=0x%02x reason=%s\n",
			branch->name, branch->ldn, it5570_reason_name(reason));
}

void it5570_log_transport_skipped(struct device *dev,
				  const struct it5570_transport_branch_desc *branch,
				  enum it5570_reason_token reason,
				  bool selected_already)
{
	const char *skip_reason = selected_already ? "already-selected" :
		it5570_reason_name(reason);

	IT5570_LOG_WARN(dev,
			 "stage=transport event=skipped branch=%s ldn=0x%02x reason=%s\n",
			 branch->name, branch->ldn, skip_reason);
}

void it5570_log_transport_abort(struct device *dev,
				const struct it5570_transport_branch_desc *branch,
				enum it5570_reason_token reason)
{
	IT5570_LOG_ERR(dev,
		      "stage=transport event=abort branch=%s reason=%s\n",
		      branch->name, it5570_reason_name(reason));
}

void it5570_log_hwmon_register_skipped(struct device *dev,
				       enum it5570_reason_token reason)
{
	IT5570_LOG_STAGE(dev, "hwmon", "register",
			 "status=skipped reason=%s channels=%s deferred=%s\n",
			 it5570_reason_name(reason),
			 it5570_visible_node_summary(),
			 it5570_deferred_interface_summary());
}

void it5570_log_hwmon_register_ready(struct device *dev,
				     const struct it5570_hwmon_data *data)
{
	char summary[256];
	size_t len = 0;
	bool pwm_visible = data->transport_state.mode == IT5570_TRANSPORT_D2EC &&
			   data->transport_state.ready;
	int i;

	summary[0] = '\0';

	if (data->sensor.prepared) {
		for (i = 0; i < ARRAY_SIZE(data->sensor.voltage); i++) {
			if (!data->sensor.voltage[i].available)
				continue;

			len += scnprintf(summary + len, sizeof(summary) - len,
					 "%sin%d_input", len ? "," : "", i);
		}

		for (i = 0; i < ARRAY_SIZE(data->sensor.tach); i++) {
			if (!data->sensor.tach[i].available)
				continue;

			len += scnprintf(summary + len, sizeof(summary) - len,
					 "%sfan%d_input", len ? "," : "", i + 1);
		}
	}

	if (pwm_visible) {
		for (i = 0; i < IT5570_PWM_CHANNEL_COUNT; i++)
			len += scnprintf(summary + len, sizeof(summary) - len,
					 "%spwm%d", len ? "," : "", i + 1);
	}

	IT5570_LOG_STAGE(dev, "hwmon", "register",
			 "status=ready transport=%s channels=%s deferred=%s\n",
			 it5570_transport_name(data->transport_state.mode),
			 summary[0] ? summary : "none",
			 it5570_deferred_interface_summary());
}

void it5570_log_all_deferred_interfaces(struct device *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(it5570_deferred_interfaces); i++)
		IT5570_LOG_WARN(dev,
				"stage=hwmon event=deferred interface=%s reason=%s\n",
				it5570_deferred_interfaces[i].name,
				it5570_reason_name(
					it5570_deferred_interfaces[i].deferred_reason));
}

void it5570_log_io_error(const struct it5570_hwmon_data *data, const char *op,
			 const char *class_name, const char *channel_name,
			 enum it5570_reason_token reason)
{
	IT5570_LOG_WARN_RL(data->dev,
			   "stage=%s event=error transport=%s ldn=0x%02x class=%s channel=%s reason=%s\n",
			   op,
			   it5570_transport_name(data->transport_state.mode),
			   data->transport_state.ldn,
			   class_name, channel_name, it5570_reason_name(reason));
}

#ifdef CONFIG_ACPI
static void it5570_log_acpi_object(struct device *dev, const char *event,
				   const char *path, union acpi_object *obj)
{
	int len;

	if (!obj) {
		IT5570_LOG_WARN(dev,
				"stage=acpi event=%s path=%s status=fail reason=null-object\n",
				event, path);
		return;
	}

	switch (obj->type) {
	case ACPI_TYPE_BUFFER:
		len = min_t(u32, obj->buffer.length, 64);
		IT5570_LOG_STAGE(dev, "acpi", event,
				 "path=%s status=ok type=buffer len=%u data=%*phN\n",
				 path, obj->buffer.length, len, obj->buffer.pointer);
		break;
	case ACPI_TYPE_INTEGER:
		IT5570_LOG_STAGE(dev, "acpi", event,
				 "path=%s status=ok type=integer value=0x%llx\n",
				 path, (unsigned long long)obj->integer.value);
		break;
	case ACPI_TYPE_PACKAGE:
		IT5570_LOG_STAGE(dev, "acpi", event,
				 "path=%s status=ok type=package count=%u\n",
				 path, obj->package.count);
		break;
	default:
		IT5570_LOG_STAGE(dev, "acpi", event,
				 "path=%s status=ok type=%u\n",
				 path, obj->type);
		break;
	}
}

static void it5570_log_acpi_package_elements(struct device *dev, const char *event,
					     const char *path, union acpi_object *obj)
{
	union acpi_object *element;
	int len;
	u32 i;

	if (!obj || obj->type != ACPI_TYPE_PACKAGE)
		return;

	for (i = 0; i < min_t(u32, obj->package.count, 16); i++) {
		element = &obj->package.elements[i];
		switch (element->type) {
		case ACPI_TYPE_INTEGER:
			IT5570_LOG_STAGE(dev, "acpi", event,
					 "path=%s index=%u type=integer value=0x%llx\n",
					 path, i,
					 (unsigned long long)element->integer.value);
			break;
		case ACPI_TYPE_BUFFER:
			len = min_t(u32, element->buffer.length, 32);
			IT5570_LOG_STAGE(dev, "acpi", event,
					 "path=%s index=%u type=buffer len=%u data=%*phN\n",
					 path, i, element->buffer.length, len,
					 element->buffer.pointer);
			break;
		default:
			IT5570_LOG_STAGE(dev, "acpi", event,
					 "path=%s index=%u type=%u\n",
					 path, i, element->type);
			break;
		}
	}
}

static bool it5570_try_log_acpi_method(struct device *dev, const char *event,
				       const char *path)
{
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	status = acpi_evaluate_object(NULL, (char *)path, NULL, &buffer);
	if (ACPI_FAILURE(status)) {
		IT5570_LOG_WARN(dev,
				"stage=acpi event=%s path=%s status=fail acpi_status=0x%x\n",
				event, path, status);
		return false;
	}

	obj = buffer.pointer;
	it5570_log_acpi_object(dev, event, path, obj);
	it5570_log_acpi_package_elements(dev, event, path, obj);
	ACPI_FREE(buffer.pointer);

	return true;
}

static bool it5570_acpi_name_is_interesting(const char *name)
{
	static const char * const names[] = {
		"H_EC", "DUMY", "RPMD", "RPRC", "RFPM", "RFTH", "RFTL",
		"RFCS", "RCFS", "RSSV", "RBPM", "RBPV", "RPBS", "RCTP",
		"RRPT", "RADR", "RWPP", "RPOI", "RPCS", "RPES", "RPNV",
		"RLDS", "RMPT", "RCPH", "RPDP", "RVPS", "RAPS", "ERB1",
		"XDAT",
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (!strcmp(name, names[i]))
			return true;
	}

	return false;
}

static bool it5570_acpi_name_is_read_method(const char *name)
{
	if (!strcmp(name, "RPMD") || !strcmp(name, "RPRC") ||
	    !strcmp(name, "RFPM") || !strcmp(name, "RFTH") ||
	    !strcmp(name, "RFTL") || !strcmp(name, "RFCS") ||
	    !strcmp(name, "RCFS") || !strcmp(name, "RSSV") ||
	    !strcmp(name, "RBPM") || !strcmp(name, "RBPV") ||
	    !strcmp(name, "RPBS") || !strcmp(name, "RCTP") ||
	    !strcmp(name, "RRPT") || !strcmp(name, "RADR") ||
	    !strcmp(name, "RWPP") || !strcmp(name, "RPOI") ||
	    !strcmp(name, "RPCS") || !strcmp(name, "RPES") ||
	    !strcmp(name, "RPNV") || !strcmp(name, "RLDS") ||
	    !strcmp(name, "RMPT") || !strcmp(name, "RCPH") ||
	    !strcmp(name, "RPDP") || !strcmp(name, "RVPS") ||
	    !strcmp(name, "RAPS") || !strcmp(name, "ERB1") ||
	    !strcmp(name, "XDAT"))
		return true;

	return false;
}

static const char *it5570_acpi_path_tail(const char *path)
{
	const char *tail = strrchr(path, '.');

	if (tail)
		return tail + 1;

	if (path[0] == '\\')
		return path + 1;

	return path;
}

static void it5570_try_log_acpi_handle(struct device *dev, const char *event,
				       const char *path, acpi_handle handle)
{
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	status = acpi_evaluate_object(handle, NULL, NULL, &buffer);
	if (ACPI_FAILURE(status)) {
		IT5570_LOG_WARN(dev,
				"stage=acpi event=%s path=%s status=fail acpi_status=0x%x\n",
				event, path, status);
		return;
	}

	obj = buffer.pointer;
	it5570_log_acpi_object(dev, event, path, obj);
	it5570_log_acpi_package_elements(dev, event, path, obj);
	ACPI_FREE(buffer.pointer);
}

struct it5570_acpi_walk_context {
	struct device *dev;
};

static acpi_status it5570_acpi_walk_namespace_cb(acpi_handle handle, u32 level,
						void *context,
						void **return_value)
{
	struct it5570_acpi_walk_context *walk = context;
	struct acpi_buffer name = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_object_type type;
	acpi_status status;
	const char *path;
	const char *tail;

	(void)level;
	(void)return_value;

	status = acpi_get_name(handle, ACPI_FULL_PATHNAME, &name);
	if (ACPI_FAILURE(status) || !name.pointer)
		return AE_OK;

	path = name.pointer;
	tail = it5570_acpi_path_tail(path);
	if (!it5570_acpi_name_is_interesting(tail))
		goto out;

	status = acpi_get_type(handle, &type);
	if (ACPI_FAILURE(status))
		goto out;

	IT5570_LOG_STAGE(walk->dev, "acpi", "namespace",
			 "path=%s name=%s type=%u\n", path, tail, type);

	if (type == ACPI_TYPE_METHOD && it5570_acpi_name_is_read_method(tail))
		it5570_try_log_acpi_handle(walk->dev, "ec-discovered", path, handle);

out:
	ACPI_FREE(name.pointer);
	return AE_OK;
}

static void it5570_walk_acpi_namespace(struct device *dev)
{
	struct it5570_acpi_walk_context context = { .dev = dev };
	acpi_status status;

	status = acpi_walk_namespace(ACPI_TYPE_ANY, ACPI_ROOT_OBJECT,
				     ACPI_UINT32_MAX,
				     it5570_acpi_walk_namespace_cb, NULL,
				     &context, NULL);
	if (ACPI_FAILURE(status))
		IT5570_LOG_WARN(dev,
				"stage=acpi event=namespace status=fail acpi_status=0x%x\n",
				status);
}

void it5570_probe_acpi_sensors(struct device *dev, bool enabled)
{
	static const char * const rpmd_paths[] = {
		"\\_SB.PTID.RPMD",
		"\\_SB.PC00.LPCB.RPMD",
		"\\_SB.PC00.LPCB.H_EC.DUMY.RPRC",
	};
	static const char * const ec_read_paths[] = {
		"\\_SB.PC00.LPCB.H_EC.RFPM",
		"\\_SB.PC00.LPCB.H_EC.RFTH",
		"\\_SB.PC00.LPCB.H_EC.RFTL",
		"\\_SB.PC00.LPCB.H_EC.RFCS",
		"\\_SB.PC00.LPCB.H_EC.RCFS",
		"\\_SB.PC00.LPCB.H_EC.RSSV",
		"\\_SB.PC00.LPCB.H_EC.RBPM",
		"\\_SB.PC00.LPCB.H_EC.RBPV",
		"\\_SB.PC00.LPCB.H_EC.RPBS",
		"\\_SB.PC00.LPCB.H_EC.RCTP",
		"\\_SB.PC00.LPCB.H_EC.RRPT",
		"\\_SB.PC00.LPCB.H_EC.RADR",
		"\\_SB.PC00.LPCB.H_EC.RWPP",
		"\\_SB.PC00.LPCB.H_EC.RPOI",
		"\\_SB.PC00.LPCB.H_EC.RPCS",
		"\\_SB.PC00.LPCB.H_EC.RPES",
		"\\_SB.PC00.LPCB.H_EC.RPNV",
		"\\_SB.PC00.LPCB.H_EC.RPRC",
		"\\_SB.PC00.LPCB.H_EC.RLDS",
		"\\_SB.PC00.LPCB.H_EC.RMPT",
		"\\_SB.PC00.LPCB.H_EC.RCPH",
		"\\_SB.PC00.LPCB.H_EC.RPDP",
		"\\_SB.PC00.LPCB.H_EC.RVPS",
		"\\_SB.PC00.LPCB.H_EC.RAPS",
		"\\_SB.PC00.LPCB.H_EC.ERB1",
		"\\_SB.PC00.LPCB.H_EC.XDAT",
	};
	int i;

	if (!enabled)
		return;

	for (i = 0; i < ARRAY_SIZE(rpmd_paths); i++)
		it5570_try_log_acpi_method(dev, "rpmd", rpmd_paths[i]);

	for (i = 0; i < ARRAY_SIZE(ec_read_paths); i++)
		it5570_try_log_acpi_method(dev, "ec-method", ec_read_paths[i]);

	it5570_walk_acpi_namespace(dev);
}
#else
void it5570_probe_acpi_sensors(struct device *dev, bool enabled)
{
	if (enabled)
		IT5570_LOG_WARN(dev,
				"stage=acpi event=rpmd status=skipped reason=acpi-disabled\n");
}
#endif

void it5570_superio_enter(unsigned short sio_port)
{
	outb(0x87, sio_port);
	outb(0x01, sio_port);
	outb(0x55, sio_port);
	outb(sio_port == IT5570_SUPERIO_PORT_4E ? 0xaa : 0x55, sio_port);
}

void it5570_superio_exit(unsigned short sio_port)
{
	outb(0x02, sio_port);
	outb(0x02, sio_port + 1);
}

int it5570_superio_request(unsigned short sio_port)
{
	if (!request_muxed_region(sio_port, 2, IT5570_HWMON_NAME))
		return -EBUSY;

	it5570_superio_enter(sio_port);

	return 0;
}

void it5570_superio_release(unsigned short sio_port)
{
	it5570_superio_exit(sio_port);
	release_region(sio_port, 2);
}

u8 it5570_superio_read8(unsigned short sio_port, u8 reg)
{
	outb(reg, sio_port);

	return inb(sio_port + 1);
}

void it5570_superio_select_ldn(unsigned short sio_port, u8 ldn)
{
	outb(IT5570_REG_LDN, sio_port);
	outb(ldn, sio_port + 1);
}

static void it5570_superio_d2_write8(unsigned short sio_port, u8 reg, u8 value)
{
	outb(IT5570_REG_D2ADR, sio_port);
	outb(reg, sio_port + 1);
	outb(IT5570_REG_D2DAT, sio_port);
	outb(value, sio_port + 1);
}

static u8 it5570_superio_d2_read8(unsigned short sio_port, u8 reg)
{
	outb(IT5570_REG_D2ADR, sio_port);
	outb(reg, sio_port + 1);
	outb(IT5570_REG_D2DAT, sio_port);

	return inb(sio_port + 1);
}

static int it5570_d2ec_select_addr(struct it5570_hwmon_data *data, u16 addr)
{
	/*
	 * Datasheet 6.3.2.9/.10 define D2ADR/D2DAT as a second-level
	 * SuperIO index/data pair. The I2EC-over-D2EC flow writes the EC
	 * memory address high byte then low byte and then accesses I2EC_DATA.
	 */
	it5570_superio_d2_write8(data->sio_port, IT5570_D2_I2EC_ADDR_H,
				 addr >> 8);
	it5570_superio_d2_write8(data->sio_port, IT5570_D2_I2EC_ADDR_L,
				 addr & 0xff);

	return 0;
}

static int it5570_d2ec_read8(struct it5570_hwmon_data *data, u16 addr, u8 *value)
{
	int ret;

	mutex_lock(&data->io_lock);
	ret = it5570_superio_request(data->sio_port);
	if (ret)
		goto out_unlock;

	ret = it5570_d2ec_select_addr(data, addr);
	if (ret)
		goto out_release;

	*value = it5570_superio_d2_read8(data->sio_port, IT5570_D2_I2EC_DATA);

out_release:
	it5570_superio_release(data->sio_port);

	IT5570_LOG_DBG(data->dev, "stage=d2ec event=read addr=0x%04x value=0x%02x\n",
		       addr, *value);

out_unlock:
	mutex_unlock(&data->io_lock);

	return ret;
}

static int it5570_d2ec_write8(struct it5570_hwmon_data *data, u16 addr, u8 value)
{
	int ret;

	mutex_lock(&data->io_lock);
	ret = it5570_superio_request(data->sio_port);
	if (ret)
		goto out_unlock;

	ret = it5570_d2ec_select_addr(data, addr);
	if (ret)
		goto out_release;

	it5570_superio_d2_write8(data->sio_port, IT5570_D2_I2EC_DATA, value);

out_release:
	it5570_superio_release(data->sio_port);

	IT5570_LOG_DBG(data->dev, "stage=d2ec event=write addr=0x%04x value=0x%02x\n",
		       addr, value);

out_unlock:
	mutex_unlock(&data->io_lock);

	return ret;
}

u16 it5570_superio_read16(unsigned short sio_port, u8 reg)
{
	return ((u16)it5570_superio_read8(sio_port, reg) << 8) |
		it5570_superio_read8(sio_port, reg + 1);
}

void it5570_transport_state_clear(struct it5570_hwmon_data *data,
				  enum it5570_reason_token reason)
{
	data->transport_state.mode = IT5570_TRANSPORT_OFF;
	data->transport_state.ldn = 0;
	data->transport_state.iobad = 0;
	data->transport_state.ready = false;
	data->gate.transport_ready = false;
	data->gate.blocked_reason = reason;
}

void it5570_transport_state_select(struct it5570_hwmon_data *data,
				   const struct it5570_transport_branch_desc *branch,
				   u16 iobad)
{
	data->transport_state.mode = branch->mode;
	data->transport_state.ldn = branch->ldn;
	data->transport_state.iobad = iobad;
	data->transport_state.ready = true;
	data->gate.transport_ready = true;
}

static int it5570_transport_check_ready(struct it5570_hwmon_data *data)
{
	if (!data->transport_state.ready)
		return -ENODEV;

	if (data->fault_mode == IT5570_FAULT_REG_ACCESS_FAIL) {
		it5570_log_fault_inject(data->dev, data->fault_mode);
		return -EIO;
	}

	if (data->transport_state.mode != IT5570_TRANSPORT_D2EC)
		return -EOPNOTSUPP;

	return 0;
}

int it5570_transport_read8(struct it5570_hwmon_data *data, u16 reg, u8 *value)
{
	int ret;

	ret = it5570_transport_check_ready(data);
	if (ret)
		return ret;

	return it5570_d2ec_read8(data, reg, value);
}

int it5570_transport_write8(struct it5570_hwmon_data *data, u16 reg, u8 value)
{
	int ret;

	ret = it5570_transport_check_ready(data);
	if (ret)
		return ret;

	return it5570_d2ec_write8(data, reg, value);
}

