/*
 * Paintbox IPU Client Common Header
 *
 * Copyright (C) 2015 Google, Inc.
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

#ifndef __IPU_CLIENT_H__
#define __IPU_CLIENT_H__

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/iommu.h>
#include <linux/ipu-core.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>

#include <uapi/ipu.h>
#include <uapi/paintbox.h>

#define RESOURCE_NAME_LEN    16

/* This timeout is the minimum wait time for a MIPI stream cleanup to
 * complete.
 */
#define MIPI_CLEANUP_TIMEOUT_US 200

/* This timeout is the minimum wait time for a DMA stop operation to complete
 * before resetting the channel.
 */
#define DMA_STOP_TIMEOUT_US 500

/* TODO(b/115416247):  These defines need to be kept in sync with the JQS.
 * These values could be moved to a common header or there could be a
 * handshake between the driver and JQS firmware at runtime.
 */
#define JQS_SESSION_MEMORY_SIZE 16384
#define PAINTBOX_BUFFER_ID_MAX 1024

struct paintbox_data;

/* Data structure for all information related to a paintbox session.  A session
 * will be allocated on open() and deleted on release().
 */
struct paintbox_session {
	struct paintbox_data *dev;
	struct list_head dma_list;
	struct list_head stp_list;
	struct list_head lbp_list;
	struct list_head wait_list;
	struct list_head cmd_queue_list;

	/* bulk allocation fields */
	struct list_head alloc_wait_list_entry;
	bool waiting_alloc;
	struct completion bulk_alloc_completion;

	int session_id;
	int highest_core_id;

	uint64_t stp_id_mask;
	uint64_t lbp_id_mask;
	uint64_t dma_channel_id_mask;

	/* The fields below are protected by pb->lock*/
	struct idr buffer_idr;
	struct dma_buf *buffer_id_table;
};

struct paintbox_debug_reg_entry;
struct paintbox_debug;

typedef int (*register_dump_t)(struct paintbox_debug *debug, char *buf,
		size_t len);
typedef int (*stats_dump_t)(struct paintbox_debug *debug, char *buf,
		size_t len);

typedef void (*register_write_t)(struct paintbox_debug_reg_entry *reg_entry,
		uint64_t val);
typedef uint64_t (*register_read_t)(struct paintbox_debug_reg_entry *reg_entry);

struct paintbox_debug_reg_entry {
	struct paintbox_debug *debug;
	struct dentry *debug_dentry;
	unsigned int reg_offset;
	register_write_t write;
	register_read_t read;
};

struct paintbox_debug {
	struct paintbox_data *pb;
	struct dentry *debug_dir;

	/* Debug FS entry used for dumping all registers in a block (STP, LBP,
	 * etc.) including field details.
	 */
	struct dentry *reg_dump_dentry;

	/* Debug FS entry used for dumping statistics in a block. */
	struct dentry *stats_dump_dentry;

	/* Array of Debug FS entries sized to number of registers in the block.
	 * (STP, LBP, etc.).  Each entry is used for read and write access
	 * to an individual register in the block.
	 */
	struct paintbox_debug_reg_entry *reg_entries;
	size_t num_reg_entries;
	const char *name;
	int resource_id;
	register_dump_t register_dump;
	stats_dump_t stats_dump;
};

struct paintbox_power {
	unsigned int active_core_count;
	int bif_mmu_clock_idle_disable_ref_count;

	struct {
		uint32_t dma_chan_en;
	} regs;
};

/* Data structure for information specific to a DMA channel.
 * One entry will be allocated for each channel on a DMA controller.
 *
 * Note that the channel id is stored with the paintbox_dma as a convenience
 * to avoid having to recover the channel id from the pb->dmas array when a
 * function only has the paintbox_dma object.
 */
struct paintbox_dma_channel {
	/* Session list entry, A DMA channel may be allocated to a session or
	 * released using the PB_ALLOCATE_DMA_CHANNEL and PB_RELEASE_DMA_CHANNEL
	 * ioctls.
	 */
	struct list_head session_entry;
#if IS_ENABLED(CONFIG_IPU_DEBUG)
	struct paintbox_debug debug;
#endif
	struct paintbox_session *session;
	unsigned int channel_id;
};

struct paintbox_dma {
#if IS_ENABLED(CONFIG_IPU_DEBUG)
	struct paintbox_debug debug;
#endif
	struct paintbox_dma_channel *channels;
	unsigned int num_channels;
	uint64_t available_channel_mask;
	struct dentry *debug_enable_dentry;
	bool debug_enabled;
};

/* Data structure for information specific to a Stencil Processor.
 * One entry will be allocated for each processor on the IPU.
 *
 * Note that the processor id is stored with the paintbox_stp as a convenience
 * to avoid having to recover the processor_id from the pb->stps array when a
 * function only has the paintbox_stp object.
 */
struct paintbox_stp {
	/* Session list entry, A stencil processor may be allocated to a session
	 * or released using the PB_ALLOCATE_PROCESSOR and PB_RELEASE_PROCESSOR
	 * ioctls.
	 */
	struct list_head session_entry;
#if IS_ENABLED(CONFIG_IPU_DEBUG)
	struct paintbox_debug debug;
#endif
	struct paintbox_session *session;
	unsigned int stp_id;
	bool pm_enabled;
};

struct paintbox_stp_common {
	struct paintbox_stp *stps;
	unsigned int num_stps;
	uint64_t available_stp_mask;
};

struct paintbox_lbp;

/* Data structure for information specific to a Line Buffer.
 * One entry will be allocated for each line buffer in a pool.
 */
struct paintbox_lb {
#if IS_ENABLED(CONFIG_IPU_DEBUG)
	struct paintbox_debug debug;
#endif
	struct paintbox_lbp *lbp;
	unsigned int lb_id;
};

/* Data structure for information specific to a Line Buffer Pool.
 * One entry will be allocated for each pool on the IPU.
 *
 * Note that the pool id is stored with the paintbox_lbp as a convenience to
 * avoid having to recover the pool id from the pb->lbps array when a
 * function only has the paintbox_lbp object.
 */
struct paintbox_lbp {
	/* Session list entry, A line buffer pool may be allocated to a session
	 * or released using the PB_ALLOCATE_LINE_BUFFER_POOL and
	 * PB_RELEASE_LINE_BUFFER_POOL ioctls.
	 */
	struct list_head session_entry;
#if IS_ENABLED(CONFIG_IPU_DEBUG)
	struct paintbox_debug debug;
#endif
	struct paintbox_session *session;
	struct paintbox_lb *lbs;
	unsigned int pool_id;
	bool pm_enabled;
};

struct paintbox_lbp_common {
	struct paintbox_lbp *lbps;
	unsigned int num_lbps;
	uint32_t max_lbs;
	uint64_t available_lbp_mask;
};

struct paintbox_buffer {
	struct sg_table *sg_table;
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attach;
	uint32_t buffer_id;
	enum dma_data_direction dir;
};

struct paintbox_data {
	struct mutex lock;
	struct miscdevice misc_device;
	struct device *dev;

	/* Depending on the IOMMU configuration the board, the IPU driver may
	 * need to use a different device object for mapping into the IOMMU IOVA
	 * space.  This is the case on Airbrush when operating on a QCOM CPU.
	 */
	struct device *dma_dev;

	struct paintbox_lbp_common lbp;
	struct paintbox_stp_common stp;
	struct paintbox_power power;
	struct paintbox_dma dma;
	int session_count;
	struct idr session_idr;

	struct list_head bulk_alloc_waiting_list;

#if IS_ENABLED(CONFIG_IPU_DEBUG)
	struct paintbox_debug aon_debug;
	struct paintbox_debug apb_debug;
	struct paintbox_debug bif_debug;
	struct dentry *debug_root;
	struct dentry *regs_dentry;
#endif
};

int ipu_jqs_send_sync_message(struct paintbox_data *pb,
		const struct jqs_message *req);

#endif /* __IPU_CLIENT_H__ */
