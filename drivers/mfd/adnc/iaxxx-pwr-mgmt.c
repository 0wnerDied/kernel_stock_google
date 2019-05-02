/*
 * iaxxx-sysfs.c -- IAxxx Sysfs attributes
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
#include <linux/pm_runtime.h>
#include <linux/mfd/core.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/mfd/adnc/iaxxx-register-defs-srb.h>
#include <linux/mfd/adnc/iaxxx-core.h>
#include <linux/mfd/adnc/iaxxx-register-defs-pwr-mgmt.h>
#include <linux/mfd/adnc/iaxxx-pwr-mgmt.h>
#include <linux/mfd/adnc/iaxxx-register-defs-ao.h>
#include <linux/mfd/adnc/iaxxx-stream-registers.h>
#include <linux/mfd/adnc/iaxxx-odsp.h>
#include <linux/mfd/adnc/iaxxx-module.h>
#include "iaxxx-btp.h"
#include "iaxxx.h"
#include "iaxxx-plugin-common.h"

#define IAXXX_PWR_DWN_VAL 0x01C00050
#define IAXXX_PWR_ON_VAL 0x845
#define IAXXX_PWR_STATE_RETRY	0x5

#define IAXXX_PM_AUTOSUSPEND_DELAY 3000

#define IAXXX_PWR_MAX_SPI_SPEED   25000000
#define IAXXX_STRM_STATUS_INIT	 0

#define IAXXX_PROC_STATUS_WAIT_MIN		(1000)
#define IAXXX_PROC_STATUS_WAIT_MAX		(1005)

#define IAXXX_PROC_POWER_UP_DOWN_MASK(proc_id) \
	(IAXXX_SRB_PROC_PWR_CTRL_PWR_ON_PROC_0_MASK << proc_id)

#define IAXXX_PROC_STALL_ENABLE_DISABLE_MASK(proc_id) \
	(IAXXX_SRB_PROC_PWR_CTRL_STALL_PROC_0_MASK << proc_id)

#define IAXXX_MEM_POWER_UP_DOWN_MASK(proc_id) \
	(IAXXX_SRB_DED_MEM_PWR_CTRL_MEM_PWR_ON_PROC_0_MASK << \
	proc_id)

#define IAXXX_MEM_RETN_ON_OFF_MASK(proc_id) \
	(IAXXX_SRB_DED_MEM_PWR_CTRL_MEM_RETN_PROC_0_MASK << \
	proc_id)

#define IAXXX_GET_PROC_PWR_STATUS_MASK(proc_id) \
	(IAXXX_SRB_PROC_ACTIVE_STATUS_PWR_STATUS_PROC_0_MASK << \
	proc_id)

#define IAXXX_GET_PROC_PWR_STALL_STATUS_MASK(proc_id) \
	(IAXXX_SRB_PROC_ACTIVE_STATUS_STALL_STATUS_PROC_0_MASK << \
	proc_id)

#define IAXXX_GET_PER_PROC_MEM_PWR_STATUS_MASK(proc_id) \
	(IAXXX_SRB_DED_MEM_PWR_STATUS_PROC_0_MEM_PWR_MASK << \
	proc_id)

#define IAXXX_GET_PER_PROC_MEM_RETN_STATUS_MASK(proc_id) \
	(IAXXX_SRB_DED_MEM_PWR_STATUS_PROC_0_MEM_RETN_MASK << \
	proc_id)

void iaxxx_pm_enable(struct iaxxx_priv *priv)
{
#ifndef CONFIG_MFD_IAXXX_DISABLE_RUNTIME_PM
	int ret = 0;

	priv->in_suspend = 0;
	priv->in_resume = 0;
	ret = pm_runtime_set_active(priv->dev);
	if (ret < 0)
		dev_err(priv->dev, "pm_runtime_set_active fail %d\n", ret);
	pm_runtime_enable(priv->dev);

	pm_runtime_set_autosuspend_delay(priv->dev, IAXXX_PM_AUTOSUSPEND_DELAY);
	pm_runtime_use_autosuspend(priv->dev);
	pm_runtime_mark_last_busy(priv->dev);
#endif
}

int iaxxx_pm_get_sync(struct device *dev)
{
	int ret = 0;

#ifndef CONFIG_MFD_IAXXX_DISABLE_RUNTIME_PM
	struct iaxxx_priv *priv = dev ? to_iaxxx_priv(dev) : NULL;

	if (priv == NULL) {
		dev_err(dev, "%s dev is NULL here\n", __func__);
		return -EINVAL;
	}

	if (!pm_runtime_enabled(dev)) {
		dev_dbg(dev, "%s Run time PM not enabled\n", __func__);
		return 0;
	}

	if (mutex_trylock(&priv->pm_mutex))
		ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		dev_dbg(dev, "%s() Fail. %d\n", __func__, ret);

	mutex_unlock(&priv->pm_mutex);
#endif
	return ret;
}

int iaxxx_pm_put_autosuspend(struct device *dev)
{
	int ret = 0;

#ifndef CONFIG_MFD_IAXXX_DISABLE_RUNTIME_PM
	struct iaxxx_priv *priv = dev ? to_iaxxx_priv(dev) : NULL;

	if (priv == NULL) {
		dev_err(dev, "%s dev is NULL here\n", __func__);
		return -EINVAL;
	}

	if (!pm_runtime_enabled(dev)) {
		dev_dbg(dev, "%s Run time PM not enabled\n", __func__);
		return 0;
	}

	if (mutex_trylock(&priv->pm_mutex)) {
		pm_runtime_mark_last_busy(dev);
		ret = pm_runtime_put_sync_autosuspend(dev);
		if (ret && ret != -EBUSY)
			dev_dbg(dev, "%s(): fail %d\n", __func__, ret);
		mutex_unlock(&priv->pm_mutex);
	}

	if (ret == -EBUSY)
		ret = 0;
#endif
	return ret;
}

int iaxxx_wakeup_chip(struct iaxxx_priv *priv)
{
	int rc, reg_val;
	unsigned long ts_now;
	unsigned int ts_diff;

	/* Enable external clock */
	if (priv->iaxxx_state->power_state == IAXXX_SLEEP_MODE) {
		if (priv->iaxxx_mclk_cb)
			priv->iaxxx_mclk_cb(priv, 1);
	}

	if (!test_bit(IAXXX_FLG_CHIP_WAKEUP_HOST0,
						&priv->flags)) {
		rc = regmap_read(priv->regmap_no_pm,
				IAXXX_SRB_SYS_STATUS_ADDR, &reg_val);

		rc = wait_event_timeout(priv->wakeup_wq,
				test_bit(IAXXX_FLG_CHIP_WAKEUP_HOST0,
						&priv->flags), HZ / 10);
		if (!test_bit(IAXXX_FLG_CHIP_WAKEUP_HOST0,
						&priv->flags) && rc == 0)
			dev_err(priv->dev,
			"Timeout for wakeup event rc :%d wake flag :%d\n",
				rc, test_bit(IAXXX_FLG_CHIP_WAKEUP_HOST0,
				&priv->flags));
		else
			goto chip_woken_up;

		rc = regmap_read(priv->regmap_no_pm,
				IAXXX_SRB_SYS_STATUS_ADDR, &reg_val);

		/* if read failed or SYS mode is not in APP mode,
		 * flag error
		 */
		reg_val &= IAXXX_SRB_SYS_STATUS_MODE_MASK;
		dev_dbg(priv->dev,
			"%s chip wake up reg_val%d\n", __func__, reg_val);
		if (rc || (reg_val != SYSTEM_STATUS_MODE_APPS)) {
			dev_err(priv->dev,
				"%s chip wake up failed %d rc %d\n",
				__func__, reg_val, rc);
			return -EIO;
		}
		test_and_set_bit(IAXXX_FLG_CHIP_WAKEUP_HOST0,
								&priv->flags);
	}
chip_woken_up:
	if (priv->iaxxx_state->power_state == IAXXX_SLEEP_MODE) {

		ts_now = jiffies;
		ts_diff = ts_now - priv->iaxxx_state->sleep_ts;
		ts_diff = jiffies_to_msecs(ts_diff);

		/* switch to internal oscillator if dt entry
		 * is selected for internal oscillator mode.
		 */
		if (priv->oscillator_mode) {
			/* Switch to internal oscillator */
			rc = iaxxx_set_mpll_source_no_pm(priv, IAXXX_INT_OSC);
			if (rc) {
				dev_err(priv->dev,
				"%s() Failed to set MPLL Clk Src to internal\n",
								__func__);
				return rc;
			}
		}

		/* program SRB */
		rc = regmap_write(priv->regmap_no_pm,
				IAXXX_SRB_SYSTEM_SLEEP_DURATION_ADDR, ts_diff);

		if (!rc)
			rc = iaxxx_send_update_block_fixed_wait_no_pm(priv->dev,
							IAXXX_HOST_0, 10);
		if (rc)
			dev_err(priv->dev, "%s failed to program sleep time\n",
				__func__);

		priv->iaxxx_state->power_state = IAXXX_NORMAL_MODE;
		dev_info(priv->dev, "%s set to normal power mode done\n",
			__func__);
	}

	dev_info(priv->dev, "%s() Success\n", __func__);
	return 0;
}

int iaxxx_suspend_chip(struct iaxxx_priv *priv)
{
	int rc;

	/* set up the SPI speed thats expected when the system is wake up
	 * Set the SPI Speed to maximum so system will be awake with max
	 * clock speed.
	 */
	rc = regmap_write(priv->regmap_no_pm,
			IAXXX_PWR_MGMT_MAX_SPI_SPEED_REQ_ADDR,
			IAXXX_PWR_MAX_SPI_SPEED);

	if (rc) {
		dev_err(priv->dev,
			"Failed to set Max SPI speed in %s\n", __func__);
		return rc;
	}

	/* Note: Control interface of second host should also be disabled
	 * to achieve optimal or sleep mode.
	 */
	/* SLEEP MODE: If plugin count is zero and route is inactive */
	if (iaxxx_core_plg_list_empty(priv) &&
			!iaxxx_core_get_route_status(priv)) {
		/* Enable external clock */
		if (priv->iaxxx_mclk_cb)
			priv->iaxxx_mclk_cb(priv, 1);

		/* switch to external oscillator if dt entry
		 * is selected for internal oscillator mode.
		 * so that on wakeup we switch back to internal
		 * oscillator mode.
		 */
		if (priv->oscillator_mode) {
			/* Switch to external oscillator */
			rc = iaxxx_set_mpll_source_no_pm(priv, IAXXX_EXT_OSC);
			if (rc) {
				dev_err(priv->dev,
				"%s() Failed to set MPLL Clk Src to external\n",
								__func__);
				return rc;
			}
		}

		/* Issue sleep power mode command */
		rc = regmap_update_bits(priv->regmap_no_pm,
				IAXXX_SRB_SYS_POWER_CTRL_ADDR,
				IAXXX_SRB_SYS_POWER_CTRL_SET_POWER_MODE_MASK,
				(IAXXX_SLEEP_MODE <<
				IAXXX_SRB_SYS_POWER_CTRL_SET_POWER_MODE_POS));

		if (rc) {
			dev_err(priv->dev, "%s() Fail\n", __func__);
			return rc;
		}
		iaxxx_send_update_block_fixed_wait_no_pm(priv->dev,
							IAXXX_HOST_0, 20);
		priv->iaxxx_state->sleep_ts = jiffies;

		/* Disable external clock */
		if (priv->iaxxx_mclk_cb)
			priv->iaxxx_mclk_cb(priv, 0);

		priv->iaxxx_state->power_state = IAXXX_SLEEP_MODE;
		dev_info(priv->dev, "%s() chip put into sleep power mode\n",
								__func__);
	} else {
	/* OPTIMAL MODE: If plugin count is not zero or route is still active */

		/* Issue optimal power mode command */
		rc = regmap_update_bits(priv->regmap_no_pm,
			IAXXX_SRB_SYS_POWER_CTRL_ADDR,
			IAXXX_SRB_SYS_POWER_CTRL_DISABLE_CTRL_INTERFACE_MASK,
			(IAXXX_OPTIMAL_MODE <<
			IAXXX_SRB_SYS_POWER_CTRL_DISABLE_CTRL_INTERFACE_POS));

		if (rc) {
			dev_err(priv->dev, "%s() Fail\n", __func__);
			return rc;
		}

		iaxxx_send_update_block_fixed_wait_no_pm(priv->dev,
							IAXXX_HOST_0, 20);

		priv->iaxxx_state->power_state = IAXXX_OPTIMAL_MODE;
		dev_info(priv->dev, "%s() chip put into optimal power mode\n",
								__func__);
	}
	test_and_clear_bit(IAXXX_FLG_CHIP_WAKEUP_HOST0,
						&priv->flags);
	dev_info(priv->dev, "%s() Success\n", __func__);

	return 0;
}

/*
 * iaxxx_pm_set_aclk, input value as follows.
 * 0 - 3.072 MHz
 * 1 - 6.144 MHz
 * 2 - 12.288 MHz
 * 3 - 24.576 MHz
 * 4 - 49.152 MHz
 * 5 - 98.304 MHz
 * 6 - 368.640 MHz
 * otherwise - Invalid
 */
int iaxxx_pm_set_aclk(struct device *dev, int clk_freq)
{
	struct iaxxx_priv *priv = dev ? to_iaxxx_priv(dev) : NULL;
	int rc, status;

	dev_dbg(dev, "%s() freq:%d\n", __func__, clk_freq);

	rc = regmap_update_bits(priv->regmap,
			IAXXX_PWR_MGMT_SYS_CLK_CTRL_ADDR,
			IAXXX_PWR_MGMT_SYS_CLK_CTRL_APLL_OUT_FREQ_MASK,
			clk_freq <<
			IAXXX_PWR_MGMT_SYS_CLK_CTRL_APLL_OUT_FREQ_POS);
	if (!rc)
		rc = regmap_update_bits(priv->regmap,
			IAXXX_SRB_SYS_POWER_CTRL_ADDR,
			IAXXX_SRB_SYS_POWER_CTRL_CONFIG_APLL_MASK,
			IAXXX_SRB_SYS_POWER_CTRL_CONFIG_APLL_MASK);

	if (!rc)
		rc = iaxxx_send_update_block_request(dev, &status,
				IAXXX_BLOCK_0);

	if (rc)
		dev_info(dev, "%s() Fail\n", __func__);
	else
		dev_info(dev, "%s() Success\n", __func__);
	return rc;
}

/*
 * iaxxx_pm_set_optimal_power_mode (host0)
 * Need to do wake up to come out
 */
int iaxxx_pm_set_optimal_power_mode_host0(struct device *dev)
{
	struct iaxxx_priv *priv = dev ? to_iaxxx_priv(dev) : NULL;
	int rc;

	dev_dbg(dev, "%s()\n", __func__);

	/* Disable both the control interfaces and the chip will go to
	 * optimal power mode
	 */
	rc = regmap_write(priv->regmap, IAXXX_PWR_MGMT_MAX_SPI_SPEED_REQ_ADDR,
			priv->spi_app_speed);

	rc = regmap_update_bits(priv->regmap, IAXXX_SRB_SYS_POWER_CTRL_ADDR,
			IAXXX_SRB_SYS_POWER_CTRL_DISABLE_CTRL_INTERFACE_MASK,
			0x1 <<
			IAXXX_SRB_SYS_POWER_CTRL_DISABLE_CTRL_INTERFACE_POS);
	if (rc) {
		dev_err(dev, "%s() Fail\n", __func__);
		return rc;
	}

	iaxxx_send_update_block_fixed_wait(dev, IAXXX_HOST_0, 20);
	priv->iaxxx_state->power_state = IAXXX_OPTIMAL_MODE;
	return rc;
}

/*
 * iaxxx_pm_set_optimal_power_mode (host1)
 * Need to do wake up to come out
 */
int iaxxx_pm_set_optimal_power_mode_host1(struct device *dev)
{
	struct iaxxx_priv *priv = dev ? to_iaxxx_priv(dev) : NULL;
	int rc;

	dev_dbg(dev, "%s()\n", __func__);

	/* Disable both the control interfaces and the chip will go to
	 * optimal power mode
	 */
	rc = regmap_write(priv->regmap, IAXXX_PWR_MGMT_MAX_SPI_SPEED_REQ_1_ADDR,
	    priv->spi_app_speed);
	if (!rc)
		rc = regmap_update_bits(priv->regmap,
			IAXXX_SRB_SYS_POWER_CTRL_1_ADDR,
			IAXXX_SRB_SYS_POWER_CTRL_1_DISABLE_CTRL_INTERFACE_MASK,
			0x1 <<
			IAXXX_SRB_SYS_POWER_CTRL_1_DISABLE_CTRL_INTERFACE_POS);
	else {
		dev_err(dev, "%s() Fail\n", __func__);
		return rc;
	}

	iaxxx_send_update_block_fixed_wait(dev, IAXXX_HOST_1, 20);
	return rc;
}

int iaxxx_set_mpll_source(struct iaxxx_priv *priv, int source)
{
	int rc;
	uint32_t status;

	dev_dbg(priv->dev, "%s() source:%d\n", __func__, source);

	rc = regmap_update_bits(priv->regmap, IAXXX_PWR_MGMT_SYS_CLK_CTRL_ADDR,
			IAXXX_PWR_MGMT_SYS_CLK_CTRL_MPLL_SRC_MASK,
			source << IAXXX_PWR_MGMT_SYS_CLK_CTRL_MPLL_SRC_POS);
	if (!rc)
		rc = regmap_update_bits(priv->regmap,
			IAXXX_SRB_SYS_POWER_CTRL_ADDR,
			IAXXX_SRB_SYS_POWER_CTRL_CONFIG_MPLL_MASK,
			IAXXX_SRB_SYS_POWER_CTRL_CONFIG_MPLL_MASK);

	if (!rc)
		rc = iaxxx_send_update_block_fixed_wait_no_pm(priv->dev,
							IAXXX_HOST_0, 20);

	if (rc) {
		dev_err(priv->dev, "%s failed error code = %d\n", __func__, rc);
		return rc;
	}

	rc = regmap_read(priv->regmap,
				IAXXX_SRB_SYS_BLK_UPDATE_ADDR, &status);
	if (rc) {
		dev_err(priv->dev, "%s failed update block status read = %d\n",
				__func__, rc);
		return rc;
	}
	dev_err(priv->dev, "%s update block status = 0x%x\n",
			__func__, status);
	WARN_ON(status & IAXXX_SRB_SYS_BLK_UPDATE_REQ_MASK);
	if (((status & IAXXX_SRB_SYS_BLK_UPDATE_RES_MASK) != 0xFF) &&
		((status & IAXXX_SRB_SYS_BLK_UPDATE_ERR_CODE_MASK) != 0x00)) {
		dev_err(priv->dev,
		"%s update block for MPLL switch not successful = 0x%x\n",
				__func__, status);
		/* clear stale errors */
		regmap_write(priv->regmap, IAXXX_SRB_SYS_BLK_UPDATE_ADDR, 0);
	}
	return rc;
}

int iaxxx_set_mpll_source_no_pm(struct iaxxx_priv *priv, int source)
{
	int rc;
	uint32_t status;

	dev_dbg(priv->dev, "%s() source:%d\n", __func__, source);

	if (source == IAXXX_EXT_OSC) {
		/* Disable control interface 1 */
		rc = iaxxx_pm_set_optimal_power_mode_host1(priv->dev);
		if (rc) {
			dev_err(priv->dev,
				"%s() disabling controle interface 1 Fail\n",
				__func__);
			return rc;
		}
	}

	rc = regmap_update_bits(priv->regmap_no_pm,
		IAXXX_PWR_MGMT_SYS_CLK_CTRL_ADDR,
		IAXXX_PWR_MGMT_SYS_CLK_CTRL_MPLL_SRC_MASK,
		(source << IAXXX_PWR_MGMT_SYS_CLK_CTRL_MPLL_SRC_POS));

	if (rc) {
		dev_err(priv->dev, "%s() Failed to set MPLL err = %d\n",
							__func__, rc);
		return rc;
	}

	rc = regmap_update_bits(priv->regmap_no_pm,
		IAXXX_SRB_SYS_POWER_CTRL_ADDR,
		IAXXX_SRB_SYS_POWER_CTRL_CONFIG_MPLL_MASK,
		(1 << IAXXX_SRB_SYS_POWER_CTRL_CONFIG_MPLL_POS));

	if (rc) {
		dev_err(priv->dev, "%s() Fail err = %d\n", __func__, rc);
		return rc;
	}

	rc = iaxxx_send_update_block_fixed_wait_no_pm(priv->dev,
							IAXXX_HOST_0, 20);
	if (rc) {
		dev_err(priv->dev, "%s() Fail err = %d\n", __func__, rc);
		return rc;
	}

	rc = regmap_read(priv->regmap_no_pm,
				IAXXX_SRB_SYS_BLK_UPDATE_ADDR, &status);
	if (rc) {
		dev_err(priv->dev, "%s failed update block status read = %d\n",
				__func__, rc);
		return rc;
	}
	dev_err(priv->dev, "%s update block status = 0x%x\n",
			__func__, status);
	WARN_ON(status & IAXXX_SRB_SYS_BLK_UPDATE_REQ_MASK);
	if (((status & IAXXX_SRB_SYS_BLK_UPDATE_RES_MASK) != 0xFF) &&
		((status & IAXXX_SRB_SYS_BLK_UPDATE_ERR_CODE_MASK) != 0x00)) {
		dev_err(priv->dev,
		"%s update block for MPLL switch failed = 0x%x\n",
				__func__, status);
		/* clear stale errors */
		regmap_write(priv->regmap, IAXXX_SRB_SYS_BLK_UPDATE_ADDR, 0);
	}
	return rc;
}

int iaxxx_set_apll_source(struct iaxxx_priv *priv, int source)
{
	int rc;
	u32 efuse_trim_value, status = 0;

	dev_dbg(priv->dev, "%s() source:%d\n", __func__, source);

	rc = regmap_update_bits(priv->regmap, IAXXX_PWR_MGMT_SYS_CLK_CTRL_ADDR,
		IAXXX_PWR_MGMT_SYS_CLK_CTRL_APLL_SRC_MASK,
		source << IAXXX_PWR_MGMT_SYS_CLK_CTRL_APLL_SRC_POS);

	if (!rc)
		rc = regmap_update_bits(priv->regmap,
				IAXXX_SRB_SYS_POWER_CTRL_ADDR,
				IAXXX_SRB_SYS_POWER_CTRL_CONFIG_APLL_MASK,
				IAXXX_SRB_SYS_POWER_CTRL_CONFIG_APLL_MASK);

	regmap_read(priv->regmap, IAXXX_AO_EFUSE_BOOT_ADDR, &efuse_trim_value);
	efuse_trim_value = (efuse_trim_value >> 25) & 0x7F;

	if (efuse_trim_value) {
		rc = regmap_update_bits(priv->regmap, IAXXX_AO_OSC_CTRL_ADDR,
			IAXXX_AO_OSC_CTRL_ADJ_MASK, efuse_trim_value);
	}

	if (!rc)
		rc = iaxxx_send_update_block_request(priv->dev,
				&status, IAXXX_BLOCK_0);
	if (rc)
		dev_err(priv->dev, "%s failed to set up apll source err = %0x\n",
					__func__, rc);
	return rc;

}

static int check_stream_active_status(struct iaxxx_priv *priv)
{
	uint32_t strm_cnt = 0;
	uint32_t strm_status = 0;
	uint32_t strm_id = 0;
	int rc;

	/* Read the no of streams count */
	rc = regmap_read(priv->regmap,
			IAXXX_STR_HDR_STR_CNT_ADDR, &strm_cnt);
	if (rc) {
		dev_err(priv->dev, "%s IAXXX_STR_HDR_STR_CNT read err = %0x\n",
			__func__, rc);
		return rc;
	}

	/* Read the stream status for each stream group
	 * to confirm stream is not in use before disabling
	 * the SSP core.
	 */
	while (strm_id < strm_cnt) {
		rc = regmap_read(priv->regmap,
				IAXXX_STR_GRP_STR_STATUS_REG(strm_id),
				&strm_status);
		if (rc) {
			dev_err(priv->dev, "%s STR_GRP_STR_STATUS(%d) read err = %0x\n",
				__func__, strm_id, rc);
			return rc;
		} else if (strm_status != IAXXX_STRM_STATUS_INIT) {
			dev_err(priv->dev, "%s strm_id(%d) stream is still active\n",
				__func__, strm_id);
			return -EBUSY;
		}
		strm_id++;
	}
	return 0;
}

int iaxxx_set_proc_pwr_ctrl(struct iaxxx_priv *priv,
			uint32_t proc_id, uint32_t proc_state)
{
	uint32_t proc_pwr_ctrl_val = 0;
	uint32_t proc_pwr_ctrl_mask = 0;
	int rc;

	/* Make sure update block bit is in cleared state */
	rc = iaxxx_poll_update_block_req_bit_clr(priv);
	if (rc) {
		dev_err(priv->dev,
			"%s() Don't do Update Block in progress, rc = %d\n",
			__func__, rc);
		goto exit;
	}

	switch (proc_state) {
	case PROC_PWR_DOWN:
		if (proc_id == IAXXX_SSP_ID) {
			/* Check if any active streams are present */
			rc = check_stream_active_status(priv);
			if (rc) {
				dev_err(priv->dev,
					"%s streams still active rc = %0x\n",
					__func__, rc);
				goto exit;
			}
		}
		proc_pwr_ctrl_mask = IAXXX_PROC_POWER_UP_DOWN_MASK(proc_id);
		proc_pwr_ctrl_val = 0;
	break;
	case PROC_PWR_UP:
		proc_pwr_ctrl_mask = IAXXX_PROC_POWER_UP_DOWN_MASK(proc_id);
		proc_pwr_ctrl_val = IAXXX_PROC_POWER_UP_DOWN_MASK(proc_id);
	break;
	case PROC_STALL_ENABLE:
		if (proc_id == IAXXX_SSP_ID) {
			/* Check if any active streams are present */
			rc = check_stream_active_status(priv);
			if (rc) {
				dev_err(priv->dev,
					"%s streams still active rc = %0x\n",
					__func__, rc);
				goto exit;
			}
		}
		proc_pwr_ctrl_mask =
				IAXXX_PROC_STALL_ENABLE_DISABLE_MASK(proc_id);
		proc_pwr_ctrl_val =
				IAXXX_PROC_STALL_ENABLE_DISABLE_MASK(proc_id);
	break;
	case PROC_STALL_DISABLE:
		proc_pwr_ctrl_mask =
				IAXXX_PROC_STALL_ENABLE_DISABLE_MASK(proc_id);
		proc_pwr_ctrl_val = 0;
	break;
	default:
		dev_err(priv->dev, "%s wrong proc state requested (%d)\n",
			__func__, proc_state);
		goto exit;
	}

	/* set the processor bits */
	rc = regmap_update_bits(priv->regmap,
				IAXXX_SRB_PROC_PWR_CTRL_ADDR,
				proc_pwr_ctrl_mask, proc_pwr_ctrl_val);
	if (rc) {
		dev_err(priv->dev,
			"%s SRB_PROC_PWR_CTRL_ADDR write err = %0x\n",
			__func__, rc);
		goto exit;
	}

	rc = regmap_update_bits(priv->regmap,
			IAXXX_SRB_SYS_POWER_CTRL_ADDR,
			IAXXX_SRB_SYS_POWER_CTRL_SET_PROC_PWR_REQ_MASK,
			IAXXX_SRB_SYS_POWER_CTRL_SET_PROC_PWR_REQ_MASK);
	if (rc) {
		dev_err(priv->dev,
			"%s SRB_SYS_POWER_CTRL_ADDR write err = %0x\n",
			__func__, rc);
		goto exit;
	}

exit:
	return rc;
}

int iaxxx_set_mem_pwr_ctrl(struct iaxxx_priv *priv,
			uint32_t proc_id, uint32_t mem_state)
{
	uint32_t mem_pwr_ctrl_val = 0;
	uint32_t mem_pwr_ctrl_mask = 0;
	int rc;

	dev_dbg(priv->dev, "%s() proc_id:%u mem_state:%u\n", __func__,
		proc_id, mem_state);

	/* Make sure update block bit is in cleared state */
	rc = iaxxx_poll_update_block_req_bit_clr(priv);
	if (rc) {
		dev_err(priv->dev,
			"%s() Don't do Update Block in progress, rc = %d\n",
			__func__, rc);
		goto exit;
	}

	if (mem_state == MEM_PWR_DOWN || mem_state == MEM_RETN_ON) {
		if (proc_id == IAXXX_SSP_ID) {
			/* Check if any active streams are present */
			rc = check_stream_active_status(priv);
			if (rc) {
				dev_err(priv->dev,
					"%s stream still active rc = %0x\n",
					__func__, rc);
				goto exit;
			}
		}
	}

	switch (mem_state) {
	case MEM_PWR_DOWN:
		mem_pwr_ctrl_mask = IAXXX_MEM_POWER_UP_DOWN_MASK(proc_id);
		mem_pwr_ctrl_val = 0;
	break;
	case MEM_PWR_UP:
		mem_pwr_ctrl_mask = IAXXX_MEM_POWER_UP_DOWN_MASK(proc_id);
		mem_pwr_ctrl_val = IAXXX_MEM_POWER_UP_DOWN_MASK(proc_id);
	break;
	case MEM_RETN_ON:
		mem_pwr_ctrl_mask = IAXXX_MEM_RETN_ON_OFF_MASK(proc_id);
		mem_pwr_ctrl_val = IAXXX_MEM_RETN_ON_OFF_MASK(proc_id);
	break;
	case MEM_RETN_OFF:
		mem_pwr_ctrl_mask = IAXXX_MEM_RETN_ON_OFF_MASK(proc_id);
		mem_pwr_ctrl_val = 0;
	break;
	default:
		dev_err(priv->dev, "%s wrong mem_state state requested (%d)\n",
			__func__, mem_state);
		goto exit;
	}

	/* set the processor bits */
	rc = regmap_update_bits(priv->regmap,
				IAXXX_SRB_DED_MEM_PWR_CTRL_ADDR,
				mem_pwr_ctrl_mask, mem_pwr_ctrl_val);
	if (rc) {
		dev_err(priv->dev, "%s SRB_DED_MEM_PWR_CTRL write err = %0x\n",
			__func__, rc);
		goto exit;
	}

	rc = regmap_update_bits(priv->regmap,
				IAXXX_SRB_SYS_POWER_CTRL_ADDR,
				IAXXX_SRB_SYS_POWER_CTRL_SET_PROC_PWR_REQ_MASK,
				IAXXX_SRB_SYS_POWER_CTRL_SET_PROC_PWR_REQ_MASK);
	if (rc)
		dev_err(priv->dev,
			"%s SRB_SYS_POWER_CTRL_ADDR write err = %0x\n",
			__func__, rc);

exit:
	return rc;

}

int iaxxx_set_proc_hw_sleep_ctrl(struct iaxxx_priv *priv)
{
	int rc;
	uint32_t status;

	dev_dbg(priv->dev, "%s()\n", __func__);

	/* Make sure update block bit is in cleared state */
	rc = iaxxx_poll_update_block_req_bit_clr(priv);
	if (rc) {
		dev_err(priv->dev,
			"%s() Don't do Update Block in progress, rc = %d\n",
			__func__, rc);
		goto exit;
	}

	/* set the processor bits */
	rc = regmap_update_bits(priv->regmap,
			IAXXX_SRB_PROC_HWSLEEP_CTRL_ADDR,
			IAXXX_SRB_PROC_HWSLEEP_CTRL_WMASK_VAL,
			IAXXX_SRB_PROC_HWSLEEP_CTRL_HWSLEEP_PROC_3_MASK);
	if (rc) {
		dev_err(priv->dev, "%s SRB_PROC_HWSLEEP_CTRL write err = %0x\n",
			__func__, rc);
		goto exit;
	}

	rc = regmap_update_bits(priv->regmap,
			IAXXX_SRB_SYS_POWER_CTRL_ADDR,
			IAXXX_SRB_SYS_POWER_CTRL_SET_PROC_HWSLEEP_REQ_MASK,
			IAXXX_SRB_SYS_POWER_CTRL_SET_PROC_HWSLEEP_REQ_MASK);
	if (rc) {
		dev_err(priv->dev, "%s SRB_SYS_POWER_CTRL_ADDR write err = %0x\n",
					__func__, rc);
		goto exit;
	}

	rc = iaxxx_send_update_block_request(priv->dev, &status,
				IAXXX_BLOCK_0);
	if (rc)
		dev_err(priv->dev, "Update blk failed %s()\n", __func__);

exit:
	return rc;

}

static int iaxxx_get_proc_pwr_status(struct iaxxx_priv *priv,
	uint32_t proc_id, uint8_t *pwr_status, uint8_t *mem_status)
{
	uint32_t proc_pwr_status;
	uint32_t mem_pwr_status;
	int rc;

	rc = regmap_read(priv->regmap, IAXXX_SRB_PROC_ACTIVE_STATUS_ADDR,
		&proc_pwr_status);
	if (rc) {
		dev_err(priv->dev,
			"%s SRB_PROC_ACTIVE_STATUS_ADDR read err = %0x\n",
			__func__, rc);
		goto exit;
	}
	*pwr_status = proc_pwr_status &
			IAXXX_GET_PROC_PWR_STATUS_MASK(proc_id);
	dev_info(priv->dev,
		"%s() SRB_PROC_ACTIVE_STATUS_ADDR: %u pwr_status: 0x%08x\n",
		__func__, proc_pwr_status, *pwr_status);

	rc = regmap_read(priv->regmap, IAXXX_SRB_DED_MEM_PWR_STATUS_ADDR,
		&mem_pwr_status);
	if (rc) {
		dev_err(priv->dev,
			"%s SRB_DED_MEM_PWR_STATUS_ADDR read err = %0x\n",
			__func__, rc);
		goto exit;
	}
	*mem_status = mem_pwr_status &
			IAXXX_GET_PER_PROC_MEM_PWR_STATUS_MASK(proc_id);
	dev_info(priv->dev,
		"%s() SRB_DED_MEM_PWR_STATUS_ADDR: %u mem_status: 0x%08x\n",
		__func__, mem_pwr_status, *mem_status);

exit:
	return rc;
}

static int check_proc_power_status(struct iaxxx_priv *priv,
		uint32_t proc_id)
{
	uint8_t pwr_status = 0;
	uint8_t mem_status = 0;
	int retry_count = 5;
	int rc;

	do {
		rc = iaxxx_get_proc_pwr_status(priv, proc_id,
				&pwr_status, &mem_status);
		if (!rc && pwr_status == 0)
			break;
		if (rc || retry_count <= 0) {
			dev_err(priv->dev, "%s timedout in processor power down\n",
				__func__);
			return -ETIMEDOUT;
		}
		usleep_range(IAXXX_PROC_STATUS_WAIT_MIN,
				IAXXX_PROC_STATUS_WAIT_MAX);
	} while (retry_count--);

	return 0;
}

int iaxxx_power_up_core_mem(
	struct iaxxx_priv *priv, uint32_t proc_id)
{
	uint32_t status;
	int rc;

	dev_dbg(priv->dev, "%s() proc_id:%u\n", __func__, proc_id);

	rc = iaxxx_set_proc_pwr_ctrl(priv, proc_id, PROC_STALL_ENABLE);
	if (rc) {
		dev_err(priv->dev, "%s proc power ctrl PROC_STALL_ENABLE failed = %0x\n",
					__func__, rc);
		goto exit;
	}

	rc = iaxxx_set_proc_pwr_ctrl(priv, proc_id, PROC_PWR_UP);
	if (rc) {
		dev_err(priv->dev, "%s proc power ctrl PROC_PWR_UP failed = %0x\n",
					__func__, rc);
		goto exit;
	}

	rc = iaxxx_set_mem_pwr_ctrl(priv, proc_id, MEM_PWR_UP);
	if (rc) {
		dev_err(priv->dev, "%s mem power ctrl MEM_PWR_UP failed = %0x\n",
					__func__, rc);
		goto exit;
	}

	rc = iaxxx_send_update_block_request(priv->dev,
				&status, IAXXX_BLOCK_0);
	if (rc) {
		dev_err(priv->dev, "Update blk failed before download %s(): %d\n",
				__func__, status);
		goto exit;
	}

	rc = iaxxx_boot_core(priv, proc_id);
	if (rc) {
		dev_err(priv->dev, "boot_core (%d) fail :%d\n", proc_id, rc);
		goto exit;
	}

	rc = iaxxx_set_proc_pwr_ctrl(priv, proc_id,
				PROC_STALL_DISABLE);
	if (rc) {
		dev_err(priv->dev, "%s proc power ctrl PROC_STALL_DIS failed = %0x\n",
					__func__, rc);
		goto exit;
	}

	rc = iaxxx_send_update_block_request(priv->dev, &status,
				IAXXX_BLOCK_0);
	if (rc)
		dev_err(priv->dev, "Update blk failed after download %s(): %d\n",
				__func__, status);
exit:
	return rc;
}

int iaxxx_power_down_core_mem(
	struct iaxxx_priv *priv, uint32_t proc_id)
{
	uint32_t status;
	int rc = 0;

	dev_dbg(priv->dev, "%s() proc_id:%u\n", __func__, proc_id);

	rc = iaxxx_set_proc_pwr_ctrl(priv, proc_id, PROC_PWR_DOWN);
	if (rc) {
		dev_err(priv->dev, "%s proc power ctrl PROC_PWR_DOWN failed = %0x\n",
					__func__, rc);
		goto exit;
	}

	if (proc_id != IAXXX_SSP_ID) {
		rc = iaxxx_send_update_block_request(priv->dev,
					&status, IAXXX_BLOCK_0);
		if (rc) {
			dev_err(priv->dev,
			"Update blk failed after proc pwr down %s(): %d\n",
			__func__, status);
			goto exit;
		}

		rc = check_proc_power_status(priv, proc_id);
		if (rc) {
			dev_err(priv->dev, "%s check_proc_power_status failed = %0x\n",
						__func__, rc);
			goto exit;
		}
	}

	rc = iaxxx_set_mem_pwr_ctrl(priv, proc_id, MEM_PWR_DOWN);
	if (rc) {
		dev_err(priv->dev, "%s mem power ctrl MEM_PWR_DOWN failed = %0x\n",
					__func__, rc);
		goto exit;
	}

	rc = iaxxx_send_update_block_request(priv->dev,
				&status, IAXXX_BLOCK_0);
	if (rc)
		dev_err(priv->dev, "Update blk failed after mem pwr down%s(): %d\n",
				__func__, status);

exit:
	return rc;
}

int iaxxx_power_up_core_mem_on(
	struct iaxxx_priv *priv, uint32_t proc_id)
{
	uint32_t status;
	int rc = 0;

	dev_dbg(priv->dev, "%s() proc_id:%u\n", __func__, proc_id);
	rc = iaxxx_set_mem_pwr_ctrl(priv, proc_id, MEM_RETN_OFF);
	if (rc) {
		dev_err(priv->dev, "%s mem power ctrl MEM_RETN_OFF failed = %0x\n",
					__func__, rc);
		goto exit;
	}

	rc = iaxxx_set_proc_pwr_ctrl(priv, proc_id, PROC_PWR_UP);
	if (rc) {
		dev_err(priv->dev, "%s proc power ctrl PROC_PWR_UP failed = %0x\n",
					__func__, rc);
		goto exit;
	}

	rc = iaxxx_send_update_block_request(priv->dev,
				&status, IAXXX_BLOCK_0);
	if (rc)
		dev_err(priv->dev, "Update blk failed after proc pwr up %s(): %d\n",
				__func__, status);

exit:
	return rc;
}

int iaxxx_power_down_core_mem_in_retn(
	struct iaxxx_priv *priv, uint32_t proc_id)
{
	uint32_t status;
	int rc;

	dev_dbg(priv->dev, "%s() proc_id:%u\n", __func__, proc_id);

	rc = iaxxx_set_proc_pwr_ctrl(priv, proc_id, PROC_PWR_DOWN);
	if (rc) {
		dev_err(priv->dev, "%s proc power ctrl PROC_PWR_DOWN failed = %0x\n",
					__func__, rc);
		goto exit;
	}

	if (proc_id != IAXXX_SSP_ID) {
		rc = iaxxx_send_update_block_request(priv->dev,
					&status, IAXXX_BLOCK_0);
		if (rc) {
			dev_err(priv->dev,
			"Update blk failed after proc pwr down %s(): %d\n",
			__func__, status);
			goto exit;
		}

		rc = check_proc_power_status(priv, proc_id);
		if (rc) {
			dev_err(priv->dev, "%s check_proc_power_status failed = %0x\n",
						__func__, rc);
			goto exit;
		}
	}

	rc = iaxxx_set_mem_pwr_ctrl(priv, proc_id, MEM_RETN_ON);
	if (rc) {
		dev_err(priv->dev, "%s mem power ctrl MEM_RETN_ON failed = %0x\n",
					__func__, rc);
		goto exit;
	}

	rc = iaxxx_send_update_block_request(priv->dev,
				&status, IAXXX_BLOCK_0);
	if (rc)
		dev_err(priv->dev, "Update blk failed after mem retn ON %s(): %d\n",
				__func__, status);

exit:
	return rc;
}

/*
 * iaxxx_get_max_spi_speed
 * Gets the Max SPI Speed supported in the optimal power mode
 * max_spi_speed   Maximum SPI speed
 */
int iaxxx_get_max_spi_speed(struct device *dev, uint32_t *max_spi_speed)
{
	struct iaxxx_priv *priv = dev ? to_iaxxx_priv(dev) : NULL;
	int rc;

	rc = regmap_read(priv->regmap,
			IAXXX_PWR_MGMT_MAX_SPI_SPEED_ADDR,
			max_spi_speed);
	if (rc) {
		dev_err(dev,
		"%s() Fail to read PWR_MGMT_MAX_SPI_SPEED_ADDR rc = %d\n",
								__func__, rc);
	}

	dev_dbg(priv->dev, "%s() speed:%u\n", __func__, *max_spi_speed);

	return rc;
}

/*****************************************************************************
 * iaxxx_core_get_pwr_stats()
 * @brief get power transition count for each xclk and aclk values.
 *
 * @pwr_stats power statistics for mpll and apll values
 *
 * @ret n number of words read, ret in case of error
 ****************************************************************************/
int iaxxx_core_get_pwr_stats(struct device *dev,
			struct iaxxx_pwr_stats *pwr_stats)
{
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	uint32_t pwr_stats_addr = 0;
	uint32_t pwr_stats_size = 0;
	int ret = 0;

	ret = regmap_read(priv->regmap,
		IAXXX_PWR_MGMT_PWR_MGMT_STATS_PTR_ADDR, &pwr_stats_addr);
	if (ret) {
		dev_err(dev, "%s() failed %d\n", __func__, ret);
		goto exit;
	}

	ret = regmap_read(priv->regmap,
		IAXXX_PWR_MGMT_PWR_MGMT_STATS_SIZE_ADDR, &pwr_stats_size);
	if (ret) {
		dev_err(dev, "%s() failed %d\n", __func__, ret);
		goto exit;
	}

	if (pwr_stats_size != (sizeof(struct iaxxx_pwr_stats) >> 2)) {
		dev_err(dev,
			"%s() pwr_stats_size %d != struct size %lu\n",
			__func__, pwr_stats_size,
			sizeof(struct iaxxx_pwr_stats) >> 2);
		ret = -EINVAL;
		goto exit;
	}

	ret = iaxxx_btp_read(priv,
		pwr_stats_addr, pwr_stats,
		pwr_stats_size / sizeof(uint32_t), IAXXX_HOST_0);
	if (ret < 0) {
		dev_err(priv->dev, "Not able to read pwr stats %d\n",
				ret);
		goto exit;
	}

	dev_dbg(priv->dev, "read pwr stats successfully in words %u\n",
			pwr_stats_size);
exit:
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_get_pwr_stats);

/* iaxxx_set_osc_trim_period
 * Non zero values would enable trimming and set the period in seconds.
 * Zero would disable the feature. Ensure that 32.56 Khz ext clock is been
 * provided
 */
int iaxxx_set_osc_trim_period(struct iaxxx_priv *priv, int period)
{
	int rc = 0;

	if (period == priv->int_osc_trim_period)
		goto exit;
	dev_dbg(priv->dev, "%s() period:%d\n", __func__, period);

	rc = regmap_update_bits(priv->regmap,
		IAXXX_SRB_SYS_POWER_CTRL_ADDR,
		IAXXX_SRB_SYS_POWER_CTRL_TRIM_OSC_FREQ_MASK |
		IAXXX_SRB_SYS_POWER_CTRL_TRIM_OSC_PERIOD_MASK,
		IAXXX_SRB_SYS_POWER_CTRL_TRIM_OSC_FREQ_MASK |
		(period << IAXXX_SRB_SYS_POWER_CTRL_TRIM_OSC_PERIOD_POS));

	if (rc) {
		dev_err(priv->dev, "%s Failed to set Osc Trim period\n",
				__func__);
		goto exit;
	}

	rc = iaxxx_send_update_block_fixed_wait(priv->dev, IAXXX_HOST_0, 20);

	if (rc)
		dev_err(priv->dev, "Update block failed in %s\n", __func__);

	priv->int_osc_trim_period = period;
exit:
	return rc;
}
