// SPDX-License-Identifier: GPL-2.0-or-later
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "it5570_hwmon.h"

static char *transport = "auto";
module_param(transport, charp, 0444);
MODULE_PARM_DESC(transport, "auto|d2ec|smfi|pmc1|pmc2|pmc3|pmc4|pmc5|peci|off");

static char *fault_inject = "none";
module_param(fault_inject, charp, 0444);
MODULE_PARM_DESC(fault_inject, "none|id_mismatch|reg_access_fail");

static bool probe_acpi;
module_param(probe_acpi, bool, 0644);
MODULE_PARM_DESC(probe_acpi, "Probe read-only ACPI RPMD/RPRC sensor methods");

static struct platform_device *it5570_hwmon_pdev;
static int it5570_hwmon_probe_status;
static bool it5570_hwmon_probe_finished;

static enum it5570_transport_mode it5570_transport_mode_from_param(void)
{
	if (!strcmp(transport, "auto"))
		return IT5570_TRANSPORT_AUTO;
	if (!strcmp(transport, "d2ec"))
		return IT5570_TRANSPORT_D2EC;
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
	if (!strcmp(fault_inject, "reg_access_fail"))
		return IT5570_FAULT_REG_ACCESS_FAIL;

	return IT5570_FAULT_NONE;
}

static bool it5570_hwmon_registration_ready(const struct it5570_hwmon_data *data)
{
	return data->gate.identification_ok && data->gate.transport_ready;
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

	for (i = 0; i < it5570_superio_port_count; i++) {
		it5570_log_probe_start(dev, transport, it5570_superio_ports[i]);

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
	static const u8 probe_ldns[] = {
		IT5570_LDN_SMFI,
		IT5570_LDN_PMC1,
		IT5570_LDN_PMC2,
		IT5570_LDN_PECI,
		IT5570_LDN_PMC3,
		IT5570_LDN_PMC4,
		IT5570_LDN_PMC5,
	};
	u8 sioctrl;
	int ret;
	int i;

	ret = it5570_superio_request(sio_port);
	if (ret)
		return ret;

	sioctrl = it5570_superio_read8(sio_port, IT5570_REG_SIOCTRL);

	for (i = 0; i < ARRAY_SIZE(probe_ldns); i++) {
		u8 ldn = probe_ldns[i];
		u8 lda;
		u16 iobad = 0;
		const char *status;

		it5570_superio_select_ldn(sio_port, ldn);
		lda = it5570_superio_read8(sio_port, IT5570_REG_LDA);

		if (!(lda & 0x01)) {
			status = it5570_reason_name(IT5570_REASON_LDA_DISABLED);
		} else if (ldn != IT5570_LDN_SMFI && !(sioctrl & IT5570_SIOCTRL_SIOEN)) {
			status = it5570_reason_name(IT5570_REASON_SIO_DISABLED);
		} else {
			iobad = it5570_superio_read16(sio_port, IT5570_REG_IOBAD0_MSB);
			status = iobad ? "ok" : it5570_reason_name(IT5570_REASON_IOBAD_ZERO);
		}

		it5570_log_ldn(dev, ldn, lda, iobad, status);
	}

	it5570_superio_release(sio_port);

	return 0;
}

static int it5570_evaluate_unsupported_transport(struct it5570_hwmon_data *data,
						 const struct it5570_transport_branch_desc *branch,
						 bool forced)
{
	it5570_log_transport_evaluating(data->dev, branch, forced);
	it5570_transport_state_clear(data, IT5570_REASON_TRANSPORT_UNSUPPORTED);
	it5570_log_transport_rejected(data->dev, branch,
				      IT5570_REASON_TRANSPORT_UNSUPPORTED);

	if (forced) {
		it5570_log_transport_abort(data->dev, branch,
						   IT5570_REASON_TRANSPORT_UNSUPPORTED);
		return -EOPNOTSUPP;
	}

	return 0;
}

static int it5570_validate_d2ec_transport(struct it5570_hwmon_data *data,
					       const struct it5570_transport_branch_desc *branch,
					       enum it5570_reason_token *reason)
{
	u8 value;
	int ret;

	it5570_transport_state_select(data, branch, data->sio_port);
	ret = it5570_transport_read8(data, IT5570_EC_REG_ADCDVSTS, &value);
	if (ret) {
		*reason = it5570_read_reason_from_errno(ret);
		it5570_transport_state_clear(data, *reason);
		return ret;
	}

	return 0;
}

static int it5570_evaluate_d2ec_transport(struct it5570_hwmon_data *data,
					  const struct it5570_transport_branch_desc *branch,
					  bool forced)
{
	enum it5570_reason_token reason;
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

	it5570_superio_release(data->sio_port);

	reason = IT5570_REASON_NO_USABLE_TRANSPORT;
	ret = it5570_validate_d2ec_transport(data, branch, &reason);
	if (ret) {
		it5570_log_transport_rejected(data->dev, branch, reason);
		if (forced) {
			it5570_log_transport_abort(data->dev, branch,
							   IT5570_REASON_FORCED_TRANSPORT_FAILED);
			return ret;
		}

		return 0;
	}

	it5570_log_transport_selected(data->dev, branch, data->sio_port);

	return 0;
}

static int it5570_select_transport(struct it5570_hwmon_data *data)
{
	int i;
	int ret;
	bool forced = data->transport_mode != IT5570_TRANSPORT_AUTO;

	if (data->transport_mode == IT5570_TRANSPORT_OFF) {
		it5570_transport_state_clear(data, IT5570_REASON_TRANSPORT_OFF);
		return 0;
	}

	for (i = 0; i < it5570_transport_branch_count; i++) {
		const struct it5570_transport_branch_desc *branch =
			&it5570_transport_branches[i];

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

		if (branch->mode == IT5570_TRANSPORT_D2EC)
			ret = it5570_evaluate_d2ec_transport(data, branch, forced);
		else
			ret = it5570_evaluate_unsupported_transport(data, branch, forced);

		if (ret)
			return ret;


		if (data->transport_state.ready)
			return 0;
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
		goto out;

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
	mutex_init(&data->sensor.lock);
	mutex_init(&data->io_lock);
	it5570_sensor_state_reset(data);

	ret = it5570_enumerate_ldns(&pdev->dev, data->sio_port);
	if (ret)
		goto out;

	ret = it5570_select_transport(data);
	if (ret)
		goto out;

	it5570_probe_acpi_sensors(&pdev->dev, probe_acpi);

	if (!it5570_hwmon_registration_ready(data)) {
		it5570_log_hwmon_register_skipped(&pdev->dev,
						  data->gate.blocked_reason);
		ret = 0;
		goto out;
	}

	ret = it5570_prepare_sensor_hwmon(data);
	if (ret) {
		it5570_log_hwmon_register_skipped(&pdev->dev,
						  IT5570_REASON_REG_ACCESS_FAILED);
		ret = 0;
		goto out;
	}

	ret = it5570_register_hwmon_device(pdev, data);
	if (ret) {
		it5570_log_hwmon_register_skipped(&pdev->dev,
						  IT5570_REASON_REG_ACCESS_FAILED);
		goto out;
	}

	it5570_log_hwmon_register_ready(&pdev->dev, data);
	it5570_log_all_deferred_interfaces(&pdev->dev);

out:
	it5570_hwmon_probe_status = ret;
	it5570_hwmon_probe_finished = true;

	return ret;
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

static int __init it5570_hwmon_init(void)
{
	int ret;

	ret = platform_driver_register(&it5570_hwmon_driver);
	if (ret)
		return ret;

	it5570_hwmon_probe_status = 0;
	it5570_hwmon_probe_finished = false;

	it5570_hwmon_pdev = platform_device_register_simple(IT5570_HWMON_NAME,
							    -1, NULL, 0);
	if (IS_ERR(it5570_hwmon_pdev)) {
		ret = PTR_ERR(it5570_hwmon_pdev);
		platform_driver_unregister(&it5570_hwmon_driver);
		return ret;
	}

	if (!it5570_hwmon_probe_finished)
		ret = -ENODEV;
	else
		ret = it5570_hwmon_probe_status;

	if (ret) {
		platform_device_unregister(it5570_hwmon_pdev);
		it5570_hwmon_pdev = NULL;
		platform_driver_unregister(&it5570_hwmon_driver);
		return ret;
	}

	return 0;
}

static void __exit it5570_hwmon_exit(void)
{
	platform_device_unregister(it5570_hwmon_pdev);
	platform_driver_unregister(&it5570_hwmon_driver);
}

module_init(it5570_hwmon_init);
module_exit(it5570_hwmon_exit);

MODULE_AUTHOR("GreenDamTan");
MODULE_DESCRIPTION("IT5570 hardware monitoring driver");
MODULE_INFO(revision, IT5570_DRIVER_REVISION);
MODULE_LICENSE("GPL");
