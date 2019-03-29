/*
 * iaxxx-event.c -- IAxxx events
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

#define DEBUG
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/mfd/adnc/iaxxx-core.h>
#include <linux/mfd/adnc/iaxxx-plugin-registers.h>
#include <linux/mfd/adnc/iaxxx-register-defs-srb.h>
#include <linux/mfd/adnc/iaxxx-evnt-mgr.h>
#include <linux/mfd/adnc/iaxxx-system-identifiers.h>
#include <linux/mfd/adnc/iaxxx-register-defs-event-mgmt.h>
#include "iaxxx.h"


/*****************************************************************************
 * iaxxx_core_evt_is_valid_src_id()
 * @brief validate the plugin event scr id
 *
 * @id               event scr id
 * @ret true on success, false in case of error
 ****************************************************************************/
bool iaxxx_core_evt_is_valid_src_id(uint32_t src_id)
{
	bool ret = true;

	if (src_id > (IAXXX_EVT_MGMT_EVT_SUB_SRC_ID_MASK
					>> IAXXX_EVT_MGMT_EVT_SUB_SRC_ID_POS))
		ret = false;
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_evt_is_valid_src_id);

/*****************************************************************************
 * iaxxx_core_evt_is_valid_dst_id()
 * @brief validate the plugin event dest id
 *
 * @id              Plugin  event dst id
 * @ret true on success, false in case of error
 ****************************************************************************/
bool iaxxx_core_evt_is_valid_dst_id(uint32_t dst_id)
{
	bool ret = true;

	if (dst_id > (IAXXX_EVT_MGMT_EVT_SUB_DST_ID_MASK
					>> IAXXX_EVT_MGMT_EVT_SUB_DST_ID_POS))
		ret = false;
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_evt_is_valid_dst_id);

/*****************************************************************************
 * iaxxx_core_evt_is_valid_event_id()
 * @brief validate the plugin event id
 *
 * @id              Plugin  event event id
 * @ret true on success, false in case of error
 ****************************************************************************/
bool iaxxx_core_evt_is_valid_event_id(uint32_t event_id)
{
	bool ret = true;

	if (event_id > (IAXXX_EVT_MGMT_EVT_ID_REG_MASK
					>> IAXXX_EVT_MGMT_EVT_ID_REG_POS))
		ret = false;
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_evt_is_valid_event_id);

/*****************************************************************************
 * iaxxx_core_evt_is_valid_dst_opaque()
 * @brief validate the plugin dst opaque
 *
 * @id              Plugin  event dst opaque
 * @ret true on success, false in case of error
 ****************************************************************************/
bool iaxxx_core_evt_is_valid_dst_opaque(uint32_t dst_opaque)
{
	bool ret = true;

	if (dst_opaque > (IAXXX_EVT_MGMT_EVT_SUB_DST_OPAQUE_REG_MASK
				>> IAXXX_EVT_MGMT_EVT_SUB_DST_OPAQUE_REG_POS))
		ret = false;
	return ret;
}
EXPORT_SYMBOL(iaxxx_core_evt_is_valid_dst_opaque);

/*****************************************************************************
 * iaxxx_core_evt_subscribe()
 * @brief Subscribe to an event
 *
 * @src_id     -   System Id of event source
 * @event_id   -   Event Id
 * @dst_id     -   System Id of event destination
 * @ds_opaque  -   Information sought by destination task when even occurs.
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_core_evt_subscribe(struct device *dev, uint16_t src_id,
			uint16_t event_id, uint16_t dst_id, uint32_t dst_opaque)
{
	int ret = -EINVAL;
	int status;
	uint32_t sys_id;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);

	if (!priv)
		return ret;

	dev_dbg(dev,
		"%s() src_id : 0x%x dst_id: 0x%x\n", __func__, src_id, dst_id);

	if (src_id == IAXXX_SYSID_INVALID || dst_id == IAXXX_SYSID_INVALID) {
		dev_err(dev, "Invalid System Ids %s()\n", __func__);
		return ret;
	}

	/*
	 * Update all event subscription registers
	 * Event ID, IDS of source and destination, destination opaque
	 */
	ret = regmap_write(priv->regmap, IAXXX_EVT_MGMT_EVT_ID_ADDR, event_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		return ret;
	}
	sys_id = ((dst_id << 16) | src_id);
	ret = regmap_write(priv->regmap, IAXXX_EVT_MGMT_EVT_SUB_ADDR, sys_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		return ret;
	}
	ret = regmap_write(priv->regmap, IAXXX_EVT_MGMT_EVT_SUB_DST_OPAQUE_ADDR,
			dst_opaque);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		return ret;
	}
	ret = regmap_update_bits(priv->regmap, IAXXX_EVT_MGMT_EVT_ADDR,
		(1 << IAXXX_EVT_MGMT_EVT_SUB_REQ_POS),
		IAXXX_EVT_MGMT_EVT_SUB_REQ_MASK);
	if (ret) {
		dev_err(dev, "Update bit failed %s()\n", __func__);
		return ret;
	}
	ret = iaxxx_send_update_block_request(dev, &status, IAXXX_BLOCK_0);
	if (ret) {
		dev_err(dev, "Update blk failed %s()\n", __func__);
		return ret;
	}
	return 0;
}
EXPORT_SYMBOL(iaxxx_core_evt_subscribe);

/*****************************************************************************
 * iaxxx_core_evt_unsubscribe()
 * @brief UnSubscribe to an event
 *
 * @src_id     -   System Id of event source
 * @event_id   -   Event Id
 * @dst_id     -   System Id of event destination
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_core_evt_unsubscribe(struct device *dev, uint16_t src_id,
			uint16_t event_id, uint16_t dst_id)
{
	int ret = -EINVAL;
	uint32_t status;
	uint32_t sys_id;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);

	if (!priv)
		return ret;

	dev_dbg(dev, "%s()\n", __func__);

	if (src_id == IAXXX_SYSID_INVALID || dst_id == IAXXX_SYSID_INVALID) {
		dev_err(dev, "Invalid System Ids %s()\n", __func__);
		return ret;
	}
	/*
	 * Update all event subscription registers
	 * Event ID, Subsystem IDS of source and destination, destination
	 *  opaque
	 */
	ret = regmap_write(priv->regmap, IAXXX_EVT_MGMT_EVT_ID_ADDR, event_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		return ret;
	}
	sys_id = ((dst_id << 16) | src_id);
	ret = regmap_write(priv->regmap, IAXXX_EVT_MGMT_EVT_SUB_ADDR, sys_id);
	if (ret) {
		dev_err(dev, "write failed %s()\n", __func__);
		return ret;
	}
	ret = regmap_update_bits(priv->regmap, IAXXX_EVT_MGMT_EVT_ADDR,
		(1 << IAXXX_EVT_MGMT_EVT_UNSUB_REQ_POS),
		IAXXX_EVT_MGMT_EVT_UNSUB_REQ_MASK);
	if (ret) {
		dev_err(dev, "Update bit failed %s()\n", __func__);
		return ret;
	}
	ret = iaxxx_send_update_block_request(dev, &status, IAXXX_BLOCK_0);
	if (ret) {
		dev_err(dev, "Update blk failed %s()\n", __func__);
		return ret;
	}
	return 0;
}
EXPORT_SYMBOL(iaxxx_core_evt_unsubscribe);

/*****************************************************************************
 * @brief Fetches next event subscription entry from the last read position
 *
 * @param[out] src_id     -   System Id of event source
 * @param[out] evt_id     -   Event Id
 * @param[out] dst_id     -   System Id of event destination
 * @param[out] dst_opaque -   Destination opaque data
 *
 * @ret 0 on success, -EINVAL in case of error
 *****************************************************************************/
int iaxxx_core_evt_read_subscription(struct device *dev,
					uint16_t *src_id,
					uint16_t *evt_id,
					uint16_t *dst_id,
					uint32_t *dst_opaque)
{
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	int ret;
	uint32_t value = 0;
	uint32_t status;

	/* 1. Set the SUB_READ_REQ bit in EVT register to read subscription. */
	ret = regmap_update_bits(priv->regmap, IAXXX_EVT_MGMT_EVT_ADDR,
				IAXXX_EVT_MGMT_EVT_SUB_READ_REQ_MASK,
				(1 << IAXXX_EVT_MGMT_EVT_SUB_READ_REQ_POS));
	if (ret) {
		dev_err(dev,
	"Setting the SUB_RET_REQ bit in EVT register failed %s()\n", __func__);
		return ret;
	}
	/*
	 * 2. Set the REQ bit in the SYS_BLK_UPDATE register.
	 * 3. Wait for the REQ bit to clear. The device will also clear the
	 *    SUB_READ_REQ bit in the EVT register automatically.
	 * 4. Check the RES field in the SYS_BLK_UPDATE register to make sure
	 *    that the operation succeeded (content is 0x0).
	 */
	ret = iaxxx_send_update_block_request(dev, &status, IAXXX_BLOCK_0);
	if (ret) {
		dev_err(dev, "Update blk failed %s()\n", __func__);
		return ret;
	}

	/*
	 * 5. Read registers EVT_ID, EVT_SUB(source and destination Ids)
	 *    and EVT_DST_OPAQUE
	 */
	ret = regmap_read(priv->regmap,
				IAXXX_EVT_MGMT_EVT_DST_OPAQUE_ADDR, &value);
	if (ret) {
		dev_err(dev,
			"Failed to read IAXXX_EVT_MGMT_EVT_DST_OPAQUE_ADDR\n");
		return ret;
	}
	*dst_opaque = value;

	ret = regmap_read(priv->regmap,
				IAXXX_EVT_MGMT_EVT_ID_ADDR, &value);
	if (ret) {
		dev_err(dev, "Failed to read IAXXX_EVT_MGMT_EVT_ID_ADDR\n");
		return ret;
	}
	*evt_id = (uint16_t)value;

	ret = regmap_read(priv->regmap,
				IAXXX_EVT_MGMT_EVT_SUB_ADDR, &value);
	if (ret) {
		dev_err(dev, "Failed to read IAXXX_EVT_MGMT_EVT_SUB_ADDR\n");
		return ret;
	}
	*src_id = (uint16_t)((value & IAXXX_EVT_MGMT_EVT_SUB_SRC_ID_MASK)
					>> IAXXX_EVT_MGMT_EVT_SUB_SRC_ID_POS);
	*dst_id = (uint16_t)((value & IAXXX_EVT_MGMT_EVT_SUB_DST_ID_MASK)
					>> IAXXX_EVT_MGMT_EVT_SUB_DST_ID_POS);

	return ret;
}
EXPORT_SYMBOL(iaxxx_core_evt_read_subscription);

/*****************************************************************************
 * @brief Retrieve an event notification
 *
 *  @param[out] *src_id       pointer to uint16_t for reporting SystemId of
 *                            event source
 *  @param[out] *evt_dd       pointer to uint16_t for reporting Id of event
 *  @param[out] *src_opaque   pointer to the first parameter of event
 *  @param[out] *dst_opaque   pointer to the second parameter of event.
 *                            This will be destOpaque in case if event is
 *                            subscribed with valid destOpaque otherwise
 *                            it will be used as second parameter.
 *
 *  @return 0 if successful, error number in case of error
 *****************************************************************************/
int iaxxx_core_evt_retrieve_notification(struct device *dev,
						uint16_t *src_id,
						uint16_t *evt_id,
						uint32_t *src_opaque,
						uint32_t *dst_opaque)
{
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	int ret;
	uint32_t value = 0;
	uint32_t status;

	/*
	 * 1. Read the number of pending events N from the EVENT_COUNT register
	 *    Repeat the following steps N times(exit if N = 0)
	 */
	ret = regmap_read(priv->regmap, IAXXX_EVT_MGMT_EVT_COUNT_ADDR, &value);
	if (ret) {
		dev_err(dev, "Getting number of event notifications failed\n");
		return ret;
	}
	if (value == 0) {
		*src_id      = (uint16_t)IAXXX_SYSID_INVALID;
		*evt_id      = 0;
		*src_opaque  = 0;
		*dst_opaque  = 0;
		return 0;
	}

	/*
	 * 2. Set the NOT(notification) bit in the EVT_NEXT_REQ register.
	 */
	ret = regmap_write(priv->regmap,
				IAXXX_EVT_MGMT_EVT_NEXT_REQ_ADDR,
				IAXXX_EVT_MGMT_EVT_NEXT_REQ_NOT_MASK);
	if (ret) {
		dev_err(dev,
			"Writing request to retrieve notification failed\n");
		return ret;
	}

	/*
	 * 3. Set the REQ bit in the SYS_BLK_UPDATE register.
	 * 4. WARNING: The Host should not set the UPDATE_COMPLETE_ENABLE,
	 *    as that will result in a new Event being generated for the block
	 *    completion.
	 * 5. Wait for the REQ bit to clear. The device will also clear the
	 *    NOT(notification) bit in the EVT_NEXT_REQ register automatically.
	 * 6. Check the RES field in the SYS_BLK_UPDATE register to make sure
	 *    that the operation succeeded (content is 0x0).
	 */
	ret = iaxxx_send_update_block_request(dev, &status, IAXXX_BLOCK_0);
	if (ret) {
		dev_err(dev, "%s() Update blk failed\n", __func__);
		return ret;
	}

	/*
	 * 7. Read the EVENT_SRC_INFO, EVT_SRC_OPAQUE, and EVT_DST_OPAQUE
	 *    registers.
	 */
	ret = regmap_read(priv->regmap,
				IAXXX_EVT_MGMT_EVT_SRC_INFO_ADDR, &value);
	if (ret) {
		dev_err(dev, "Getting source information failed\n");
		return ret;
	}

	*src_id = (uint16_t)((value & IAXXX_EVT_MGMT_EVT_SRC_INFO_SYS_ID_MASK)
				>> IAXXX_EVT_MGMT_EVT_SRC_INFO_SYS_ID_POS);
	*evt_id = (uint16_t)((value & IAXXX_EVT_MGMT_EVT_SRC_INFO_EVT_ID_MASK)
				>> IAXXX_EVT_MGMT_EVT_SRC_INFO_EVT_ID_POS);

	ret = regmap_read(priv->regmap,
				IAXXX_EVT_MGMT_EVT_SRC_OPAQUE_ADDR, &value);
	if (ret) {
		dev_err(dev, "Getting source opaque failed\n");
		return ret;
	}
	*src_opaque = value;

	ret = regmap_read(priv->regmap,
				IAXXX_EVT_MGMT_EVT_DST_OPAQUE_ADDR, &value);
	if (ret) {
		dev_err(dev, "Getting destination opaque failed\n");
		return ret;
	}
	*dst_opaque = value;

	return ret;
}
EXPORT_SYMBOL(iaxxx_core_evt_retrieve_notification);

/*****************************************************************************
 *  @brief Reset index for retrieving subscription entries
 *
 *  @param   void
 *
 *  @return 0 if successful, error number in case of error
 *****************************************************************************/
int iaxxx_core_evt_reset_read_index(struct device *dev)
{
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	int ret;
	uint32_t status;

	/* Set the RESET_RD_IDX  bit to read subscription */
	ret = regmap_update_bits(priv->regmap, IAXXX_EVT_MGMT_EVT_ADDR,
				IAXXX_EVT_MGMT_EVT_RESET_RD_IDX_MASK,
				(1 << IAXXX_EVT_MGMT_EVT_RESET_RD_IDX_POS));
	if (ret) {
		dev_err(dev,
		"%s() Setting the RESET_RD_IDX bit in EVT register failed\n",
								__func__);
		return ret;
	}

	ret = iaxxx_send_update_block_request(dev, &status, IAXXX_BLOCK_0);
	if (ret) {
		dev_err(dev, "%s() Update blk failed\n", __func__);
		return ret;
	}

	return ret;
}
EXPORT_SYMBOL(iaxxx_core_evt_reset_read_index);

/*****************************************************************************
 * iaxxx_core_evt_trigger()
 *  @brief Trigger an event. This may be most useful when debugging the system,
 *        but can also be used to trigger simultaneous behavior in entities
 *        which have subscribed, or to simply provide notifications regarding
 *        host status:
 *
 *  @param[in] src_id        SystemId of event source
 *  @param[in] evt_id        Id of event
 *  @param[in] src_opaque    Source opaque to pass with event notification
 *
 *  @return 0 if successful, error number in case of error
 ****************************************************************************/
int iaxxx_core_evt_trigger(struct device *dev,
			uint16_t src_id, uint16_t evt_id, uint32_t src_opaque)
{
	int ret = -EINVAL;
	int status;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);

	if (!priv)
		return ret;

	dev_dbg(dev, "%s() src_id=%u, evt_id=%u, src_opaque=%u\n",
				__func__, src_id, evt_id, src_opaque);

	if (src_id == IAXXX_SYSID_INVALID) {
		dev_err(dev, "Invalid System Ids %s()\n", __func__);
		return ret;
	}

	/*
	 * 1. Set the System ID(src Id and evt Id) in the field of
	 *    EVT_SRC_INFO register.
	 */
	ret = regmap_write(priv->regmap, IAXXX_EVT_MGMT_EVT_SRC_INFO_ADDR,
			(src_id << IAXXX_EVT_MGMT_EVT_SRC_INFO_SYS_ID_POS)
			| (evt_id << IAXXX_EVT_MGMT_EVT_SRC_INFO_EVT_ID_POS));
	if (ret) {
		dev_err(dev, "Writing source information failed %s()\n",
								__func__);
		return ret;
	}

	/*
	 * 2. Set the source opaque data by writing to
	 *    the EVT_SRC_OPAQUE register.
	 */
	ret = regmap_write(priv->regmap,
			IAXXX_EVT_MGMT_EVT_SRC_OPAQUE_ADDR, src_opaque);
	if (ret) {
		dev_err(dev, "Writing source opaque failed %s()\n", __func__);
		return ret;
	}

	/* 3. Set the TRIG_REQ bit (and only it) in EVT register. */
	ret = regmap_update_bits(priv->regmap, IAXXX_EVT_MGMT_EVT_ADDR,
				IAXXX_EVT_MGMT_EVT_TRIG_REQ_MASK,
				(1 << IAXXX_EVT_MGMT_EVT_TRIG_REQ_POS));
	if (ret) {
		dev_err(dev,
		    "Setting the TRIG_REQ bit in EVT register failed %s()\n",
		    __func__);
		return ret;
	}

	/*
	 * 4. Set the REQ bit in the SYS_BLK_UPDATE register
	 *
	 * 5. Wait for the REQ bit to clear.
	 *    The device will also clear the EVENT_SUB_REQ bit automatically.
	 *
	 * 6. Check the RES field in the SYS_BLK_UPDATE register
	 *    to make sure that the operation succeeded (content is 0x0)
	 */

	ret = iaxxx_send_update_block_request(dev, &status, IAXXX_BLOCK_0);
	if (ret) {
		dev_err(dev, "Update blk failed %s()\n", __func__);
		return ret;
	}
	return 0;
}
EXPORT_SYMBOL(iaxxx_core_evt_trigger);

/*****************************************************************************
 * iaxxx_core_retrieve_event()
 * @brief Retrieve an event notification
 *
 * @event_id	-	Event Id
 * @data	-	Event data
 *
 * @ret 0 on success, -EINVAL in case of error
 ****************************************************************************/
int iaxxx_core_retrieve_event(struct device *dev, uint16_t *event_id,
		uint32_t *data)
{
	int ret = -EINVAL;
	struct iaxxx_priv *priv = to_iaxxx_priv(dev);
	int r_index = priv->event_queue->r_index;

	if (!priv)
		return ret;

	dev_dbg(dev, "%s()\n", __func__);

	mutex_lock(&priv->event_queue_lock);
	r_index++;
	/* Check if there are no events */
	if (r_index == (priv->event_queue->w_index + 1)) {
		dev_err(dev, "%s Buffer underflow\n", __func__);
		mutex_unlock(&priv->event_queue_lock);
		return ret;
	}
	if (r_index == IAXXX_MAX_EVENTS)
		r_index = 0;

	priv->event_queue->r_index = r_index;
	*event_id = priv->event_queue->event_info[r_index].event_id;
	*data = priv->event_queue->event_info[r_index].data;
	pr_debug("%s() event Id %d, data %d read index %d\n", __func__,
			*event_id, *data, r_index);
	mutex_unlock(&priv->event_queue_lock);
	return 0;
}
EXPORT_SYMBOL(iaxxx_core_retrieve_event);

/**
 * iaxxx_get_event_work - Work function to read events from the event queue
 *
 * @work : used to retrieve Transport Layer private structure
 *
 * This work function is scheduled by the ISR when any data is found in the
 * event queue.
 *
 * This function reads the available events from the queue and passes them
 * along to the event manager.
 */
static void iaxxx_get_event_work(struct work_struct *work)
{
	int rc;
	uint32_t count;
	struct iaxxx_event event;
	struct iaxxx_priv *priv = container_of(work, struct iaxxx_priv,
							event_work_struct);
	struct device *dev = priv->dev;

	mutex_lock(&priv->event_work_lock);

	if (priv->cm4_crashed) {
		dev_dbg(priv->dev, "CM4 crash event handler called:%d\n",
							priv->cm4_crashed);
		iaxxx_fw_crash(dev, IAXXX_FW_CRASH_EVENT);
		goto out;
	}

	/* Read the count of available events */
	rc = regmap_read(priv->regmap, IAXXX_EVT_MGMT_EVT_COUNT_ADDR,
			&count);
	if (rc) {
		dev_err(dev, "Failed to read EVENT_COUNT, rc = %d\n", rc);
		goto out;
	}

	while (count) {
		rc = iaxxx_next_event_request(priv, &event);
		if (rc) {
			dev_err(dev, "Failed to read event, rc = %d\n", rc);
			goto out;
		}
		rc = iaxxx_event_handler(priv, &event);
		if (rc) {
			dev_err(dev, "Event 0x%.04X:0x%.04X not delivered\n",
					event.event_src, event.event_id);
			goto out;
		}
		/* Read the count of available events */
		rc = regmap_read(priv->regmap,
				IAXXX_EVT_MGMT_EVT_COUNT_ADDR,
				&count);
		if (rc) {
			dev_err(dev, "Failed to read EVENT_COUNT, rc = %d\n",
					rc);
			goto out;
		}
	}

out:
	mutex_unlock(&priv->event_work_lock);
}

/**
 * iaxxx_event_init - Initialize Event Queue
 *
 * @priv : iaxxx private data
 */
int iaxxx_event_init(struct iaxxx_priv *priv)
{
	int rc;

	priv->event_queue = kmalloc(sizeof(struct iaxxx_evt_queue), GFP_KERNEL);
	if (!priv->event_queue)
		return -ENOMEM;
	priv->event_queue->r_index = -1;
	priv->event_queue->w_index = -1;
	priv->event_workq =
		alloc_workqueue("iaxxx-evnt-wq", WQ_MEM_RECLAIM, 0);
	if (!priv->event_workq) {
		pr_err("%s: failed to register event workq\n",
				__func__);
		rc = -ENOMEM;
		kfree(priv->event_queue);
		return rc;
	}
	/* Set the work queue function as iaxxx_get_event_work() */
	INIT_WORK(&priv->event_work_struct, iaxxx_get_event_work);
	return 0;
}

/**
 * iaxxx_event_exit - Free Event Queue
 *
 * @priv : iaxxx private data
 */
void iaxxx_event_exit(struct iaxxx_priv *priv)
{
	kfree(priv->event_queue);
	destroy_workqueue(priv->event_workq);
	priv->event_workq = NULL;
}
