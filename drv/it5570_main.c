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
MODULE_PARM_DESC(fault_inject, "none|id_mismatch|smfi_unstable|reg_access_fail");

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
	if (!strcmp(fault_inject, "smfi_unstable"))
		return IT5570_FAULT_SMFI_UNSTABLE;
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
	int ret;
	int i;

	ret = it5570_superio_request(sio_port);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(probe_ldns); i++) {
		u8 lda;
		u16 iobad;
		const char *status;

		it5570_superio_select_ldn(sio_port, probe_ldns[i]);
		lda = it5570_superio_read8(sio_port, IT5570_REG_LDA);
		iobad = it5570_superio_read16(sio_port, IT5570_REG_IOBAD0_MSB);

		if (!(lda & 0x01))
			status = it5570_reason_name(IT5570_REASON_LDA_DISABLED);
		else if (!iobad)
			status = it5570_reason_name(IT5570_REASON_IOBAD_ZERO);
		else
			status = "ok";

		it5570_log_ldn(dev, probe_ldns[i], lda, iobad, status);
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

static int it5570_evaluate_d2ec_transport(struct it5570_hwmon_data *data,
					  const struct it5570_transport_branch_desc *branch,
					  bool forced)
{
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
	it5570_transport_state_select(data, branch, data->sio_port);
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

		if (branch->mode == IT5570_TRANSPORT_SMFI)
			ret = it5570_evaluate_smfi_transport(data, forced);
		else if (branch->mode == IT5570_TRANSPORT_D2EC)
			ret = it5570_evaluate_d2ec_transport(data, branch, forced);
		else
			ret = it5570_evaluate_fallback_transport(data, branch, forced);

		if (ret)
			return ret;

		if (data->transport_state.ready &&
		    data->transport_state.mode != IT5570_TRANSPORT_D2EC) {
			it5570_transport_state_clear(data, IT5570_REASON_TRANSPORT_UNSUPPORTED);
			if (forced) {
				it5570_log_transport_abort(data->dev, branch,
							   IT5570_REASON_TRANSPORT_UNSUPPORTED);
				return -EOPNOTSUPP;
			}
			continue;
		}

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
