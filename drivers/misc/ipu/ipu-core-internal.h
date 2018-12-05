/*
 * Internal data structures for IPU core
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

#ifndef __IPU_CORE_INTERNAL_H__
#define __IPU_CORE_INTERNAL_H__

#include <linux/atomic.h>
#include <linux/ipu-core.h>
#include <linux/iommu.h>
#include <linux/mutex.h>
#include <linux/pm_domain.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "ipu-regs.h"
#include "ipu-adapter.h"
#include "ipu-core-jqs-structs.h"

#define IPU_STATE_LINK_READY (1 << 0)
#define IPU_STATE_JQS_READY  (1 << 1)

/* Host-side only data associated with a jqs_circular_buffer */
struct host_jqs_cbuf {
	/* Shared data of the circular buffer data structure, struct jqs_cbuf */
	struct paintbox_shared_buffer *shared_buf_cbuf;
	uint32_t cbuf_offset;

	/* Shared data of data backing the circular buffer jqs_cbuf -> data */
	struct paintbox_shared_buffer *shared_buf_data;
	uint32_t data_offset;

	/* Value of buf->bytes_written at the time of the last data sync.
	 * See implementation of paintbox_jqs_circular_buffer_sync
	 * for details.
	 */
	uint32_t last_sync;
	/* to_device == true iff buf == sys_jqs_buffer */
	bool to_device;
};

/* host_jqs_queue_waiter is used to wait on incoming data for a jqs queue.
 *
 * Note: Only one waiter is allowed per queue.
 *
 * Note: The waiting behavior differs for an application queue vs. the kernel
 * queue. For the application queue, the void* is a user-space address, and any
 * incoming data on the queue will release the waiting thread.
 *
 * For the kernel queue, the waiter will be released only on the response to a
 * write_sync message. The void* is a kernel-space address.
 */
struct host_jqs_queue_waiter {
	struct completion completion;
	/* ret > 0 with number of bytes read, or < 0 on failure */
	int ret;

	void *buf;
	size_t size;
	bool enabled;
};

struct host_jqs_queue {
	/* shared_buf for the data that backs the queue */
	struct paintbox_shared_buffer shared_buf_data;

	struct host_jqs_cbuf host_jqs_sys_cbuf;
	struct host_jqs_cbuf host_sys_jqs_cbuf;

	/* The waiter, if any, on this queue */
	struct host_jqs_queue_waiter waiter;
};

struct paintbox_jqs_msg_transport {
	struct paintbox_shared_buffer shared_buf;

	struct host_jqs_queue queues[JQS_TRANSPORT_MAX_QUEUE];
	struct jqs_msg_transport_shared_state *jqs_shared_state;

	/* Bitmask of the available queue ids */
	uint32_t free_queue_ids;
};

#if IS_ENABLED(CONFIG_IPU_DEBUG)
struct ipu_bus_debug_register {
	struct paintbox_bus *bus;
	struct dentry *dentry;
	unsigned int offset;
};
#endif

struct paintbox_jqs {
	struct paintbox_shared_buffer fw_shared_buffer;
	struct mutex lock;
	const struct firmware *fw;
	enum paintbox_jqs_status status;
	enum jqs_log_level log_level;
	enum jqs_log_level log_trigger_level;
	uint32_t log_sink_mask;
	uint32_t uart_baud;
	uint64_t clock_rate_hz;
#if IS_ENABLED(CONFIG_IPU_DEBUG)
	struct dentry *debug_dir;
	struct dentry *fw_state_dentry;
	struct dentry *log_level_dentry;
	struct dentry *trigger_level_dentry;
	struct dentry *kernel_log_dentry;
	struct dentry *uart_log_dentry;
	struct dentry *uart_baud_dentry;
	struct dentry *reg_dump;
	struct ipu_bus_debug_register debug_registers[IO_JQS_NUM_REGS];
	uint32_t shadow_reg_jqs_sys_dbl;
#endif
};

struct paintbox_device;

struct paintbox_bus {
	struct paintbox_device *devices[PAINTBOX_DEVICE_TYPE_COUNT];
	struct generic_pm_domain gpd;
	struct paintbox_jqs jqs;
	struct paintbox_bus_ops *ops;
	struct paintbox_pdata *pdata;
	struct device *parent_dev;
	struct iommu_group *group;
#if IS_ENABLED(CONFIG_IPU_DEBUG)
	struct dentry *debug_root;
#endif
	struct paintbox_jqs_msg_transport *jqs_msg_transport;
	spinlock_t irq_lock;
	struct work_struct recovery_work;
	atomic_t state;

	/* Protects jqs msg transport structure. */
	struct mutex transport_lock;
};

struct paintbox_device {
	struct device dev;
	struct paintbox_bus *bus;
	enum paintbox_device_type type;
	const struct paintbox_device_ops *dev_ops;
#if IS_ENABLED(CONFIG_IPU_DEBUG)
	struct dentry *debug_root;
#endif
};

static inline struct paintbox_device *to_paintbox_device(struct device *dev)
{
	return container_of(dev, struct paintbox_device, dev);
}

static inline bool ipu_core_jqs_is_ready(struct paintbox_bus *bus)
{
	return !!(atomic_read(&bus->state) & IPU_STATE_JQS_READY);
}

static inline bool ipu_core_link_is_ready(struct paintbox_bus *bus)
{
	return !!(atomic_read(&bus->state) & IPU_STATE_LINK_READY);
}

static inline void ipu_core_writel(struct paintbox_bus *bus, uint32_t val,
		unsigned int offset)
{
	bus->ops->write32(bus->parent_dev, val, offset);
}

static inline void ipu_core_writeq(struct paintbox_bus *bus, uint64_t val,
		unsigned int offset)
{
	bus->ops->write64(bus->parent_dev, val, offset);
}

static inline uint32_t ipu_core_readl(struct paintbox_bus *bus,
		unsigned int offset)
{
	return bus->ops->read32(bus->parent_dev, offset);
}

static inline uint64_t ipu_core_readq(struct paintbox_bus *bus,
		unsigned int offset)
{
	return bus->ops->read64(bus->parent_dev, offset);
}

static inline int ipu_core_memory_alloc(struct paintbox_bus *bus, size_t size,
		struct paintbox_shared_buffer *shared_buffer)
{
	memset(shared_buffer, 0, sizeof(*shared_buffer));
	/*
	 * JQS caches data JQS_CACHE_LINE_SIZE bytes at a time, and writes all
	 * bytes back to memory if any byte in the line is modified.
	 *
	 * We need to round to the nearest cache line size to avoid a potential
	 * memory consistency problem where ipu_core_memory_alloc is called
	 * twice, memory is written from the AP on one of the allocations,
	 * memory is written from JQS on the other allocation, and the
	 * allocations share a cache line.
	 *
	 * Note: This logic should live in the Airbrush DRAM manager. But since
	 * that component doesn't exist yet, it'll live here for now.
	 */
	size = ALIGN(size, JQS_CACHE_LINE_SIZE);
	return bus->ops->alloc(bus->parent_dev, size, shared_buffer);
}

static inline void ipu_core_memory_free(struct paintbox_bus *bus,
		struct paintbox_shared_buffer *shared_buffer) {
	if (shared_buffer->host_vaddr) {
		bus->ops->free(bus->parent_dev, shared_buffer);
		memset(shared_buffer, 0, sizeof(*shared_buffer));
	}
}

static inline void ipu_core_sync(struct paintbox_bus *bus,
		struct paintbox_shared_buffer *alloc, uint32_t offset,
		size_t size, enum dma_data_direction direction) {
	bus->ops->sync(bus->parent_dev, alloc, offset, size, direction);
}

static inline void ipu_core_memory_map_to_bar(struct paintbox_bus *bus,
		struct paintbox_shared_buffer *alloc)
{
	/* map_to_bar is only supported on PCI based platforms */
	if (!bus->ops->map_to_bar)
		return;

	bus->ops->map_to_bar(bus->parent_dev, alloc);
}

static inline void ipu_core_memory_unmap_from_bar(struct paintbox_bus *bus,
		struct paintbox_shared_buffer *alloc)
{
	/* unmap_from_bar is only supported on PCI based platforms */
	if (!bus->ops->unmap_from_bar)
		return;

	bus->ops->unmap_from_bar(bus->parent_dev, alloc);
}

void ipu_core_notify_firmware_up(struct paintbox_bus *bus);
void ipu_core_notify_firmware_down(struct paintbox_bus *bus);

#endif /* __IPU_CORE_INTERNAL_H__ */
