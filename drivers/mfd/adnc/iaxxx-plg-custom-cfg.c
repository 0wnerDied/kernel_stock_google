/*
 * iaxxx-plg-custom-cfg.c -- IAxxx interface for custom configuring
 * plugins
 *
 * Copyright 2018 Knowles Corporation
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
#include <linux/delay.h>
#include <linux/mfd/adnc/iaxxx-core.h>
#include <linux/mfd/adnc/iaxxx-plugin-registers.h>
#include <linux/mfd/adnc/iaxxx-register-defs-srb.h>
#include <linux/mfd/adnc/iaxxx-register-defs-pkg-mgmt.h>
#include "iaxxx.h"

#define IAXXX_INVALID_FILE ('\0')

#define CHUNK_OVERHEAD_SIZE_IN_WORDS 3
#define CHUNK_MIN_COMMAND_SIZE_ALLOWED_IN_WORDS 2
#define CHUNK_MAX_COMMAND_SIZE_IN_WORDS 200
#define CHUNK_CMD_START_INDEX 2
#define CHUNK_SIZE_IN_WORDS (CHUNK_MAX_COMMAND_SIZE_IN_WORDS \
	+ CHUNK_OVERHEAD_SIZE_IN_WORDS)

/* The allowed command size is less than 1 because
 * we always have to add a word with 0-value at the
 * end of every chunk.
 */
#define CHUNK_MAX_COMMAND_SIZE_ALLOWED_IN_WORDS  \
	(CHUNK_MAX_COMMAND_SIZE_IN_WORDS-1)

#define PROXY_FUNCTION_ID_3RDPARTY_CHUNK  112
#define MAX_STATUS_RETRIES 5
#define CHUNK_RESPONSE_SIZE_IN_WORDS 4
#define CHUNK_RESPONSE_ERRORCODE_INDEX 1
#define FW_ERROR_CODE_BUSY 4

static int read_plugin_error(
		struct device  *dev,
		const uint32_t  block_id,
		uint32_t *error,
		uint8_t  *error_instance)
{
	int ret;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	uint32_t reg_val;

	if (!priv)
		return -EINVAL;

	ret = regmap_read(priv->regmap,
		IAXXX_PLUGIN_HDR_ERROR_BLOCK_ADDR(block_id),
		&reg_val);
	if (ret) {
		dev_err(dev, "read plugin error failed %s()\n", __func__);
		goto read_plugin_error_err;
	}

	*error = reg_val;

	ret = regmap_read(priv->regmap,
		IAXXX_PLUGIN_HDR_ERROR_INS_ID_BLOCK_ADDR(block_id),
		&reg_val);
	if (ret) {
		dev_err(dev, "read plugin error instance failed %s()\n",
			__func__);
		goto read_plugin_error_err;
	}
	*error_instance = (uint8_t) reg_val;

read_plugin_error_err:
	return ret;

}


static uint32_t calculate_crc(const uint32_t *pBuf, const uint32_t nLen)
{
	uint32_t i;
	uint32_t crc = 0;

	crc = 0U;
	for (i = 0U; i < nLen; i++)
		crc ^= pBuf[i];

	return crc;
}

static int send_chunk_to_plugin(
		struct device *dev,
		const uint32_t  inst_id,
		const uint32_t  block_id,
		const uint32_t  param_blk_id,
		uint32_t       *chunk_data,
		const uint32_t  chunk_actual_size_in_words)
{
	int ret;
	int max_status_retries = MAX_STATUS_RETRIES;
	uint32_t get_param_data[CHUNK_RESPONSE_SIZE_IN_WORDS];
	uint32_t error;
	uint8_t  error_instance;

	/* In the chunk sent to plugin, 3 extra words are added.
	 * First word contains chunk_length+3 and ID
	 * Second word contains chunk length
	 * After the chunk data, CRC word is added.
	 */
	uint32_t nLen = (chunk_actual_size_in_words +
			CHUNK_OVERHEAD_SIZE_IN_WORDS);

	chunk_data[0] = (nLen << 16) |
			(uint32_t)(PROXY_FUNCTION_ID_3RDPARTY_CHUNK);

	chunk_data[1] = chunk_actual_size_in_words;
	chunk_data[nLen-1] = calculate_crc(&chunk_data[0], nLen-1);

	/* use odsp public api to send the parameter block */
	ret = iaxxx_core_set_param_blk_fixed_slot(dev, inst_id,
			nLen * sizeof(uint32_t),
			&chunk_data[0], block_id, param_blk_id);
	if (ret) {
		dev_err(dev, "Error sending chunk! %s()\n",
			__func__);
		goto out;
	}

	while (max_status_retries--) {
		memset(get_param_data, 0, sizeof(get_param_data));
		ret = iaxxx_core_get_param_blk(dev, inst_id, block_id,
				param_blk_id, get_param_data,
				ARRAY_SIZE(get_param_data));
		if (ret) {
			ret = read_plugin_error(dev, block_id,
					&error, &error_instance);

			if (!ret && error == FW_ERROR_CODE_BUSY) {
				dev_err(dev,
				"Getparamblk busy..retry after delay\n");
				usleep_range(10000, 10005);
			} else {
				dev_err(dev, "Getparamblk error error=%x\n",
						error);
				goto out_err;
			}
		} else {
			if (get_param_data[CHUNK_RESPONSE_ERRORCODE_INDEX]) {
				dev_err(dev,
				"Chunk write returned errcode=%x %u\n",
				get_param_data[1], get_param_data[2]);
				goto out_err;
			} else {
				ret = 0;
				goto out;
			}
		}
	}

out_err:
	ret = -EINVAL;
out:
	return ret;
}


static int parse_config_filedata_send_as_chunks(
		struct device *dev,
		const uint32_t  inst_id,
		const uint32_t  block_id,
		const uint32_t  param_blk_id,
		const uint32_t *config_data,
		const uint32_t  config_data_size_in_words)
{
	int      ret = 0;
	uint32_t config_data_index = 0;
	uint32_t cmd_len = 0;
	uint32_t *chunk_data_buffer = NULL;
	uint32_t  chunk_data_index = 0;
	uint32_t  chunk_data_free = 0;
	uint32_t  chunk_data_used = 0;

	chunk_data_buffer = kcalloc(CHUNK_SIZE_IN_WORDS, sizeof(uint32_t),
				    GFP_KERNEL);

	if (!chunk_data_buffer)
		return -ENOMEM;

	while (config_data_index < config_data_size_in_words) {
		/* Get the next command's length */
		cmd_len = high_16_bits(config_data[config_data_index]);

		if (!cmd_len)
			break;

		dev_dbg(dev, "Command %u\n", cmd_len);

		/* Check if the Command size is less than minimum
		 * size (2 WORDS)
		 * Also Check if the Command size is more than the
		 * max size (199 WORDS)
		 */
		if ((cmd_len > CHUNK_MAX_COMMAND_SIZE_ALLOWED_IN_WORDS) ||
		    (cmd_len < CHUNK_MIN_COMMAND_SIZE_ALLOWED_IN_WORDS)) {
			dev_err(dev, "Invalid command-size (%u)%s()\n",
					cmd_len, __func__);
			ret = -EINVAL;
			goto parse_config_filedata_error;
		}

		/* only command-length-1 words including the first word
		 * is copied from the file.
		 */
		cmd_len--;

		/* Find how much words are free to write in chunk */
		chunk_data_free = CHUNK_MAX_COMMAND_SIZE_ALLOWED_IN_WORDS -
					chunk_data_used;

		/* Next command cannot fit in this chunk, send the
		 * chunk and empty it.
		 */
		if (chunk_data_free < cmd_len) {

			dev_dbg(dev, "Sending chunk size:%u\n",
				chunk_data_used);

			ret = send_chunk_to_plugin(dev, inst_id, block_id,
				param_blk_id, chunk_data_buffer,
				chunk_data_used + 1);
			if (ret) {
				dev_err(dev, "Error Sending chunk!\n");
				goto parse_config_filedata_error;
			}

			chunk_data_used = 0;
			/* Chunk data buffer is cleared to ensure
			 * words at the end of chunk are 0.
			 */
			memset(chunk_data_buffer, 0, sizeof(chunk_data_buffer));
		}

		/* Copy chunk data to the chunk buffer */
		chunk_data_index = chunk_data_used +
				CHUNK_CMD_START_INDEX;
		memcpy(&chunk_data_buffer[chunk_data_index],
			&config_data[config_data_index],
			cmd_len * sizeof(uint32_t));
		config_data_index += cmd_len;
		chunk_data_used   += cmd_len;
	}

	/* Write the remainder chunk if any */
	if (chunk_data_used) {
		dev_dbg(dev, "Sending remainder chunk size:%u\n",
				chunk_data_used+1);
		/* Send the chunk with 0-value word at the end */
		ret = send_chunk_to_plugin(dev, inst_id, block_id,
			param_blk_id, chunk_data_buffer, chunk_data_used+1);
		if (ret)
			dev_err(dev, "Error Sending chunk!\n");
	}

parse_config_filedata_error:
	kfree(chunk_data_buffer);
	return ret;
}

/*****************************************************************************
 * iaxxx_core_set_custom_cfg()
 * @brief Set custom configuration for plugins
 *
 * @inst_id	      Plugin Instance Id
 * @block_id	      Update block id
 * @param_blk_id      Parameter block id
 * @custom_config_id  Id for what type of custom configuration
 * @file              File with config data
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_core_set_custom_cfg(
		struct device *dev, uint32_t inst_id,
		uint32_t block_id, uint32_t  param_blk_id,
		uint32_t custom_config_id, char *file)
{
	int ret = -EINVAL;
	const struct firmware *fw = NULL;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	uint32_t *data = NULL;

	if (!priv)
		return ret;

	inst_id &= IAXXX_PLGIN_ID_MASK;

	/* protect this plugin operation */
	mutex_lock(&priv->plugin_lock);

	/* Check if plugin exists */
	if (!priv->iaxxx_state->plgin[inst_id].plugin_inst_state) {
		mutex_unlock(&priv->plugin_lock);
		dev_err(dev, "Plugin instance 0x%x does not exist! %s()\n",
				inst_id, __func__);
		ret = -EEXIST;
		goto set_custom_cfg_err;
	}
	mutex_unlock(&priv->plugin_lock);

	if (file[0] == IAXXX_INVALID_FILE) {
		dev_err(dev, "Invalid custom config file name %s()\n",
			__func__);
		goto set_custom_cfg_err;
	}

	ret = request_firmware(&fw, file, priv->dev);
	if (ret) {
		dev_err(dev, "Custom config file %s not found %d\n",
				file, ret);
		ret = -EINVAL;
		goto set_custom_cfg_err;
	}

	if (fw->size & 0x3) {
		dev_err(dev, "Custom config file %s not 4 bytes aligned\n",
				file);
		ret = -EINVAL;
		goto set_custom_cfg_err;
	}

	data = kmalloc(fw->size, GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto set_custom_cfg_err;
	}

	iaxxx_copy_le32_to_cpu(data, fw->data, fw->size);

	dev_dbg(dev, "Custom config file %s read. Size %lu words\n",
			file, fw->size/4);

	/* custom_config_id will be checked here and if more than
	 * one custom configuration is needed and respective
	 * function will be called.
	 */
	ret = parse_config_filedata_send_as_chunks(dev, inst_id, block_id,
		param_blk_id, data, fw->size/4);

	kfree(data);

set_custom_cfg_err:
	if (fw)
		release_firmware(fw);
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_set_custom_cfg);
