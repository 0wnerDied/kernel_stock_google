/*
 * JQS management support for the Paintbox programmable IPU
 *
 * Copyright (C) 2018 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/ipu-core.h>
#include <linux/ipu-jqs-messages.h>
#include <linux/types.h>

#include "ipu-core-internal.h"
#include "ipu-core-jqs.h"
#include "ipu-core-jqs-msg-transport.h"
#include "ipu-core-jqs-preamble.h"
#include "ipu-regs.h"

#define JQS_FIRMWARE_NAME "paintbox-jqs.fw"

#define A0_IPU_DEFAULT_CLOCK_RATE 549000000 /* hz */

/* Delay for I/O block to wake up */
#define IO_POWER_RAMP_TIME 10 /* us */

/* Delay to prevent in-rush current */
#define CORE_POWER_RAMP_TIME 10 /* us */

/* Delay for rams to wake up */
#define RAM_POWER_RAIL_RAMP_TIME 1 /* us */

/* Delay for system to stabilize before sending real traffic */
#define CORE_SYSTEM_STABLIZE_TIME 100 /* us */

static int ipu_core_jqs_send_clock_rate(struct paintbox_bus *bus,
		uint32_t clock_rate_hz)
{
	struct jqs_message_clock_rate req;

	dev_dbg(bus->parent_dev, "%s: clock rate %u\n", __func__,
			clock_rate_hz);

	INIT_JQS_MSG(req, JQS_MESSAGE_TYPE_CLOCK_RATE);

	req.clock_rate = clock_rate_hz;

	return ipu_core_jqs_msg_transport_kernel_write(bus,
			(const struct jqs_message *)&req);
}

static int ipu_core_jqs_send_set_log_info(struct paintbox_bus *bus,
		enum jqs_log_level log_level,
		enum jqs_log_level interrupt_level,
		uint32_t log_sinks, uint32_t uart_baud_rate)
{
	struct jqs_message_set_log_info req;

	dev_dbg(bus->parent_dev,
			"%s: log sinks 0x%08x log level %u log int level %u uart baud_rate %u\n",
			__func__, log_sinks, log_level, interrupt_level,
			uart_baud_rate);

	INIT_JQS_MSG(req, JQS_MESSAGE_TYPE_SET_LOG_INFO);

	req.log_level = log_level;
	req.interrupt_level = interrupt_level;
	req.log_sinks = log_sinks;
	req.uart_baud_rate = uart_baud_rate;

	return ipu_core_jqs_msg_transport_kernel_write(bus,
			(const struct jqs_message *)&req);
}

int ipu_core_jqs_load_firmware(struct paintbox_bus *bus)
{
	int ret;

	dev_dbg(bus->parent_dev, "requesting firmware %s\n", JQS_FIRMWARE_NAME);

	ret = request_firmware(&bus->fw, JQS_FIRMWARE_NAME, bus->parent_dev);
	if (ret < 0) {
		dev_err(bus->parent_dev, "%s: unable to load %s, %d\n",
				__func__, JQS_FIRMWARE_NAME, ret);
		return ret;
	}

	bus->fw_status = JQS_FW_STATUS_REQUESTED;

	return 0;
}

void ipu_core_jqs_unload_firmware(struct paintbox_bus *bus)
{
	if (bus->fw_status != JQS_FW_STATUS_REQUESTED)
		return;

	dev_dbg(bus->parent_dev, "%s: unloading firmware\n", __func__);

	if (bus->fw) {
		release_firmware(bus->fw);
		bus->fw = NULL;
	}

	bus->fw_status = JQS_FW_STATUS_INIT;
}

int ipu_core_jqs_stage_firmware(struct paintbox_bus *bus)
{
	struct jqs_firmware_preamble preamble;
	size_t fw_binary_len_bytes;
	int ret;

	if (WARN_ON(!bus->fw))
		return -EINVAL;

	memcpy(&preamble, bus->fw->data, min(sizeof(preamble), bus->fw->size));

	if (preamble.magic != JQS_PREAMBLE_MAGIC_WORD) {
		dev_err(bus->parent_dev,
			"%s: invalid magic in JQS firmware preamble\n",
			__func__);
		return -EINVAL;
	}

	dev_dbg(bus->parent_dev,
			"%s: size %u fw_base_address 0x%08x FW and working set size %u prefill transport offset bytes %u\n",
			__func__, preamble.size, preamble.fw_base_address,
			preamble.fw_and_working_set_bytes,
			preamble.prefill_transport_offset_bytes);

	/* TODO(b/115524239):  It would be good to have some sort of bounds
	 * checking to make sure that the firmware could not allocate an
	 * unreasonable amount of memory for its working set.
	 *
	 * TODO(b/115522126):  The firmware is compiled for a specific address
	 * in AB DRAM.  This will necessitate having a carveout region in AB
	 * DRAM so we can guarantee the address.
	 */
	ret = bus->ops->alloc(bus->parent_dev,
			preamble.fw_and_working_set_bytes,
			&bus->fw_shared_buffer);
	if (ret < 0)
		return ret;

	fw_binary_len_bytes = bus->fw->size -
			sizeof(struct jqs_firmware_preamble);

	memcpy(bus->fw_shared_buffer.host_vaddr, bus->fw->data +
			sizeof(struct jqs_firmware_preamble),
			fw_binary_len_bytes);

	bus->ops->sync(bus->parent_dev, &bus->fw_shared_buffer, 0,
			fw_binary_len_bytes, DMA_TO_DEVICE);

	bus->fw_status = JQS_FW_STATUS_STAGED;

	return 0;
}

void ipu_core_jqs_unstage_firmware(struct paintbox_bus *bus)
{
	if (bus->fw_status != JQS_FW_STATUS_STAGED)
		return;

	dev_dbg(bus->parent_dev, "%s: unstaging firmware\n", __func__);

	ipu_core_memory_free(bus, &bus->fw_shared_buffer);
	bus->fw_status = JQS_FW_STATUS_REQUESTED;
}

static void ipu_core_jqs_power_enable(struct paintbox_bus *bus,
		dma_addr_t boot_ab_paddr, dma_addr_t smem_ab_paddr)
{
	/* The Airbrush IPU needs to be put in reset before turning on the
	 * I/O block.
	 */
	ipu_core_writel(bus, SOFT_RESET_IPU_MASK, IPU_CSR_AON_OFFSET +
			SOFT_RESET);

	ipu_core_writel(bus, JQS_CACHE_ENABLE_I_CACHE_MASK |
			JQS_CACHE_ENABLE_D_CACHE_MASK,
			IPU_CSR_AON_OFFSET + JQS_CACHE_ENABLE);

	ipu_core_writel(bus, (uint32_t)boot_ab_paddr, IPU_CSR_AON_OFFSET +
			JQS_BOOT_ADDR);

	/* Pre-power the I/O block and then enable power */
	ipu_core_writeq(bus, IO_POWER_ON_N_MAIN_MASK, IPU_CSR_AON_OFFSET +
			IO_POWER_ON_N);
	ipu_core_writeq(bus, 0, IPU_CSR_AON_OFFSET + IO_POWER_ON_N);

	udelay(IO_POWER_RAMP_TIME);

	/* We need to run the clock to the I/O block while it is being powered
	 * on briefly so that all the synchronizers clock through their data and
	 * all the Xs (or random values in the real HW) clear. Then we need to
	 * turn the clock back off so that we can meet timing on the RAM SD pin
	 * -- the setup & hold on the RAM's SD pin is significantly longer than
	 * 1 clock cycle.
	 */
	ipu_core_writel(bus, IPU_IO_SWITCHED_CLK_EN_VAL_MASK,
			IPU_CSR_AON_OFFSET + IPU_IO_SWITCHED_CLK_EN);
	ipu_core_writel(bus, 0, IPU_CSR_AON_OFFSET + IPU_IO_SWITCHED_CLK_EN);

	/* Power on RAMs for I/O block */
	ipu_core_writel(bus, 0, IPU_CSR_AON_OFFSET + IO_RAM_ON_N);
	udelay(RAM_POWER_RAIL_RAMP_TIME);

	/* Turn on clocks to I/O block */
	ipu_core_writel(bus, IPU_IO_SWITCHED_CLK_EN_VAL_MASK,
			IPU_CSR_AON_OFFSET + IPU_IO_SWITCHED_CLK_EN);

	/* Turn off isolation for I/O block */
	ipu_core_writel(bus, 0, IPU_CSR_AON_OFFSET + IO_ISO_ON);

	/* Take the IPU out of reset. */
	ipu_core_writel(bus, 0, IPU_CSR_AON_OFFSET + SOFT_RESET);

	ipu_core_writel(bus, (uint32_t)smem_ab_paddr, IPU_CSR_JQS_OFFSET +
			SYS_JQS_GPR_0);

	/* Enable the JQS */
	ipu_core_writel(bus, JQS_CONTROL_CORE_FETCH_EN_MASK,
			IPU_CSR_AON_OFFSET + JQS_CONTROL);
}

static int ipu_core_jqs_start_firmware(struct paintbox_bus *bus)
{
	int ret;

	dev_dbg(bus->parent_dev, "%s: enabling firmware\n", __func__);

	ret = ipu_core_jqs_msg_transport_init(bus);
	if (ret < 0)
		return ret;

	ipu_core_jqs_power_enable(bus, bus->fw_shared_buffer.jqs_paddr,
			bus->jqs_msg_transport->shared_buf.jqs_paddr);

	ret = ipu_core_jqs_send_clock_rate(bus, A0_IPU_DEFAULT_CLOCK_RATE);
	if (ret < 0)
		return ret;

	ret = ipu_core_jqs_send_set_log_info(bus, JQS_LOG_LEVEL_INFO,
			JQS_LOG_LEVEL_INFO, JQS_LOG_SINK_UART, 115200);
	if (ret < 0)
		return ret;

	bus->fw_status = JQS_FW_STATUS_RUNNING;

	/* Notify paintbox devices that the firmware is up */
	ipu_core_notify_firmware_up(bus);

	return 0;
}

static void ipu_core_jqs_start_rom_firmware(struct paintbox_bus *bus)
{
	dev_dbg(bus->parent_dev, "enabling ROM firmware\n");
	ipu_core_jqs_power_enable(bus, JQS_BOOT_ADDR_DEF, 0);
	bus->fw_status = JQS_FW_STATUS_RUNNING;

	/* Notify paintbox devices that the firmware is up */
	ipu_core_notify_firmware_up(bus);
}

int ipu_core_jqs_enable_firmware(struct paintbox_bus *bus)
{
	int ret;

	/* Firmware status will be set to INIT at boot or if the driver is
	 * unloaded and reloaded (likely due to a PCIe link change).
	 */
	if (bus->fw_status == JQS_FW_STATUS_INIT) {
		ret = ipu_core_jqs_load_firmware(bus);
		if (ret < 0)
			goto start_rom_firmware;
	}

	/* If the firmware is in the requested state then stage it to DRAM.
	 * Firmware status will return this state whenever Airbrush transitions
	 * to the OFF state.
	 */
	if (bus->fw_status == JQS_FW_STATUS_REQUESTED) {
		ret = ipu_core_jqs_stage_firmware(bus);
		if (ret < 0)
			goto unload_firmware;
	}

	/* If the firmware has been staged then enable the firmware.  Firmware
	 * status will return to this state for all suspend and sleep states
	 * with the exception of OFF.
	 */
	if (bus->fw_status == JQS_FW_STATUS_STAGED) {
		ret = ipu_core_jqs_start_firmware(bus);
		if (ret < 0)
			goto unstage_firmware;
	}

	return 0;

unstage_firmware:
	ipu_core_jqs_unstage_firmware(bus);
unload_firmware:
	ipu_core_jqs_unload_firmware(bus);
start_rom_firmware:
	ipu_core_jqs_start_rom_firmware(bus);
	return 0;
}

void ipu_core_jqs_disable_firmware(struct paintbox_bus *bus)
{
	if (bus->fw_status != JQS_FW_STATUS_RUNNING)
		return;

	dev_dbg(bus->parent_dev, "%s: disabling firmware\n", __func__);

	/* Notify paintbox devices that the firmware is down */
	ipu_core_notify_firmware_down(bus);

	ipu_core_jqs_msg_transport_shutdown(bus);

	ipu_core_writel(bus, 0, IPU_CSR_AON_OFFSET + JQS_CONTROL);

	/* Turn on isolation for I/O block */
	ipu_core_writel(bus, IO_ISO_ON_VAL_MASK, IPU_CSR_AON_OFFSET +
			IO_ISO_ON);

	/* Turn off clocks to I/O block */
	ipu_core_writel(bus, 0, IPU_CSR_AON_OFFSET + IPU_IO_SWITCHED_CLK_EN);

	/* Power off RAMs for I/O block */
	ipu_core_writel(bus, IO_RAM_ON_N_VAL_MASK, IPU_CSR_AON_OFFSET +
			IO_RAM_ON_N);

	/* Need to briefly turn on the clocks to the I/O block to propagate the
	 * RAM SD pin change into the RAM, then need to turn the clocks off
	 * again, since the I/O block is being turned off.
	 */
	ipu_core_writel(bus, IPU_IO_SWITCHED_CLK_EN_VAL_MASK,
			IPU_CSR_AON_OFFSET + IPU_IO_SWITCHED_CLK_EN);
	ipu_core_writel(bus, 0, IPU_CSR_AON_OFFSET +
			IPU_IO_SWITCHED_CLK_EN);

	/* Power off I/O block */
	ipu_core_writeq(bus, IO_POWER_ON_N_PRE_MASK |
			IO_POWER_ON_N_MAIN_MASK, IPU_CSR_AON_OFFSET +
			IO_POWER_ON_N);

	bus->fw_status = JQS_FW_STATUS_STAGED;
}

void ipu_core_jqs_release(struct paintbox_bus *bus)
{
	ipu_core_jqs_disable_firmware(bus);
	ipu_core_jqs_unstage_firmware(bus);
	ipu_core_jqs_unload_firmware(bus);
}

enum paintbox_jqs_status ipu_bus_get_fw_status(struct paintbox_bus *bus)
{
	return bus->fw_status;
}
