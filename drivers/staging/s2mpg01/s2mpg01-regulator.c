/*
 * Copyright (C) 2017-2018 Google, Inc.
 *
 * Author: Trevor Bunker <trevorbunker@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#include "s2mpg01-core.h"

#define DRIVER_NAME "s2mpg01-regulator"

struct s2mpg01_regulator {
	struct device *dev;
	struct s2mpg01_core *s2mpg01_core;
	struct regulator_dev **rdevs;

	/* bitmask for tracking enabled regulators */
	unsigned long reg_enabled_mask;
};

static struct s2mpg01_regulator *_s2mpg01_regulator;

static int s2mpg01_regulator_get_voltage(struct regulator_dev *rdev);
static int s2mpg01_regulator_enable(struct regulator_dev *rdev);
static int s2mpg01_regulator_disable(struct regulator_dev *rdev);
static int s2mpg01_regulator_is_enabled(struct regulator_dev *rdev);

static struct regulator_ops s2mpg01_regulator_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_ascend,
	.get_voltage = s2mpg01_regulator_get_voltage,
	.enable = s2mpg01_regulator_enable,
	.disable = s2mpg01_regulator_disable,
	.is_enabled = s2mpg01_regulator_is_enabled,
};

/* No support for DVS so just a single voltage level */
static const unsigned int s2mpg01_ldo1_vtbl[] = { 1800000 };
static const unsigned int s2mpg01_ldo2_vtbl[] = { 600000 };
static const unsigned int s2mpg01_ldo3_vtbl[] = { 750000 };
static const unsigned int s2mpg01_ldo4_vtbl[] = { 850000 };
static const unsigned int s2mpg01_ldo5_vtbl[] = { 1800000 };
static const unsigned int s2mpg01_smps1_vtbl[] = { 750000 };
static const unsigned int s2mpg01_smps2_vtbl[] = { 850000 };
static const unsigned int s2mpg01_smps3_vtbl[] = { 1100000 };

static struct regulator_desc
	s2mpg01_regulator_desc[S2MPG01_NUM_REGULATORS] = {
	[S2MPG01_ID_SMPS1] = {
		.name = S2MPG01_REGLTR_NAME_SMPS1,
		.id = S2MPG01_ID_SMPS1,
		.ops = &s2mpg01_regulator_ops,
		.n_voltages = ARRAY_SIZE(s2mpg01_smps1_vtbl),
		.volt_table = s2mpg01_smps1_vtbl,
		.enable_time = 200,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
	},
	[S2MPG01_ID_SMPS2] = {
		.name = S2MPG01_REGLTR_NAME_SMPS2,
		.id = S2MPG01_ID_SMPS2,
		.ops = &s2mpg01_regulator_ops,
		.n_voltages = ARRAY_SIZE(s2mpg01_smps2_vtbl),
		.volt_table = s2mpg01_smps2_vtbl,
		.enable_time = 200,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
	},
	[S2MPG01_ID_SMPS3] = {
		.name = S2MPG01_REGLTR_NAME_SMPS3,
		.id = S2MPG01_ID_SMPS3,
		.ops = &s2mpg01_regulator_ops,
		.n_voltages = ARRAY_SIZE(s2mpg01_smps3_vtbl),
		.volt_table = s2mpg01_smps3_vtbl,
		.enable_time = 200,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
	},
	[S2MPG01_ID_LDO1] = {
		.name = S2MPG01_REGLTR_NAME_LDO1,
		.id = S2MPG01_ID_LDO1,
		.ops = &s2mpg01_regulator_ops,
		.n_voltages = ARRAY_SIZE(s2mpg01_ldo1_vtbl),
		.volt_table = s2mpg01_ldo1_vtbl,
		.enable_time = 200,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
	},
	[S2MPG01_ID_LDO2] = {
		.name = S2MPG01_REGLTR_NAME_LDO2,
		.id = S2MPG01_ID_LDO2,
		.ops = &s2mpg01_regulator_ops,
		.n_voltages = ARRAY_SIZE(s2mpg01_ldo2_vtbl),
		.volt_table = s2mpg01_ldo2_vtbl,
		.enable_time = 200,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
	},
	[S2MPG01_ID_LDO3] = {
		.name = S2MPG01_REGLTR_NAME_LDO3,
		.id = S2MPG01_ID_LDO3,
		.ops = &s2mpg01_regulator_ops,
		.n_voltages = ARRAY_SIZE(s2mpg01_ldo3_vtbl),
		.volt_table = s2mpg01_ldo3_vtbl,
		.enable_time = 200,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
	},
	[S2MPG01_ID_LDO4] = {
		.name = S2MPG01_REGLTR_NAME_LDO4,
		.id = S2MPG01_ID_LDO4,
		.ops = &s2mpg01_regulator_ops,
		.n_voltages = ARRAY_SIZE(s2mpg01_ldo4_vtbl),
		.volt_table = s2mpg01_ldo4_vtbl,
		.enable_time = 200,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
	},
	[S2MPG01_ID_LDO5] = {
		.name = S2MPG01_REGLTR_NAME_LDO5,
		.id = S2MPG01_ID_LDO5,
		.ops = &s2mpg01_regulator_ops,
		.n_voltages = ARRAY_SIZE(s2mpg01_ldo5_vtbl),
		.volt_table = s2mpg01_ldo5_vtbl,
		.enable_time = 200,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
	},
};

static struct regulator_init_data
	s2mpg01_regulator_init_data[S2MPG01_NUM_REGULATORS] = {
	[S2MPG01_ID_SMPS1] = {
		.constraints = {
			.name = "s2mpg01_smps1",
			.valid_ops_mask = REGULATOR_CHANGE_STATUS,
			.min_uV = 750000,
			.max_uV = 750000,
		},
	},
	[S2MPG01_ID_SMPS2] = {
		.constraints = {
			.name = "s2mpg01_smps2",
			.valid_ops_mask = REGULATOR_CHANGE_STATUS,
			.min_uV = 850000,
			.max_uV = 850000,
		},
	},
	[S2MPG01_ID_SMPS3] = {
		.constraints = {
			.name = "s2mpg01_smps3",
			.valid_ops_mask = REGULATOR_CHANGE_STATUS,
			.min_uV = 1100000,
			.max_uV = 1100000,
		},
	},
	[S2MPG01_ID_LDO1] = {
		.constraints = {
			.name = "s2mpg01_ldo1",
			.valid_ops_mask = REGULATOR_CHANGE_STATUS,
			.min_uV = 1800000,
			.max_uV = 1800000,
		},
	},
	[S2MPG01_ID_LDO2] = {
		.constraints = {
			.name = "s2mpg01_ldo2",
			.valid_ops_mask = REGULATOR_CHANGE_STATUS,
			.min_uV = 600000,
			.max_uV = 600000,
		},
	},
	[S2MPG01_ID_LDO3] = {
		.constraints = {
			.name = "s2mpg01_ldo3",
			.valid_ops_mask = REGULATOR_CHANGE_STATUS,
			.min_uV = 750000,
			.max_uV = 750000,
		},
	},
	[S2MPG01_ID_LDO4] = {
		.constraints = {
			.name = "s2mpg01_ldo4",
			.valid_ops_mask = REGULATOR_CHANGE_STATUS,
			.min_uV = 850000,
			.max_uV = 850000,
		},
	},
	[S2MPG01_ID_LDO5] = {
		.constraints = {
			.name = "s2mpg01_ldo5",
			.valid_ops_mask = REGULATOR_CHANGE_STATUS,
			.min_uV = 1800000,
			.max_uV = 1800000,
		},
	},
};

/* get the current voltage of the regulator in microvolts */
static int s2mpg01_regulator_get_voltage(struct regulator_dev *rdev)
{
	struct s2mpg01_regulator *s2mpg01_regulator = rdev_get_drvdata(rdev);
	struct s2mpg01_core *s2mpg01_core = s2mpg01_regulator->s2mpg01_core;
	enum s2mpg01_regulator_ids rid = rdev_get_id(rdev);
	u8 reg_data;
	int vsel, vstep, vbase;
	int ret;

	dev_dbg(s2mpg01_regulator->dev, "%s: rid %d\n", __func__, rid);

	switch (rid) {
	case S2MPG01_ID_SMPS1:
		ret = s2mpg01_read_byte(s2mpg01_core, S2MPG01_REG_BUCK1_OUT,
					&reg_data);
		if (ret)
			return ret;
		vbase = 300000;
		vstep = 6250;
		vsel = reg_data;
		break;
	case S2MPG01_ID_SMPS2:
		ret = s2mpg01_read_byte(s2mpg01_core, S2MPG01_REG_BUCK2_OUT,
					&reg_data);
		if (ret)
			return ret;
		vbase = 300000;
		vstep = 6250;
		vsel = reg_data;
		break;
	case S2MPG01_ID_SMPS3:
		ret = s2mpg01_read_byte(s2mpg01_core, S2MPG01_REG_BUCK3_OUT,
					&reg_data);
		if (ret)
			return ret;
		vbase = 300000;
		vstep = 6250;
		vsel = reg_data;
		break;
	case S2MPG01_ID_LDO1:
		ret = s2mpg01_read_byte(s2mpg01_core, S2MPG01_REG_LDO1_CTRL,
					&reg_data);
		if (ret)
			return ret;
		vbase = 700000;
		vstep = 25000;
		vsel = reg_data & 0x3F;
		break;
	case S2MPG01_ID_LDO2:
		ret = s2mpg01_read_byte(s2mpg01_core, S2MPG01_REG_LDO2_CTRL,
					&reg_data);
		if (ret)
			return ret;
		vbase = 400000;
		vstep = 12500;
		vsel = reg_data & 0x3F;
		break;
	case S2MPG01_ID_LDO3:
		ret = s2mpg01_read_byte(s2mpg01_core, S2MPG01_REG_LDO3_CTRL,
					&reg_data);
		if (ret)
			return ret;
		vbase = 300000;
		vstep = 12500;
		vsel = reg_data & 0x3F;
		break;
	case S2MPG01_ID_LDO4:
		ret = s2mpg01_read_byte(s2mpg01_core, S2MPG01_REG_LDO4_CTRL,
					&reg_data);
		if (ret)
			return ret;
		vbase = 400000;
		vstep = 12500;
		vsel = reg_data & 0x3F;
		break;
	case S2MPG01_ID_LDO5:
		ret = s2mpg01_read_byte(s2mpg01_core, S2MPG01_REG_LDO5_CTRL,
					&reg_data);
		if (ret)
			return ret;
		vbase = 700000;
		vstep = 25000;
		vsel = reg_data & 0x3F;
		break;
	default:
		return -EINVAL;
	}

	dev_dbg(s2mpg01_regulator->dev, "%s: rid %d, returning voltage %d\n",
		__func__, rid, vbase + vsel * vstep);

	return vbase + vsel * vstep;
}

/* enable the regulator */
static int s2mpg01_regulator_enable(struct regulator_dev *rdev)
{
	struct s2mpg01_regulator *s2mpg01_regulator = rdev_get_drvdata(rdev);
	struct s2mpg01_core *s2mpg01_core = s2mpg01_regulator->s2mpg01_core;
	enum s2mpg01_regulator_ids rid = rdev_get_id(rdev);
	int ret;

	dev_dbg(s2mpg01_regulator->dev, "%s: rid %d\n", __func__, rid);

	switch (rid) {
	case S2MPG01_ID_SMPS1:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_BUCK1_CTRL,
					 0xF8);
		break;
	case S2MPG01_ID_SMPS2:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_BUCK2_CTRL,
					 0xD8);
		break;
	case S2MPG01_ID_SMPS3:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_BUCK3_CTRL,
					 0xD8);
		break;
	case S2MPG01_ID_LDO1:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_LDO1_CTRL,
					 0xEC);
		break;
	case S2MPG01_ID_LDO2:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_LDO2_CTRL,
					 0x90);
		break;
	case S2MPG01_ID_LDO3:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_LDO3_CTRL,
					 0xA4);
		break;
	case S2MPG01_ID_LDO4:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_LDO4_CTRL,
					 0xA4);
		break;
	case S2MPG01_ID_LDO5:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_LDO5_CTRL,
					 0xEC);
		break;
	default:
		return -EINVAL;
	}

	if (!ret)
		set_bit(rid, &s2mpg01_regulator->reg_enabled_mask);

	return ret;
}

/* disable the regulator */
static int s2mpg01_regulator_disable(struct regulator_dev *rdev)
{
	struct s2mpg01_regulator *s2mpg01_regulator = rdev_get_drvdata(rdev);
	struct s2mpg01_core *s2mpg01_core = s2mpg01_regulator->s2mpg01_core;
	enum s2mpg01_regulator_ids rid = rdev_get_id(rdev);
	int ret;

	dev_dbg(s2mpg01_regulator->dev, "%s: rid %d\n", __func__, rid);

	switch (rid) {
	case S2MPG01_ID_SMPS1:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_BUCK1_CTRL,
					 0x38);
		break;
	case S2MPG01_ID_SMPS2:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_BUCK2_CTRL,
					 0x18);
		break;
	case S2MPG01_ID_SMPS3:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_BUCK3_CTRL,
					 0x18);
		break;
	case S2MPG01_ID_LDO1:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_LDO1_CTRL,
					 0x2C);
		break;
	case S2MPG01_ID_LDO2:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_LDO2_CTRL,
					 0x10);
		break;
	case S2MPG01_ID_LDO3:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_LDO3_CTRL,
					 0x24);
		break;
	case S2MPG01_ID_LDO4:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_LDO4_CTRL,
					 0x24);
		break;
	case S2MPG01_ID_LDO5:
		ret = s2mpg01_write_byte(s2mpg01_core, S2MPG01_REG_LDO5_CTRL,
					 0x2C);
		break;
	default:
		return -EINVAL;
	}

	if (!ret)
		clear_bit(rid, &s2mpg01_regulator->reg_enabled_mask);

	return ret;
}

/* get regulator enable status */
static int s2mpg01_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct s2mpg01_regulator *s2mpg01_regulator = rdev_get_drvdata(rdev);
	enum s2mpg01_regulator_ids rid = rdev_get_id(rdev);

	dev_dbg(s2mpg01_regulator->dev, "%s: rid %d\n", __func__, rid);

	if ((rid >= 0) && (rid < S2MPG01_NUM_REGULATORS))
		return !!(s2mpg01_regulator->reg_enabled_mask & (1 << rid));
	else
		return -EINVAL;
}

void s2mpg01_regulator_notify(enum s2mpg01_regulator_ids rid,
			      unsigned long event)
{
	if (!_s2mpg01_regulator || !_s2mpg01_regulator->rdevs ||
	    !_s2mpg01_regulator->rdevs[rid])
		return;

	if (!s2mpg01_regulator_is_enabled(_s2mpg01_regulator->rdevs[rid]))
		return;

	dev_err(_s2mpg01_regulator->dev, "%s: rid %d, event 0x%lx\n", __func__,
		rid, event);

	regulator_notifier_call_chain(_s2mpg01_regulator->rdevs[rid], event,
				      NULL);
}
EXPORT_SYMBOL_GPL(s2mpg01_regulator_notify);

/* register a regulator with the kernel regulator framework */
static int
s2mpg01_regulator_register(struct s2mpg01_regulator *s2mpg01_regulator)
{
	struct device *dev = s2mpg01_regulator->dev;
	struct s2mpg01_core *s2mpg01_core = s2mpg01_regulator->s2mpg01_core;
	struct regulator_config cfg = {
		.dev = dev,
		.driver_data = s2mpg01_regulator,
		.regmap = s2mpg01_core->regmap
	};
	struct regulator_dev *rdev;
	int i;

	for (i = 0; i < S2MPG01_NUM_REGULATORS; i++) {
		cfg.init_data = &s2mpg01_regulator_init_data[i];
		rdev = devm_regulator_register(dev,
					       &s2mpg01_regulator_desc[i],
					       &cfg);
		if (IS_ERR(rdev)) {
			dev_err(dev,
				"%s: failed to register regulator %d\n",
				__func__, i);
			return PTR_ERR(rdev);
		}

		*(s2mpg01_regulator->rdevs + i) = rdev;
	}

	return 0;
}

static int s2mpg01_regulator_probe(struct platform_device *pdev)
{
	struct s2mpg01_core *s2mpg01_core = dev_get_drvdata(pdev->dev.parent);
	struct s2mpg01_regulator *s2mpg01_regulator;
	struct device *dev = &pdev->dev;

	s2mpg01_regulator = devm_kzalloc(dev, sizeof(*s2mpg01_regulator),
					 GFP_KERNEL);
	if (!s2mpg01_regulator)
		return -ENOMEM;

	s2mpg01_regulator->dev = dev;
	s2mpg01_regulator->s2mpg01_core = s2mpg01_core;
	_s2mpg01_regulator = s2mpg01_regulator;

	platform_set_drvdata(pdev, s2mpg01_regulator);

	/* initialize and register device regulators */
	s2mpg01_regulator->rdevs =
		devm_kzalloc(dev,
			     S2MPG01_NUM_REGULATORS *
			     sizeof(struct regulator_dev *),
			     GFP_KERNEL);
	if (!s2mpg01_regulator->rdevs) {
		dev_err(dev,
			"%s: could not initialize rdevs array\n",
			__func__);
		return -ENOMEM;
	}

	return s2mpg01_regulator_register(s2mpg01_regulator);
}

static int s2mpg01_regulator_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id s2mpg01_regulator_of_match[] = {
	{ .compatible = "samsung,s2mpg01-regulator", },
	{ }
};
MODULE_DEVICE_TABLE(of, s2mpg01_regulator_of_match);

static struct platform_driver s2mpg01_regulator_driver = {
	.probe = s2mpg01_regulator_probe,
	.remove = s2mpg01_regulator_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = s2mpg01_regulator_of_match,
	},
};
module_platform_driver(s2mpg01_regulator_driver);

MODULE_AUTHOR("Trevor Bunker <trevorbunker@google.com>");
MODULE_DESCRIPTION("S2MPG01 Regulator Driver");
MODULE_LICENSE("GPL");
