/*
 * Copyright (C) 2018 Samsung Electronics Co., Ltd.
 *
 * Authors:
 *	Raman Kumar Banka <raman.k2@samsung.com>
 *
 * PMIC rails controller for airbrush state manager.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 */
#include <linux/airbrush-sm-ctrl.h>

/* Enable boost mode on smps1 and ldo3 */
void ab_pmic_enable_boost(struct ab_state_context *sc)
{
	if (!regulator_is_enabled(sc->boost_smps1) &&
		!regulator_is_enabled(sc->boost_ldo3)) {
		regulator_enable(sc->boost_smps1);
		/*
		 * Note: once boost mode is enabled, both smps1 and ldo3
		 * are at boost mode.  Below call is actually no-op.
		 */
		regulator_enable(sc->boost_ldo3);
	}
}

/* Disable boost mode on smps1 and ldo3 */
void ab_pmic_disable_boost(struct ab_state_context *sc)
{
	if (regulator_is_enabled(sc->boost_ldo3) &&
		regulator_is_enabled(sc->boost_smps1)) {
		regulator_disable(sc->boost_ldo3);
		/*
		 * Note: once boost mode is disabled, both smps1 and ldo3
		 * are back to normal mode.  Below call is actually no-op.
		 */
		regulator_disable(sc->boost_smps1);
	}
}

int ab_blk_pw_rails_enable(struct ab_state_context *sc,
			   enum block_name blk_name,
			   enum block_state to_block_substate_id)
{
	dev_dbg(sc->dev,
			"enabling rails for block %u block substate id %u\n",
			blk_name, to_block_substate_id);

	switch (blk_name) {
	case BLK_IPU:
	case BLK_TPU:
		if (!regulator_is_enabled(sc->smps2))
			if (regulator_enable(sc->smps2))
				goto fail_regulator_enable;
		sc->smps2_state = true;
		if (!regulator_is_enabled(sc->ldo4))
			if (regulator_enable(sc->ldo4))
				goto fail_regulator_enable;
		sc->ldo4_state = true;
		if (!regulator_is_enabled(sc->ldo5))
			if (regulator_enable(sc->ldo5))
				goto fail_regulator_enable;
		sc->ldo5_state = true;
		if (!regulator_is_enabled(sc->smps1))
			if (regulator_enable(sc->smps1))
				goto fail_regulator_enable;
		sc->smps1_state = true;
		if (!regulator_is_enabled(sc->ldo3))
			if (regulator_enable(sc->ldo3))
				goto fail_regulator_enable;
		sc->ldo3_state = true;
		if (!regulator_is_enabled(sc->ldo2))
			if (regulator_enable(sc->ldo2))
				goto fail_regulator_enable;
		sc->ldo2_state = true;
		break;
	case BLK_AON:
		if (!regulator_is_enabled(sc->smps2))
			if (regulator_enable(sc->smps2))
				goto fail_regulator_enable;
		sc->smps2_state = true;
		if (!regulator_is_enabled(sc->ldo4))
			if (regulator_enable(sc->ldo4))
				goto fail_regulator_enable;
		sc->ldo4_state = true;
		if (!regulator_is_enabled(sc->ldo5))
			if (regulator_enable(sc->ldo5))
				goto fail_regulator_enable;
		sc->ldo5_state = true;
		break;
	case DRAM:
		if (!regulator_is_enabled(sc->ldo2))
			if (regulator_enable(sc->ldo2))
				goto fail_regulator_enable;
		sc->ldo2_state = true;
		break;
	case BLK_MIF:
		break;
	case BLK_FSYS:
		break;
	default:
		return -EINVAL;
	}
	return 0;

fail_regulator_enable:
	if (regulator_is_enabled(sc->ldo3)) {
		regulator_disable(sc->ldo3);
		sc->ldo3_state = false;
	}
	if (regulator_is_enabled(sc->smps1)) {
		regulator_disable(sc->smps1);
		sc->smps1_state = false;
	}
	if (regulator_is_enabled(sc->ldo5)) {
		regulator_disable(sc->ldo5);
		sc->ldo5_state = false;
	}
	if (regulator_is_enabled(sc->ldo4)) {
		regulator_disable(sc->ldo4);
		sc->ldo4_state = false;
	}
	if (regulator_is_enabled(sc->smps3)) {
		regulator_disable(sc->smps3);
		sc->smps3_state = false;
	}
	if (regulator_is_enabled(sc->ldo1)) {
		regulator_disable(sc->ldo1);
		sc->ldo1_state = false;
	}
	if (regulator_is_enabled(sc->smps2)) {
		regulator_disable(sc->smps2);
		sc->smps2_state = false;
	}
	dev_err(sc->dev, "PMIC power up failure\n");

	return -ENODEV;
}

int ab_blk_pw_rails_disable(struct ab_state_context *sc,
			   enum block_name blk_name,
			   enum block_state to_block_substate_id)
{
	dev_dbg(sc->dev,
			"disabling rails for block %u block substate id %u\n",
			blk_name, to_block_substate_id);

	switch (blk_name) {
	case BLK_IPU:
	case BLK_TPU:
		sc->ldo3_state = false;
		sc->smps1_state = false;
		break;
	case BLK_AON:
		sc->ldo5_state = false;
		sc->ldo4_state = false;
		sc->smps2_state = false;
		break;
	case DRAM:
		sc->ldo2_state = false;
		if (to_block_substate_id == BLOCK_STATE_3_0) {
			sc->ldo1_state = false;
			sc->smps3_state = false;
		}
		break;
	case BLK_MIF:
	case BLK_FSYS:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int ab_pmic_off(struct ab_state_context *sc)
{
	int ret1, ret2 = 0;

	dev_dbg(sc->dev, "Turning OFF PMIC rails\n");

	if (!sc->ldo2_state && regulator_is_enabled(sc->ldo2)) {
		ret1 = regulator_disable(sc->ldo2);
		if (ret1 < 0)
			dev_err(sc->dev,
				"failed to disable LDO2, ret %d\n", ret1);
		ret2 = ret2 ? ret2 : ret1;
	}

	if (!sc->ldo3_state && regulator_is_enabled(sc->ldo3)) {
		ret1 = regulator_disable(sc->ldo3);
		if (ret1 < 0)
			dev_err(sc->dev,
				"failed to disable LDO3, ret %d\n", ret1);
		ret2 = ret2 ? ret2 : ret1;
		usleep_range(2000, 3000);
	}

	if (!sc->smps1_state && regulator_is_enabled(sc->smps1)) {
		ret1 = regulator_disable(sc->smps1);
		if (ret1 < 0)
			dev_err(sc->dev,
				"failed to disable SMPS1, ret %d\n", ret1);
		ret2 = ret2 ? ret2 : ret1;
	}

	if (!sc->ldo5_state && regulator_is_enabled(sc->ldo5)) {
		ret1 = regulator_disable(sc->ldo5);
		if (ret1 < 0)
			dev_err(sc->dev,
				"failed to disable LDO5, ret %d\n", ret1);
		ret2 = ret2 ? ret2 : ret1;

		/* NOTE: delay required by b/120785608 */
		if (!sc->ldo4_state || !sc->smps2_state)
			usleep_range(sc->ldo5_delay, sc->ldo5_delay + 1);
	}

	if (!sc->ldo4_state && regulator_is_enabled(sc->ldo4)) {
		ret1 = regulator_disable(sc->ldo4);
		if (ret1 < 0)
			dev_err(sc->dev,
				"failed to disable LDO4, ret %d\n", ret1);
		ret2 = ret2 ? ret2 : ret1;

		if (sc->ldo4_delay)
			usleep_range(sc->ldo4_delay, sc->ldo4_delay + 1);
	}

	if (!sc->smps3_state && regulator_is_enabled(sc->smps3)) {
		ret1 = regulator_disable(sc->smps3);
		if (ret1 < 0)
			dev_err(sc->dev,
				"failed to disable SMPS3, ret %d\n", ret1);
		ret2 = ret2 ? ret2 : ret1;
	}

	if (!sc->ldo1_state && regulator_is_enabled(sc->ldo1)) {
		ret1 = regulator_disable(sc->ldo1);
		if (ret1 < 0)
			dev_err(sc->dev,
				"failed to disable LDO1, ret %d\n", ret1);
		ret2 = ret2 ? ret2 : ret1;
	}

	if (!sc->smps2_state && regulator_is_enabled(sc->smps2)) {
		ret1 = regulator_disable(sc->smps2);
		if (ret1 < 0)
			dev_err(sc->dev,
				"failed to disable SMPS2, ret %d\n", ret1);
		ret2 = ret2 ? ret2 : ret1;

		if (sc->smps2_delay)
			usleep_range(sc->smps2_delay, sc->smps2_delay + 1);
	}

	/* NOTE: delay required by b/120785608 */
	if (!sc->smps2_state &&
			!sc->ldo1_state &&
			!sc->smps3_state &&
			!sc->ldo4_state &&
			!sc->ldo5_state &&
			!sc->smps1_state &&
			!sc->ldo3_state &&
			!sc->ldo2_state)
		usleep_range(sc->s60_delay, sc->s60_delay + 1);

	return ret2;
}

int ab_pmic_on(struct ab_state_context *sc)
{
	dev_dbg(sc->dev, "setting rails to on\n");

	if (!regulator_is_enabled(sc->smps2))
		if (regulator_enable(sc->smps2))
			goto fail_regulator_enable;
	sc->smps2_state = true;

	if (!regulator_is_enabled(sc->ldo1))
		if (regulator_enable(sc->ldo1))
			goto fail_regulator_enable;
	sc->ldo1_state = true;

	if (!regulator_is_enabled(sc->smps3))
		if (regulator_enable(sc->smps3))
			goto fail_regulator_enable;
	sc->smps3_state = true;

	if (!regulator_is_enabled(sc->ldo4))
		if (regulator_enable(sc->ldo4))
			goto fail_regulator_enable;
	sc->ldo4_state = true;

	if (!regulator_is_enabled(sc->ldo5))
		if (regulator_enable(sc->ldo5))
			goto fail_regulator_enable;
	sc->ldo5_state = true;

	if (!regulator_is_enabled(sc->smps1))
		if (regulator_enable(sc->smps1))
			goto fail_regulator_enable;
	sc->smps1_state = true;

	if (!regulator_is_enabled(sc->ldo3))
		if (regulator_enable(sc->ldo3))
			goto fail_regulator_enable;
	sc->ldo3_state = true;

	if (!regulator_is_enabled(sc->ldo2))
		if (regulator_enable(sc->ldo2))
			goto fail_regulator_enable;
	sc->ldo2_state = true;

	return 0;

fail_regulator_enable:
	if (regulator_is_enabled(sc->ldo3))
		regulator_disable(sc->ldo3);
	sc->ldo3_state = false;
	if (regulator_is_enabled(sc->smps1))
		regulator_disable(sc->smps1);
	sc->smps1_state = false;
	if (regulator_is_enabled(sc->ldo5))
		regulator_disable(sc->ldo5);
	sc->ldo5_state = false;
	if (regulator_is_enabled(sc->ldo4))
		regulator_disable(sc->ldo4);
	sc->ldo4_state = false;
	if (regulator_is_enabled(sc->smps3))
		regulator_disable(sc->smps3);
	sc->smps3_state = false;
	if (regulator_is_enabled(sc->ldo1))
		regulator_disable(sc->ldo1);
	sc->ldo1_state = false;
	if (regulator_is_enabled(sc->smps2))
		regulator_disable(sc->smps2);
	sc->smps2_state = false;
	dev_err(sc->dev, "PMIC power up failure\n");
	return -ENODEV;
}

static int ab_register_notifier(struct ab_state_context *sc)
{
	struct device *dev = sc->dev;
	int ret = 0;

	ret |= devm_regulator_register_notifier(sc->smps1, &sc->regulator_nb);
	ret |= devm_regulator_register_notifier(sc->smps2, &sc->regulator_nb);
	ret |= devm_regulator_register_notifier(sc->smps3, &sc->regulator_nb);
	ret |= devm_regulator_register_notifier(sc->ldo1, &sc->regulator_nb);
	ret |= devm_regulator_register_notifier(sc->ldo2, &sc->regulator_nb);
	ret |= devm_regulator_register_notifier(sc->ldo3, &sc->regulator_nb);
	ret |= devm_regulator_register_notifier(sc->ldo4, &sc->regulator_nb);
	ret |= devm_regulator_register_notifier(sc->ldo5, &sc->regulator_nb);
	if (ret) {
		dev_err(dev, "failed to register notifier block\n");
		return ret;
	}

	return 0;
}

int ab_get_pmic_resources(struct ab_state_context *sc)
{
	struct device *dev = sc->dev;

	if (!sc->soc_pwrgood) {
		sc->soc_pwrgood =
			devm_gpiod_get(dev, "soc-pwrgood", GPIOD_OUT_LOW);
		if (IS_ERR(sc->soc_pwrgood)) {
			dev_err(dev, "Could not get pmic_soc_pwrgood gpio (%ld)\n",
					PTR_ERR(sc->soc_pwrgood));
			goto fail;
		}
	}

	if (!sc->ddr_sr) {
		sc->ddr_sr =
			devm_gpiod_get(dev, "ddr-sr", GPIOD_OUT_LOW);
		if (IS_ERR(sc->ddr_sr)) {
			dev_err(dev, "Could not get pmic_ddr_sr gpio (%ld)\n",
					PTR_ERR(sc->ddr_sr));
			goto fail;
		}
	}

	if (!sc->ddr_iso) {
		sc->ddr_iso =
			devm_gpiod_get(dev, "ddr-iso", GPIOD_OUT_LOW);
		if (IS_ERR(sc->ddr_iso)) {
			dev_err(dev, "Could not get pmic_ddr_iso gpio (%ld)\n",
					PTR_ERR(sc->ddr_iso));
			goto fail;
		}
	}

	if (!sc->smps1) {
		sc->smps1 = devm_regulator_get(dev, "s2mpg01_smps1");
		if (IS_ERR(sc->smps1)) {
			dev_err(dev, "failed to get s2mpg01_smps1 supply (%ld)\n",
					PTR_ERR(sc->smps1));
			goto fail;
		}
	}

	if (!sc->smps2) {
		sc->smps2 = devm_regulator_get(dev, "s2mpg01_smps2");
		if (IS_ERR(sc->smps2)) {
			dev_err(dev, "failed to get s2mpg01_smps2 supply (%ld)\n",
					PTR_ERR(sc->smps2));
			goto fail;
		}
	}

	if (!sc->smps3) {
		sc->smps3 = devm_regulator_get(dev, "s2mpg01_smps3");
		if (IS_ERR(sc->smps3)) {
			dev_err(dev, "failed to get s2mpg01_smps3 supply (%ld)\n",
					PTR_ERR(sc->smps3));
			goto fail;
		}
	}

	if (!sc->ldo1) {
		sc->ldo1 = devm_regulator_get(dev, "s2mpg01_ldo1");
		if (IS_ERR(sc->ldo1)) {
			dev_err(dev, "failed to get s2mpg01_ldo1 supply (%ld)\n",
					PTR_ERR(sc->ldo1));
			goto fail;
		}
	}

	if (!sc->ldo2) {
		sc->ldo2 = devm_regulator_get(dev, "s2mpg01_ldo2");
		if (IS_ERR(sc->ldo2)) {
			dev_err(dev, "failed to get s2mpg01_ldo2 supply (%ld)\n",
					PTR_ERR(sc->ldo2));
			goto fail;
		}
	}

	if (!sc->ldo3) {
		sc->ldo3 = devm_regulator_get(dev, "s2mpg01_ldo3");
		if (IS_ERR(sc->ldo3)) {
			dev_err(dev, "failed to get s2mpg01_ldo3 supply (%ld)\n",
					PTR_ERR(sc->ldo3));
			goto fail;
		}
	}

	if (!sc->ldo4) {
		sc->ldo4 = devm_regulator_get(dev, "s2mpg01_ldo4");
		if (IS_ERR(sc->ldo4)) {
			dev_err(dev, "failed to get s2mpg01_ldo4 supply (%ld)\n",
					PTR_ERR(sc->ldo4));
			goto fail;
		}
	}

	if (!sc->ldo5) {
		sc->ldo5 = devm_regulator_get(dev, "s2mpg01_ldo5");
		if (IS_ERR(sc->ldo5)) {
			dev_err(dev, "failed to get s2mpg01_ldo5 supply (%ld)\n",
					PTR_ERR(sc->ldo5));
			goto fail;
		}
	}

	if (!sc->boost_smps1) {
		sc->boost_smps1 =
			devm_regulator_get(dev, "s2mpg01_boost_smps1");
		if (IS_ERR(sc->boost_smps1)) {
			dev_err(dev, "failed to get s2mpg01_boost_smps1 supply (%ld)\n",
					PTR_ERR(sc->boost_smps1));
			goto fail;
		}
	}

	if (!sc->boost_ldo3) {
		sc->boost_ldo3 =
			devm_regulator_get(dev, "s2mpg01_boost_ldo3");
		if (IS_ERR(sc->boost_ldo3)) {
			dev_err(dev, "failed to get s2mpg01_boost_ldo3 supply (%ld)\n",
					PTR_ERR(sc->boost_ldo3));
			goto fail;
		}
	}

	if (ab_register_notifier(sc))
		goto fail;

	return 0;

fail:
	return -ENODEV;
}

