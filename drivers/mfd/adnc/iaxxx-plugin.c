/*
 * iaxxx-plugin.c -- IAxxx plugin interface for Plugins
 *
 * Copyright 2017 Knowles Corporation
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/mfd/adnc/iaxxx-core.h>
#include <linux/mfd/adnc/iaxxx-plugin-registers.h>
#include <linux/mfd/adnc/iaxxx-register-defs-srb.h>
#include <linux/mfd/adnc/iaxxx-register-defs-pkg-mgmt.h>
#include <linux/mfd/adnc/iaxxx-system-identifiers.h>
#include "iaxxx.h"

#define IAXXX_BITS_SWAP	32
#define IAXXX_BLK_HEADER_SIZE 4
#define IAXXX_BIN_INFO_SEC_ADDR	0xF1F00000
#define IAXXX_INVALID_FILE ('\0')
#define IAXXX_KW_BITMAP 0x7
#define IAXXX_MAX_VALID_KW_ID 0xffff
#define IAXXX_VQ_PARAM_BLOCK_ID_BASE  (917520)
#define IAXXX_VQ_INST_ID 0

/*
 * Generate package id with 'i' package id and 'p' processor id
 */
#define GEN_PKG_ID(i, p) \
	((i & IAXXX_PKG_MGMT_PKG_PROC_ID_PACKAGE_ID_MASK) | \
	((p << IAXXX_PKG_MGMT_PKG_PROC_ID_PROC_ID_POS) & \
	IAXXX_PKG_MGMT_PKG_PROC_ID_PROC_ID_MASK))

struct pkg_bin_info {
	uint32_t    version;
	uint32_t    entry_point;
	uint32_t    core_id;
	uint32_t    vendor_id;
	uint32_t    text_start_addr;
	uint32_t    text_end_addr;
	uint32_t    ro_data_start_addr;
	uint32_t    ro_data_end_addr;
	uint32_t    data_start_addr;
	uint32_t    data_end_addr;
	uint32_t    bss_start_addr;
	uint32_t    bss_end_addr;
};

struct pkg_mgmt_info {
	uint32_t req;
	uint32_t proc_id;
	uint32_t info;
	uint32_t p_text_addr;
	uint32_t v_text_addr;
	uint32_t text_size;
	uint32_t p_data_addr;
	uint32_t v_data_addr;
	uint32_t data_size;
	uint32_t entry_pt;
	uint32_t error;
};

/*****************************************************************************
 * iaxxx_core_create_plg_common()
 * @brief Create plugin instance
 *
 * @id              Plugin Instance Id
 * @param_id        Param Id
 * @param_val       Param value
 * @block_id        Update block id
 * @static_package  True if the plugin is part of static package
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
static int iaxxx_core_create_plg_common(
		struct device *dev, uint32_t inst_id,
		uint32_t priority, uint32_t pkg_id,
		uint32_t plg_idx, uint8_t block_id,
		bool static_package)
{
	int ret = -EINVAL;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	uint32_t status;
	uint32_t package;
	uint8_t proc_id;

	if (!priv)
		return ret;

	dev_dbg(dev,
		"%s() inst_id=%u prio=%u pkg_id=%u plg_idx=%u blk_id=%u\n",
		__func__, inst_id, priority, pkg_id, plg_idx,
		block_id);

	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);

	inst_id &= IAXXX_PLGIN_ID_MASK;

	package = pkg_id & IAXXX_PKG_ID_MASK;

	/* Check Package is loaded. DO NOT check for
	 * statically loaded packages
	 */
	if (!static_package) {
		if (!(priv->iaxxx_state->pkg[package].pkg_state)) {
			dev_err(dev, "Package 0x%x is not created %s()\n",
				pkg_id, __func__);
			goto core_create_plugin_err;
		}
	}
	/* Check if Plugin exist */
	if (priv->iaxxx_state->plgin[inst_id].plugin_inst_state) {
		dev_err(dev, "Plugin instance 0x%x exist %s()\n",
			inst_id, __func__);
		ret = -EEXIST;
		goto core_create_plugin_err;
	}

	proc_id = IAXXX_BLOCK_ID_TO_PROC_ID(block_id);

	/* Create SysID of Package ID using Package Index
	 * and Proc ID
	 */
	pkg_id  = GEN_PKG_ID(package, proc_id);

	/* Update Package ID of plugin to be created */
	ret = regmap_update_bits(priv->regmap,
		IAXXX_PLUGIN_INS_GRP_ORIGIN_REG(inst_id),
		IAXXX_PLUGIN_INS_GRP_ORIGIN_PKG_ID_MASK,
		pkg_id << IAXXX_PLUGIN_INS_GRP_ORIGIN_PKG_ID_POS);

	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto core_create_plugin_err;
	}
	/* Update Plugin priority */
	ret = regmap_update_bits(priv->regmap,
		IAXXX_PLUGIN_INS_GRP_CTRL_REG(inst_id),
		IAXXX_PLUGIN_INS_GRP_CTRL_PRIORITY_MASK,
		priority << IAXXX_PLUGIN_INS_GRP_CTRL_PRIORITY_POS);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto core_create_plugin_err;
	}

	/* Update Plugin index */
	ret = regmap_update_bits(priv->regmap,
		IAXXX_PLUGIN_INS_GRP_ORIGIN_REG(inst_id),
		IAXXX_PLUGIN_INS_GRP_ORIGIN_PLUGIN_INDEX_MASK,
		plg_idx << IAXXX_PLUGIN_INS_GRP_ORIGIN_PLUGIN_INDEX_POS);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto core_create_plugin_err;
	}

	/* Update Plugin instance id in plg inst header */
	ret = regmap_update_bits(priv->regmap,
		IAXXX_PLUGIN_HDR_CREATE_BLOCK_ADDR(block_id),
		1 << inst_id, 1 << inst_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto core_create_plugin_err;
	}
	ret = iaxxx_send_update_block_request(dev, &status, block_id);
	if (ret) {
		dev_err(dev, "Update blk failed %s()\n", __func__);
		goto core_create_plugin_err;
	}

	priv->iaxxx_state->plgin[inst_id].plugin_inst_state =
		IAXXX_PLUGIN_LOADED;
	priv->iaxxx_state->plgin[inst_id].proc_id = pkg_id;

core_create_plugin_err:
	mutex_unlock(&priv->plugin_lock);
	return ret;
}


/*****************************************************************************
 * iaxxx_core_create_plg()
 * @brief Create plugin instance
 *
 * @id		Plugin Instance Id
 * @param_id	Param Id
 * @param_val	Param value
 * @block_id	Update block id
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_core_create_plg(struct device *dev, uint32_t inst_id,
			uint32_t priority, uint32_t pkg_id,
			uint32_t plg_idx, uint8_t block_id)
{
	return iaxxx_core_create_plg_common(dev, inst_id, priority,
			pkg_id, plg_idx, block_id, false);
}
EXPORT_SYMBOL(iaxxx_core_create_plg);

/*****************************************************************************
 * iaxxx_core_create_plg_static_package()
 * @brief Create plugin instance from statically loaded package
 *
 * @id      Plugin Instance Id
 * @param_id    Param Id
 * @param_val   Param value
 * @block_id    Update block id
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_core_create_plg_static_package(
		struct device *dev, uint32_t inst_id,
		uint32_t priority, uint32_t pkg_id,
		uint32_t plg_idx, uint8_t block_id)
{
	/* Generate package id using package index and
	 * block_id
	 */
	uint32_t proc_id = IAXXX_BLOCK_ID_TO_PROC_ID(block_id);

	pkg_id = GEN_PKG_ID(pkg_id, proc_id);
	return iaxxx_core_create_plg_common(dev, inst_id, priority,
			pkg_id, plg_idx, block_id, true);
}
EXPORT_SYMBOL(iaxxx_core_create_plg_static_package);


/*****************************************************************************
 * iaxxx_core_change_plg_state()
 * @brief Change plugin state to enable/disable
 *
 * @inst_id	Plugin Instance Id
 * @is_enable	0 - Disable, 1 - Enable
 * @block_id	Update block id
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_core_change_plg_state(struct device *dev, uint32_t inst_id,
			uint8_t is_enable, uint8_t block_id)
{
	int ret = -EINVAL;
	uint32_t status = 0;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);

	if (!priv)
		return ret;

	dev_dbg(dev, "%s() inst_id:%u block_id:%u enable:%u\n", __func__,
			inst_id, block_id, is_enable);
	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);

	/* Check plugin instance is created */
	if (!(priv->iaxxx_state->plgin[inst_id].plugin_inst_state)) {
		dev_err(dev, "Plugin instance 0x%x is not created %s()\n",
				inst_id, __func__);
		ret = -EEXIST;
		goto core_change_plg_state_err;
	}

	/* Set enable bit in plugin inst enable header */
	ret = regmap_update_bits(priv->regmap,
		IAXXX_PLUGIN_HDR_ENABLE_BLOCK_ADDR(block_id),
		1 << inst_id,
		is_enable << inst_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto core_change_plg_state_err;
	}
	ret = iaxxx_send_update_block_request(dev, &status, block_id);
	if (ret)
		dev_err(dev, "Update blk failed %s()\n", __func__);

core_change_plg_state_err:
	mutex_unlock(&priv->plugin_lock);
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_change_plg_state);

/*****************************************************************************
 * iaxxx_core_destroy_plg()
 * @brief Destroy plugin instance
 *
 * @inst_id	Plugin Instance Id
 * @block_id	Update block id
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_core_destroy_plg(struct device *dev, uint32_t inst_id,
				uint8_t block_id)
{
	int ret = -EINVAL;
	uint32_t status = 0;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);

	if (!priv)
		return ret;

	dev_dbg(dev, "%s() inst_id:%u block_id:%u\n", __func__,
			inst_id, block_id);
	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);

	/* Check plugin instance is created */
	if (!(priv->iaxxx_state->plgin[inst_id].plugin_inst_state)) {
		dev_err(dev, "Plugin instance 0x%x is not created %s()\n",
				inst_id, __func__);
		ret = -EEXIST;
		goto core_destroy_plg_err;
	}

	/* Clear bit in plugin instance header */
	ret = regmap_update_bits(priv->regmap,
		IAXXX_PLUGIN_HDR_CREATE_BLOCK_ADDR(block_id),
		1 << inst_id,
		0 << inst_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto core_destroy_plg_err;
	}

	ret = iaxxx_send_update_block_request(dev, &status, block_id);
	if (ret) {
		dev_err(dev, "Update blk failed %s()\n", __func__);
	} else
		priv->iaxxx_state->plgin[inst_id].plugin_inst_state =
				    IAXXX_PLUGIN_UNLOADED;

core_destroy_plg_err:
	mutex_unlock(&priv->plugin_lock);
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_destroy_plg);

/*****************************************************************************
 * iaxxx_core_reset_plg()
 * @brief Reset plugin instance
 *
 * @inst_id	Plugin Instance Id
 * @block_id	Update block id
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_core_reset_plg(struct device *dev, uint32_t inst_id,
				uint8_t block_id)
{
	int ret = -EINVAL;
	int rc;
	int status = 0;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);

	if (!priv)
		return ret;

	inst_id &= IAXXX_PLGIN_ID_MASK;
	dev_dbg(dev, "%s() inst_id:%u block_id:%u\n", __func__,
			inst_id, block_id);
	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);
	/* Check plugin instance is created */
	if (!(priv->iaxxx_state->plgin[inst_id].plugin_inst_state)) {
		dev_err(dev, "Plugin instance 0x%x is not created %s()\n",
				inst_id, __func__);
		goto core_reset_plg_err;
	}
	/* Clear bit in plugin instance header */
	ret = regmap_update_bits(priv->regmap,
		IAXXX_PLUGIN_HDR_RESET_BLOCK_ADDR(block_id),
		1 << inst_id,
		1 << inst_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto core_reset_plg_err;
	}
	ret = iaxxx_send_update_block_request(dev, &status, block_id);
	if (ret) {
		dev_err(dev, "Update blk failed %s()\n", __func__);
		if (status) {
			/* Clear bit in plugin instance header */
			rc = regmap_update_bits(priv->regmap,
				IAXXX_PLUGIN_HDR_RESET_BLOCK_ADDR(block_id),
				1 << inst_id, 0);
			if (rc) {
				dev_err(dev, "clear failed %s() %d\n",
						__func__, rc);
				goto core_reset_plg_err;
			}
		}
		goto core_reset_plg_err;
	}

core_reset_plg_err:
	mutex_unlock(&priv->plugin_lock);
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_reset_plg);

/*****************************************************************************
 * iaxxx_core_plg_set_param_by_inst()
 * @brief Set a param in a plugin instance
 *
 * @id		Plugin Instance Id
 * @param_id	Param Id
 * @param_val	Param value
 * @block_id	Update block id
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_core_plg_set_param_by_inst(struct device *dev, uint32_t inst_id,
				uint32_t param_id,
				uint32_t param_val, uint32_t block_id)
{
	int ret = -EINVAL;
	uint32_t status = 0;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	int rc;

	if (!priv)
		return ret;

	dev_dbg(dev, "%s() inst_id=%u param_id=%u blk_id=%u param_val=%u\n",
		__func__, inst_id, param_id, block_id, param_val);

	inst_id &= IAXXX_PLGIN_ID_MASK;
	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);

	/* Plugin instance exists or not */
	if (!priv->iaxxx_state->plgin[inst_id].plugin_inst_state) {
		dev_err(dev, "Plugin instance 0x%x is not created %s()\n",
				inst_id, __func__);
		goto plg_set_param_inst_err;
	}
	ret = regmap_write(priv->regmap,
			IAXXX_PLUGIN_INS_GRP_PARAM_ID_REG(inst_id), param_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto plg_set_param_inst_err;
	}

	ret = regmap_write(priv->regmap,
			IAXXX_PLUGIN_INS_GRP_PARAM_REG(inst_id), param_val);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto plg_set_param_inst_err;
	}

	ret = regmap_update_bits(priv->regmap,
		IAXXX_PLUGIN_HDR_SET_PARAM_REQ_BLOCK_ADDR(block_id),
		1 << inst_id, 1 << inst_id);
	if (ret) {
		dev_err(dev, "update bit failed %s()\n", __func__);
		goto plg_set_param_inst_err;
	}

	ret = iaxxx_send_update_block_request(dev, &status, block_id);
	if (ret) {
		dev_err(dev, "Update blk failed %s()\n", __func__);
		if (status) {
			rc = regmap_update_bits(priv->regmap,
				IAXXX_PLUGIN_HDR_SET_PARAM_REQ_BLOCK_ADDR
				(block_id),
				1 << inst_id, 0);
			if (rc)
				dev_err(dev, "clear bit failed %s() %d\n",
						__func__, rc);
		}
		goto plg_set_param_inst_err;
	}

plg_set_param_inst_err:
	mutex_unlock(&priv->plugin_lock);
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_plg_set_param_by_inst);


/*****************************************************************************
 * iaxxx_core_plg_get_param_by_inst()
 * @brief get a param in a plugin instance
 *
 * @id		Plugin Instance Id
 * @param_id	Param Id
 * @param_val	return param value
 * @block_id	Update block id
 *
 * @ret 0 in case of success, -EINVAL in case of error.
 ****************************************************************************/
int iaxxx_core_plg_get_param_by_inst(struct device *dev, uint32_t inst_id,
				uint32_t param_id,
				uint32_t *param_val, uint32_t block_id)
{
	int ret = -EINVAL;
	int rc;
	uint32_t status = 0;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);

	if (!priv)
		return ret;

	inst_id &= IAXXX_PLGIN_ID_MASK;

	dev_dbg(dev, "%s() inst_id=%u param_id=%u blk_id=%u\n",
		__func__, inst_id, param_id, block_id);

	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);

	/* Plugin instance exists or not */
	if (!priv->iaxxx_state->plgin[inst_id].plugin_inst_state) {
		dev_err(dev, "Plugin instance 0x%x is not created %s()\n",
				inst_id, __func__);
		goto plg_get_param_inst_err;
	}
	ret = regmap_write(priv->regmap,
			IAXXX_PLUGIN_INS_GRP_PARAM_ID_REG(inst_id), param_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto plg_get_param_inst_err;
	}

	ret = regmap_update_bits(priv->regmap,
			IAXXX_PLUGIN_HDR_GET_PARAM_REQ_BLOCK_ADDR(block_id),
			1 << inst_id, 1 << inst_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto plg_get_param_inst_err;
	}

	ret = iaxxx_send_update_block_request(dev, &status, block_id);
	if (ret) {
		dev_err(dev, "Update blk failed %s()\n", __func__);
		if (status) {
			rc = regmap_update_bits(priv->regmap,
				IAXXX_PLUGIN_HDR_GET_PARAM_REQ_BLOCK_ADDR
				(block_id),
				1 << inst_id, 0);
			if (rc)
				dev_err(dev, "clear bit failed %s() %d\n",
						__func__, rc);
		}
		goto plg_get_param_inst_err;
	}

	ret = regmap_read(priv->regmap,
			IAXXX_PLUGIN_INS_GRP_PARAM_REG(inst_id), param_val);
	if (ret) {
		dev_err(dev, "read failed %s()\n", __func__);
		goto plg_get_param_inst_err;
	}

plg_get_param_inst_err:
	mutex_unlock(&priv->plugin_lock);
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_plg_get_param_by_inst);

/*****************************************************************************
 * iaxxx_core_set_create_cfg()
 * @brief Set a param in a plugin instance
 *
 * @inst_id	Plugin Instance Id
 * @cfg_size	Create cfg size
 * @cfg_val	Config val
 * @block_id	Update block id
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_core_set_create_cfg(struct device *dev, uint32_t inst_id,
			uint32_t cfg_size, uint64_t cfg_val, uint32_t block_id,
			char *file)
{
	int ret = -EINVAL;
	int status;
	uint32_t reg_addr;
	uint32_t val;
	uint32_t reg_val;
	const struct firmware *fw = NULL;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	uint8_t *data = NULL;

	if (!priv)
		return ret;

	inst_id &= IAXXX_PLGIN_ID_MASK;

	dev_dbg(dev, "%s() inst_id=%u cfg_size=%u blk_id=%u\n",
		__func__, inst_id, cfg_size, block_id);

	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);
	/* If plugin instance already exist */
	if (priv->iaxxx_state->plgin[inst_id].plugin_inst_state) {
		dev_err(dev, "Plugin instance 0x%x already exist %s()\n",
				inst_id, __func__);
		ret = -EEXIST;
		goto set_create_cfg_err;
	}

	if (file[0] != IAXXX_INVALID_FILE) {
		dev_dbg(dev, "%s() %s\n", __func__, file);
		ret = request_firmware(&fw, file, priv->dev);
		if (ret) {
			dev_err(dev, "Firmware file not found = %d\n", ret);
			ret = -EINVAL;
			goto set_create_cfg_err;
		}
		cfg_size = fw->size;
		dev_dbg(dev, "%s() cfg_size %d\n", __func__, cfg_size);
	}
	if (cfg_size > sizeof(uint32_t)) {
		if (file[0] == IAXXX_INVALID_FILE) {
			dev_dbg(dev, "%s() %llx\n", __func__, cfg_val);
			/* MSB word should be the first word to be written */
			cfg_val = (cfg_val >> IAXXX_BITS_SWAP) |
				(cfg_val << IAXXX_BITS_SWAP);
			dev_dbg(dev, "%s() cfg_val 0x%llx\n",
					__func__, cfg_val);
		} else {
			data = kmalloc(cfg_size, GFP_KERNEL);
			iaxxx_copy_le32_to_cpu(data, fw->data, cfg_size);
		}

		/* Write to the ParamBlkCtrl register */
		val = (((cfg_size >> 2) <<
			IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_BLK_SIZE_POS) &
			IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_BLK_SIZE_MASK) |
			((inst_id <<
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_INSTANCE_ID_POS) &
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_INSTANCE_ID_MASK) |
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_SET_BLK_REQ_MASK |
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_IS_CREATION_CFG_MASK;

		ret = regmap_write(priv->regmap,
			IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_ADDR(block_id),
			val);
		if (ret) {
			dev_err(dev, "write failed %s()\n", __func__);
			goto set_create_cfg_err;
		}

		ret = iaxxx_send_update_block_request(dev, &status, block_id);
		if (ret) {
			dev_err(dev, "Update blk failed %s()\n", __func__);
			goto set_create_cfg_err;
		}

		ret = regmap_read(priv->regmap,
			IAXXX_PLUGIN_HDR_PARAM_BLK_ADDR_BLOCK_ADDR(block_id),
			&reg_addr);
		if (ret) {
			dev_err(dev, "read failed %s()\n", __func__);
			goto set_create_cfg_err;
		}
		pr_debug("%s() Configuration address %x\n", __func__, reg_addr);

		if (priv->raw_write) {
			if (file[0] == IAXXX_INVALID_FILE)
				ret = priv->raw_write(dev, &reg_addr, &cfg_val,
							sizeof(cfg_val));
			else {
				ret = priv->raw_write(dev, &reg_addr, data,
							cfg_size);
				kfree(data);
			}
			if (ret) {
				dev_err(dev, "Blk write failed %s()\n",
						__func__);
				goto set_create_cfg_err;
			}
		} else {
			dev_err(dev, "Raw blk write failed %s()\n", __func__);
			goto set_create_cfg_err;
		}
	} else {
		if (file[0] == IAXXX_INVALID_FILE)
			reg_val = (uint32_t)cfg_val;
		else
			iaxxx_copy_le32_to_cpu(&reg_val, fw->data, cfg_size);
		pr_debug("%s() reg_val 0x%x\n", __func__, reg_val);

		ret = regmap_write(priv->regmap,
			IAXXX_PLUGIN_INS_GRP_CREATION_CFG_REG(inst_id),
			reg_val);
		if (ret) {
			dev_err(dev, "write failed %s()\n", __func__);
			goto set_create_cfg_err;
		}
	}

set_create_cfg_err:
	if (fw)
		release_firmware(fw);
	mutex_unlock(&priv->plugin_lock);
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_set_create_cfg);

int iaxxx_core_set_param_blk(
			struct device *dev,
			uint32_t inst_id, uint32_t blk_size,
			const void *ptr_blk, uint32_t block_id,
			uint32_t param_blk_id)
{
	int ret = -EINVAL;
	uint32_t status = 0;
	int rc;
	uint32_t reg_addr;
	uint32_t reg_val;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);

	if (!priv)
		return ret;

	inst_id &= IAXXX_PLGIN_ID_MASK;
	dev_dbg(dev, "%s() inst_id=%u blk_size=%u blk_id=%u id=%u\n",
		__func__, inst_id, blk_size, block_id, param_blk_id);
	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);
	/* Plugin instance exists or not */
	if (!priv->iaxxx_state->plgin[inst_id].plugin_inst_state) {
		dev_err(dev, "Plugin instance 0x%x is not created %s()\n",
				inst_id, __func__);
		goto set_param_blk_err;
	}

	/* Write the PluginHdrParamBlkCtrl register */
	/* The block size is divided by 4 here because this function gets it
	 * as block size in bytes but firmware expects in 32bit words.
	 */
	reg_val =  (((blk_size >> 2) <<
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_BLK_SIZE_POS) &
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_BLK_SIZE_MASK);
	reg_val |= ((inst_id <<
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_INSTANCE_ID_POS) &
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_INSTANCE_ID_MASK);
	reg_val |= IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_SET_BLK_REQ_MASK;
	ret = regmap_write(priv->regmap,
			IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_ADDR(block_id),
			reg_val);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto set_param_blk_err;
	}

	ret = regmap_write(priv->regmap,
		IAXXX_PLUGIN_HDR_PARAM_BLK_HDR_BLOCK_ADDR(block_id),
		param_blk_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto set_param_blk_err;
	}

	ret = iaxxx_send_update_block_request(dev, &status, block_id);
	if (ret) {
		dev_err(dev, "Update blk failed after id (%u) config %s()\n",
				param_blk_id, __func__);
		if (status) {
			rc = regmap_update_bits(priv->regmap,
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_ADDR(block_id),
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_BLK_SIZE_MASK |
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_INSTANCE_ID_MASK |
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_SET_BLK_REQ_MASK,
			0);
			if (rc) {
				dev_err(dev, "clear failed %s() %d\n",
					__func__, rc);
				goto set_param_blk_err;
			}
		}
		goto set_param_blk_err;
	}
	ret = regmap_read(priv->regmap,
			IAXXX_PLUGIN_HDR_PARAM_BLK_ADDR_BLOCK_ADDR(block_id),
			&reg_addr);
	if (ret) {
		dev_err(dev, "read failed %s()\n", __func__);
		goto set_param_blk_err;
	}

	if (priv->raw_write) {
		ret = priv->raw_write(dev, &reg_addr, ptr_blk,
				blk_size);
		if (ret) {
			dev_err(dev, "Raw blk write failed %s()\n", __func__);
			goto set_param_blk_err;
		}
	} else {
		dev_err(dev, "Raw blk write not supported  %s()\n", __func__);
		goto set_param_blk_err;
	}

	/* The block size is divided by 4 here because this function gets it
	 * as block size in bytes but firmware expects in 32bit words.
	 */
	reg_val =  (((blk_size >> 2)
		<< IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_BLK_SIZE_POS) &
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_BLK_SIZE_MASK);
	reg_val |= ((inst_id <<
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_INSTANCE_ID_POS) &
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_INSTANCE_ID_MASK);
	reg_val |= IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_SET_BLK_DONE_MASK;
	ret = regmap_write(priv->regmap,
			IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_ADDR(block_id),
			reg_val);

	ret = iaxxx_send_update_block_request(dev, &status, block_id);
	if (ret) {
		dev_err(dev,
		"Update blk failed after plugin ctrl block config %s()\n",
		__func__);
		if (status) {
			rc = regmap_update_bits(priv->regmap,
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_ADDR(block_id),
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_BLK_SIZE_MASK |
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_INSTANCE_ID_MASK |
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_SET_BLK_DONE_MASK,
			0);
			if (rc)
				dev_err(dev, "clear failed %s() %d\n",
					__func__, rc);
			}
	}
set_param_blk_err:
	mutex_unlock(&priv->plugin_lock);
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_set_param_blk);

int iaxxx_core_set_param_blk_from_file(
			struct device *dev,
			uint32_t inst_id,
			uint32_t block_id,
			uint32_t param_blk_id,
			const char *file)
{
	int ret = -EINVAL;
	uint8_t *data = NULL;
	const struct firmware *fw = NULL;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);

	if (!priv)
		return ret;

	if (file && IAXXX_INVALID_FILE != file[0]) {
		ret = request_firmware(&fw, file, priv->dev);
		if (ret) {
			dev_err(dev, "Firmware file not found = %d\n", ret);
			ret = -EINVAL;
			goto iaxxx_core_set_param_blk_from_file_err;
		}
		data = kmalloc(fw->size, GFP_KERNEL);
		if (!data)
			goto iaxxx_core_set_param_blk_from_file_err;
		iaxxx_copy_le32_to_cpu(data, fw->data, fw->size);
		ret = iaxxx_core_set_param_blk(
				dev, inst_id, fw->size, fw->data,
				block_id, param_blk_id);
	}

iaxxx_core_set_param_blk_from_file_err:
	kfree(data);
	if (fw)
		release_firmware(fw);
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_set_param_blk_from_file);

/*****************************************************************************
 * iaxxx_core_set_event()
 * @brief Write the event enable mask to a plugin instance
 *
 * @inst_id		Plugin Instance Id
 * @event_enable_mask	Event enable mask
 * @block_id		Update block id
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_core_set_event(struct device *dev, uint8_t inst_id,
			uint32_t event_enable_mask, uint32_t block_id)
{
	int ret = -EINVAL;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	uint32_t status = 0;
	int rc;

	if (!priv)
		return ret;

	inst_id &= IAXXX_PLGIN_ID_MASK;
	dev_dbg(dev, "%s() inst_id:%u block_id:%u event_en_mask:%x\n",
			__func__, inst_id, block_id, event_enable_mask);
	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);
	/* Plugin instance exists or not */
	if (!priv->iaxxx_state->plgin[inst_id].plugin_inst_state) {
		dev_err(dev, "Plugin instance 0x%x is not created %s()\n",
				inst_id, __func__);
		goto set_event_err;
	}
	ret = regmap_write(priv->regmap,
		IAXXX_PLUGIN_INS_GRP_EVT_EN_REG(inst_id),
		event_enable_mask);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto set_event_err;
	}
	ret = regmap_update_bits(priv->regmap,
		IAXXX_PLUGIN_HDR_EVT_UPDATE_BLOCK_ADDR(block_id),
		1 << inst_id, 1 << inst_id);
	if (ret)
		dev_err(dev, "update bit failed %s()\n", __func__);

	ret = iaxxx_send_update_block_request(dev, &status, block_id);
	if (ret) {
		dev_err(dev, "Update blk failed %s()\n", __func__);
		if (status) {
			rc = regmap_update_bits(priv->regmap,
				IAXXX_PLUGIN_HDR_EVT_UPDATE_BLOCK_ADDR
				(block_id),
				1 << inst_id, 0);
			if (rc)
				dev_err(dev, "clear failed %s() %d\n",
						__func__, rc);
		}
	}

set_event_err:
	mutex_unlock(&priv->plugin_lock);
	return ret;

}
EXPORT_SYMBOL(iaxxx_core_set_event);

static int write_pkg_info(bool update, struct iaxxx_priv *priv, uint32_t pkg_id,
		struct pkg_bin_info bin_info, struct pkg_mgmt_info *pkg)
{
	int rc;
	struct device *dev = priv->dev;
	uint32_t status;
	uint32_t block_id;

	pkg_id &= IAXXX_PKG_ID_MASK;
	dev_dbg(dev, "Text:start:0x%x end:0x%x\nRO data:start 0x%x end:0x%x\n",
		bin_info.text_start_addr, bin_info.text_end_addr,
		bin_info.ro_data_start_addr, bin_info.ro_data_end_addr);
	dev_dbg(dev, "Data:start 0x%x end 0x%x\nBSS:start 0x%x end 0x%x\n",
			bin_info.data_start_addr, bin_info.data_end_addr,
			bin_info.bss_start_addr, bin_info.bss_end_addr);
	if (update) {
		pkg->req = 1 << IAXXX_PKG_MGMT_PKG_REQ_LOAD_POS;
		pkg->proc_id = GEN_PKG_ID(pkg_id, bin_info.core_id);
		pkg->info = bin_info.core_id |
			(bin_info.vendor_id <<
			 IAXXX_PKG_MGMT_PKG_INFO_VENDOR_ID_POS);
		pkg->v_text_addr = bin_info.text_start_addr;
		pkg->text_size = bin_info.text_end_addr -
			bin_info.text_start_addr;
		pkg->v_data_addr = bin_info.ro_data_start_addr;
		pkg->data_size = bin_info.bss_end_addr -
			bin_info.ro_data_start_addr;
		pkg->entry_pt = bin_info.entry_point;
	} else
		pkg->req = 1;
	/* Write Package Binary information */
	rc = regmap_bulk_write(priv->regmap, IAXXX_PKG_MGMT_PKG_REQ_ADDR, pkg,
					sizeof(struct pkg_mgmt_info) >> 2);
	if (rc) {
		dev_err(dev, "Pkg info write fail %s()\n", __func__);
		return rc;
	}
	block_id = IAXXX_PROC_ID_TO_BLOCK_ID(bin_info.core_id);
	rc = iaxxx_send_update_block_request(dev, &status, block_id);
	if (rc) {
		dev_err(dev, "Update blk failed %s()\n", __func__);
		return rc;
	}
	return rc;
}

static uint32_t get_physical_address(uint32_t addr,
		uint32_t text, uint32_t data, struct pkg_bin_info bin_info)
{
	/* Calculate the physical address to write to */
	if ((addr >= bin_info.text_start_addr)
			&& (addr <= bin_info.text_end_addr))
		return (text + (addr - bin_info.text_start_addr));
	else
		return (data + (addr - bin_info.ro_data_start_addr));
}

static int iaxxx_download_pkg(struct iaxxx_priv *priv,
		const struct firmware *fw, uint32_t pkg_id, uint32_t *proc_id)
{
	const uint8_t *data;
	struct device *dev = priv->dev;
	size_t file_section_bytes;
	struct firmware_file_header header;
	struct firmware_section_header file_section = { 0x0, 0x0 };
	/* Checksum variable */
	unsigned int sum1 = 0xffff;
	unsigned int sum2 = 0xffff;
	struct pkg_bin_info bin_info = {0};
	uint32_t *word_data;
	int i, j;
	int rc = 0;
	uint32_t text_phy_addr = 0;
	uint32_t data_phy_addr = 0;
	uint8_t *buf_data;
	struct pkg_mgmt_info pkg = {0};

	dev_dbg(dev, "%s()\n", __func__);
	/* File header */
	if (sizeof(header) > fw->size) {
		dev_err(dev, "Bad package binary file (too small)\n");
		return -EINVAL;
	}
	iaxxx_copy_le32_to_cpu(&header, fw->data, sizeof(header));
	data = fw->data + sizeof(header);

	/* Verify the file header */
	rc = iaxxx_verify_fw_header(dev, &header);
	if (rc) {
		dev_err(dev, "Bad Package binary file\n");
		return rc;
	}
	/* Include file header fields as part of the checksum */
	CALC_FLETCHER16(header.number_of_sections, sum1, sum2);
	CALC_FLETCHER16(header.entry_point, sum1, sum2);

	/* Find the Binary info section */
	for (i = 0; i < header.number_of_sections; i++) {
		/* Load the next data section */
		if (((data - fw->data) + sizeof(file_section)) > fw->size)
			return -EINVAL;
		iaxxx_copy_le32_to_cpu
			(&file_section, data, sizeof(file_section));
		data += sizeof(file_section);
		/* Check for the magic number for the start of info section */
		if (file_section.start_address == IAXXX_BIN_INFO_SEC_ADDR) {
			/* Include section header fields in the checksum */
			CALC_FLETCHER16(file_section.length, sum1, sum2);
			CALC_FLETCHER16(file_section.start_address, sum1, sum2);
			if (((data - fw->data) + sizeof(bin_info))
				> fw->size)
				return -EINVAL;
			iaxxx_copy_le32_to_cpu
				(&bin_info, data, sizeof(bin_info));
			word_data = (uint32_t *)&bin_info;
			for (j = 0 ; j < file_section.length; j++)
				CALC_FLETCHER16(word_data[j], sum1, sum2);
			data += sizeof(bin_info);
			rc = write_pkg_info(true, priv, pkg_id, bin_info, &pkg);
			if (rc) {
				dev_err(dev, "%s() Pkg info error\n", __func__);
				return rc;
			}
			break;
		} else if (file_section.length > 0)
			data += file_section.length * sizeof(uint32_t);
	}
	/* Read text and data physical address */
	rc = regmap_read(priv->regmap,
			IAXXX_PKG_MGMT_PKG_IADDR_P_ADDR, &text_phy_addr);
	if (rc) {
		dev_err(dev, "Text physical addr read failed %s %d()\n",
								__func__, rc);
		return rc;
	}
	rc = regmap_read(priv->regmap,
			IAXXX_PKG_MGMT_PKG_DADDR_P_ADDR, &data_phy_addr);
	if (rc) {
		dev_err(dev, "Data physical addr read failed %s %d()\n",
								__func__, rc);
		return rc;
	}
	dev_dbg(dev, "%s() Text physical addr:0x%x Data physical addr 0x%x\n",
					__func__, text_phy_addr, data_phy_addr);

	data = fw->data + sizeof(header);
	/* Download sections except binary info and checksum */
	for (i = 0; i < header.number_of_sections; i++) {
		if (((data - fw->data) + sizeof(file_section)) > fw->size)
			return -EINVAL;
		iaxxx_copy_le32_to_cpu
			(&file_section, data, sizeof(file_section));
		data += sizeof(file_section);
		dev_dbg(dev, "%s() Section%d addr %x length %x\n", __func__, i,
			file_section.start_address, file_section.length);
		if (file_section.start_address == IAXXX_BIN_INFO_SEC_ADDR)
			data += sizeof(bin_info);
		else if (file_section.length) {
			file_section_bytes = file_section.length * sizeof(u32);
			/* Include section header fields in the checksum */
			CALC_FLETCHER16(file_section.length, sum1, sum2);
			CALC_FLETCHER16(file_section.start_address, sum1, sum2);
			file_section.start_address =
				get_physical_address(file_section.start_address,
					text_phy_addr, data_phy_addr, bin_info);
			dev_dbg(dev, "%s() Physical address %x\n",
					__func__, file_section.start_address);
			buf_data = kcalloc(file_section.length,
					sizeof(uint32_t), GFP_KERNEL);
			if (((data - fw->data) + (file_section.length
				* sizeof(uint32_t))) > fw->size)
				return -EINVAL;
			iaxxx_copy_le32_to_cpu(buf_data, data,
					file_section.length * sizeof(uint32_t));
			word_data = (uint32_t *)buf_data;
			for (j = 0 ; j < file_section.length; j++)
				CALC_FLETCHER16(word_data[j], sum1, sum2);
			rc = iaxxx_download_section(priv, data, &file_section);
			data += file_section_bytes;
			kfree(buf_data);
		}
	}
	/* If the last section length is 0, then verify the checksum */
	if (file_section.length == 0) {
		uint32_t checksum = (sum2 << 16) | sum1;

		dev_info(dev, "Expected checksum = 0x%.08X\n", checksum);
		if (checksum != file_section.start_address) {
			rc = -EINVAL;
			dev_err(dev, "%s(): mismatch 0x%.08X != 0x%.08X\n",
				__func__, checksum, file_section.start_address);
		}
	}
	/* Write zeros to BSS region */
	if (bin_info.bss_start_addr != bin_info.bss_end_addr) {
		file_section.start_address = data_phy_addr +
			(bin_info.bss_start_addr - bin_info.ro_data_start_addr);
		file_section.length = (bin_info.bss_end_addr
				- bin_info.bss_start_addr) >> 2;
		buf_data = kcalloc(file_section.length, sizeof(uint32_t),
								GFP_KERNEL);
		rc = iaxxx_download_section(priv, buf_data, &file_section);
		kfree(buf_data);
	}
	/* Write to Package Management ARB */
	rc = write_pkg_info(false, priv, pkg_id, bin_info, &pkg);
	if (rc) {
		dev_err(dev, "%s() Pkg info error\n", __func__);
		return rc;
	}
	*proc_id = pkg.proc_id;
	return 0;
}

static int iaxxx_unload_pkg(struct iaxxx_priv *priv, uint32_t pkg_id,
			uint32_t proc_id)
{
	struct device *dev = priv->dev;
	uint32_t status;
	uint32_t block_id;
	int rc = 0;

	uint32_t proc_pkg_id = GEN_PKG_ID(pkg_id, proc_id);

	/* Write the package id and proc id */
	rc = regmap_write(priv->regmap, IAXXX_PKG_MGMT_PKG_PROC_ID_ADDR,
				proc_pkg_id);
	if (rc) {
		dev_err(dev,
			"%s() Write to package id (%d) register failed\n",
			__func__, pkg_id);
		return rc;
	}

	/* Write the request to unload */
	rc = regmap_write(priv->regmap, IAXXX_PKG_MGMT_PKG_REQ_ADDR,
				IAXXX_PKG_MGMT_PKG_REQ_UNLOAD_MASK);
	if (rc) {
		dev_err(dev,
			"%s() Write to package (%d) request register failed\n",
			__func__, pkg_id);
		return rc;
	}

	block_id = IAXXX_PROC_ID_TO_BLOCK_ID(proc_id);
	rc = iaxxx_send_update_block_request(dev, &status, block_id);
	if (rc) {
		dev_err(dev, "Update blk failed %s()\n", __func__);
		return rc;
	}
	return 0;
}

/*****************************************************************************
 * iaxxx_package_load()
 * @brief Load the package
 *
 * @pkg_name	Package binary name
 * @pkg_id	Package Id
 * @proc_id	Package Id and Core Id
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_package_load(struct device *dev, const char *pkg_name,
					uint32_t pkg_id, uint32_t *proc_id)
{
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	const struct firmware *fw = NULL;
	int rc = -EINVAL;

	dev_info(dev, "%s()\n", __func__);

	if (!pkg_name) {
		dev_err(dev, "%s() Package name is NULL\n", __func__);
		return -EINVAL;
	}
	dev_info(dev, "Download Package %s\n", pkg_name);

	pkg_id &= IAXXX_PKG_ID_MASK;
	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);
	/* If package already exist */
	if (priv->iaxxx_state->pkg[pkg_id].pkg_state) {
		dev_err(dev, "Package 0x%x already exist %s()\n",
				pkg_id, __func__);
		*proc_id = priv->iaxxx_state->pkg[pkg_id].proc_id;
		rc = -EEXIST;
		goto out;
	}
	rc = request_firmware(&fw, pkg_name, priv->dev);
	if (rc) {
		dev_err(dev, "Firmware file %s not found rc = %d\n",
							pkg_name, rc);
		goto out;
	}
	rc = iaxxx_download_pkg(priv, fw, pkg_id, proc_id);
	if (rc) {
		dev_err(dev, "%s() pkg load fail %d\n", __func__, rc);
		rc = -EINVAL;
		goto out;
	}
	priv->iaxxx_state->pkg[pkg_id].pkg_state = IAXXX_PKG_LOADED;
	priv->iaxxx_state->pkg[pkg_id].proc_id = *proc_id;
out:
	release_firmware(fw);
	mutex_unlock(&priv->plugin_lock);
	return rc;
}
EXPORT_SYMBOL(iaxxx_package_load);

/*****************************************************************************
 * iaxxx_package_unload()
 * @brief Load the package
 *
 * @pkg_id	Package Id
 * @proc_id	Process Id
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_package_unload(struct device *dev,
			int32_t pkg_id,
			uint32_t proc_id)
{
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	int rc = -EINVAL;

	dev_info(dev, "%s() pkg_id:0x%x proc_id:%u\n", __func__,
			pkg_id, proc_id);

	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);

	pkg_id &= IAXXX_PKG_ID_MASK;
	if (priv->iaxxx_state->pkg[pkg_id].pkg_state != IAXXX_PKG_LOADED) {
		dev_err(dev, "%s() pkg not loaded already %d\n",
			__func__, pkg_id);
		goto out;
	}

	rc = iaxxx_unload_pkg(priv, pkg_id, proc_id);
	if (rc) {
		dev_err(dev, "%s() pkg unload fail %d\n", __func__, rc);
		goto out;
	}
	priv->iaxxx_state->pkg[pkg_id].pkg_state = IAXXX_PKG_UNLOADED;
	dev_info(dev, "Package %d unloaded.\n", pkg_id);
out:
	mutex_unlock(&priv->plugin_lock);
	return rc;
}
EXPORT_SYMBOL(iaxxx_package_unload);

int iaxxx_core_get_param_blk(
		struct device *dev,
		uint32_t  inst_id,
		uint32_t  block_id,
		uint32_t  param_blk_id,
		uint32_t *getparam_block_data,
		uint32_t  getparam_block_size_in_words)
{
	int ret;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	uint32_t write_val, read_val, getparam_block_size;
	uint32_t status = 0;

	if (!priv || !getparam_block_data || !getparam_block_size_in_words)
		return -EINVAL;

	dev_dbg(dev, "%s() inst_id=%u blk_size=%u blk_id=%u param_blk_id=%u\n",
		__func__, inst_id, getparam_block_size_in_words, block_id,
		param_blk_id);

	inst_id &= IAXXX_PLGIN_ID_MASK;

	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);

	/* Check if plugin exists */
	if (!priv->iaxxx_state->plgin[inst_id].plugin_inst_state) {
		dev_err(dev, "Plugin instance 0x%x does not exist! %s()\n",
				inst_id, __func__);
		ret = -EEXIST;
		goto iaxxx_core_get_param_error;
	}

	/* Write the PluginHdrParamBlkCtrl register
	 * to request the parameter block.
	 */
	write_val = ((inst_id <<
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_INSTANCE_ID_POS) &
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_INSTANCE_ID_MASK);
	write_val |= IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_GET_BLK_REQ_MASK;

	ret = regmap_write(priv->regmap,
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_ADDR(block_id),
		write_val);
	if (ret) {
		dev_err(dev, "getparamblk request failed %s()\n", __func__);
		goto iaxxx_core_get_param_error;
	}

	ret = regmap_write(priv->regmap,
		IAXXX_PLUGIN_HDR_PARAM_BLK_HDR_BLOCK_ADDR(block_id),
		param_blk_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		goto iaxxx_core_get_param_error;
	}

	ret = iaxxx_send_update_block_request(dev, &status, block_id);
	if (ret) {
		dev_err(dev, "Update blk failed(%x) after GET_BLK_REQ %s()\n",
				status, __func__);
		goto iaxxx_core_get_param_error;
	}

	/* Get block size of parameter block to read and validate it */
	ret = regmap_read(priv->regmap,
			IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_ADDR(block_id),
			&read_val);
	if (ret) {
		dev_err(dev, "getparamblk blksize failed %s()\n", __func__);
		goto iaxxx_core_get_param_error;
	}

	getparam_block_size = (read_val &
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_BLK_SIZE_MASK)
		>> IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_BLK_SIZE_POS;

	if (!getparam_block_size ||
		getparam_block_size > getparam_block_size_in_words) {
		dev_err(dev, "invalid getparam blocksize %s()\n", __func__);
		ret = -EINVAL;
		goto iaxxx_core_get_param_error;
	}

	/* Get parameter block address to read */
	ret = regmap_read(priv->regmap,
			IAXXX_PLUGIN_HDR_PARAM_BLK_ADDR_BLOCK_ADDR(block_id),
			&read_val);
	if (ret) {
		dev_err(dev, "getparamblk addr failed %s()\n", __func__);
		goto iaxxx_core_get_param_error;
	}

	/* read the block from the address */
	ret = priv->bulk_read(priv->dev,
		read_val, getparam_block_data,
		getparam_block_size_in_words);

	if (ret != getparam_block_size_in_words) {
		dev_err(dev, "getparamblk read failed %s()\n", __func__);
		goto iaxxx_core_get_param_error;
	}

	/* Write the param block done */
	write_val = ((inst_id <<
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_INSTANCE_ID_POS) &
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_INSTANCE_ID_MASK);
	write_val |=
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_0_GET_BLK_DONE_MASK;

	ret = regmap_write(priv->regmap,
		IAXXX_PLUGIN_HDR_PARAM_BLK_CTRL_BLOCK_ADDR(block_id),
		write_val);
	if (ret) {
		dev_err(dev, "getparamblk done failed %s()\n", __func__);
		goto iaxxx_core_get_param_error;
	}

	ret = iaxxx_send_update_block_request(dev, &status, block_id);
	if (ret) {
		dev_err(dev, "Update blk failed(%x) after GET_BLK_DONE %s()\n",
				status, __func__);
		goto iaxxx_core_get_param_error;
	}
	ret = 0;

iaxxx_core_get_param_error:
	mutex_unlock(&priv->plugin_lock);
	return ret;

}
EXPORT_SYMBOL(iaxxx_core_get_param_blk);
