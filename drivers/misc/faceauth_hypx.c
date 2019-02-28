/*
 * Google FaceAuth driver interface to hypx
 *
 * Copyright (C) 2018-2019 Google, Inc.
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

#include <asm/cacheflush.h>
#include <asm/compiler.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <misc/faceauth_hypx.h>
#include <soc/qcom/scm.h>
#include <soc/qcom/secure_buffer.h>

#define HYPX_SMC_ID(func) (0x43DEAD00 | func)
#define HYPX_SMC_FUNC_CHECK_PIL_COMPLETION HYPX_SMC_ID(0x1)
#define HYPX_SMC_FUNC_INIT HYPX_SMC_ID(0x2)
#define HYPX_SMC_FUNC_PROCESS HYPX_SMC_ID(0x3)
#define HYPX_SMC_FUNC_CHECK_PROCESS_RESULT HYPX_SMC_ID(0x4)
#define HYPX_SMC_FUNC_CLEANUP HYPX_SMC_ID(0x5)
#define HYPX_SMC_FUNC_GET_DEBUG_RESULT HYPX_SMC_ID(0x6)
#define HYPX_SMC_FUNC_GET_DEBUG_DATA HYPX_SMC_ID(0x7)
#define HYPX_SMC_FUNC_GET_DEBUG_BUFFER HYPX_SMC_ID(0x8)

#define MIN(x, y) ((x) < (y) ? (x) : (y))

#define PIL_DMA_TIMEOUT 3000
#define INPUT_IMAGE_WIDTH 480
#define INPUT_IMAGE_HEIGHT 640
#define DEBUG_DATA_BIN_SIZE (2 * 1024 * 1024)

struct hypx_mem_segment {
	/* address of the segment begin */
	uint32_t addr;
	/* number of pages in the segment */
	uint32_t pages;
} __packed;

#define HYPX_MEMSEGS_NUM (PAGE_SIZE / sizeof(struct hypx_mem_segment))

struct hypx_blob {
	struct hypx_mem_segment segments[HYPX_MEMSEGS_NUM];
} __packed;

/* Keep this struct in sync with HypX firmware code at
 * https://team.git.corp.google.com/soc-gerrit-admin/faceauth-hypx/
 * Put fields in order to avoid unaligned access that EL2 does not like
 */
struct hypx_fa_init {
	uint64_t verbosity_level;
	/* phy address for 4KiB temporary buffer used by EL2 DMA engine */
	uint64_t bounce_buff;
	uint64_t features;
} __packed;

struct hypx_fa_process {
	uint64_t image_dot_left; /* PHY addr */
	uint64_t image_dot_right; /* PHY addr */
	uint64_t image_flood; /* PHY addr */
	uint64_t calibration; /* PHY addr */

	uint32_t operation;
	uint32_t profile_id;

	uint32_t image_dot_left_size;
	uint32_t image_dot_right_size;
	uint32_t image_flood_size;
	uint32_t calibration_size;
} __packed;

struct hypx_fa_process_results {
	uint32_t result;
	uint32_t bin_result;
	uint32_t fw_version;
	int32_t error_code;
	uint64_t debug_buffer; /* PHY addr*/
	uint32_t debug_buffer_size;
} __packed;

struct hypx_fa_debug_data {
	uint64_t image_left;
	uint64_t image_right;
	uint64_t image_flood;
	uint64_t ab_state; /* PHY addr */
	uint64_t output_buffers;
	uint32_t offset_int_state;
	uint32_t offset_ab_state;
	uint32_t image_left_size;
	uint32_t image_right_size;
	uint32_t image_flood_size;
	uint32_t internal_state_struct_size;
	uint32_t buffer_list_size;
	uint32_t buffer_base;
} __packed;

struct faceauth_blob {
	enum dma_data_direction direction;
	struct hypx_blob *hypx_blob;
	__user void *buffer;
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attach;
	struct sg_table *sg_table;
};

static void hypx_free_blob_userbuf(phys_addr_t blob_phy, bool reassign)
{
	int source_vm[] = { VMID_EXT_DSP, VMID_HLOS_FREE };
	int dest_vm[] = { VMID_HLOS };
	int dest_perm[] = { PERM_READ | PERM_WRITE | PERM_EXEC };
	struct hypx_blob *blob = phys_to_virt(blob_phy);
	int i;

	for (i = 0; i < HYPX_MEMSEGS_NUM; i++) {
		uint64_t phy_addr;
		void *virt_addr;
		int ret = 0;

		if (!blob->segments[i].addr)
			break;
		phy_addr = (uint64_t)blob->segments[i].addr * PAGE_SIZE;
		virt_addr = phys_to_virt(phy_addr);

		if (reassign) {
			ret = hyp_assign_phys(
				phy_addr, blob->segments[i].pages * PAGE_SIZE,
				source_vm, ARRAY_SIZE(source_vm), dest_vm,
				dest_perm, ARRAY_SIZE(dest_vm));
			if (ret)
				pr_err("hyp_assign_phys returned an error %d\n",
				       ret);
		}

		kfree(virt_addr);
	}

	free_page((unsigned long)blob);
}

static int hypx_copy_from_blob_userbuf(void __user *buffer,
				       phys_addr_t blob_phy, size_t size,
				       bool copy_user)
{
	int source_vm[] = { VMID_EXT_DSP, VMID_HLOS_FREE };
	int dest_vm[] = { VMID_HLOS };
	int dest_perm[] = { PERM_READ | PERM_WRITE | PERM_EXEC };
	struct hypx_blob *blob = phys_to_virt(blob_phy);
	void __user *buffer_iter = buffer;
	uint64_t buffer_iter_remaining = size;
	uint64_t tocopy;
	int i;
	int lret, ret = 0;

	for (i = 0; i < HYPX_MEMSEGS_NUM; i++) {
		uint64_t phy_addr;
		void *virt_addr;

		if (!blob->segments[i].addr)
			break;
		tocopy = MIN(buffer_iter_remaining,
			     blob->segments[i].pages * PAGE_SIZE);
		phy_addr = (uint64_t)blob->segments[i].addr * PAGE_SIZE;
		virt_addr = phys_to_virt(phy_addr);

		lret = hyp_assign_phys(phy_addr,
				       blob->segments[i].pages * PAGE_SIZE,
				       source_vm, ARRAY_SIZE(source_vm),
				       dest_vm, dest_perm, ARRAY_SIZE(dest_vm));
		if (lret) {
			ret = lret;
			pr_err("hyp_assign_phys returned an error %d\n", ret);
		}
		if (copy_user) {
			ret = copy_to_user(buffer_iter, virt_addr, tocopy);
			if (ret) {
				pr_err("copy from blob cp failed: %d", ret);
				return -EFAULT;
			}
		} else {
			memcpy(buffer_iter, virt_addr, tocopy);
		}

		buffer_iter += tocopy;
		buffer_iter_remaining -= tocopy;
	}
	return ret;
}

/* Returns PHY address for the allocated buffer */
static phys_addr_t hypx_create_blob_userbuf(struct device *dev,
					    void __user *buffer, size_t size)
{
	uint64_t buffer_iter_remaining;
	const void __user *buffer_iter;
	struct hypx_blob *blob;
	struct hypx_mem_segment *segments_iter;
	int i;
	uint64_t page_order;

	/* note that allocated page is not reclaimable */
	blob = (void *)get_zeroed_page(0);
	if (!blob) {
		pr_err("Cannot allocate memory for hypx blob\n");
		goto exit;
	}
	WARN_ON(virt_to_phys(blob) % PAGE_SIZE);
	page_order = MIN(MAX_ORDER - 1, roundup_pow_of_two(size));
	buffer_iter = buffer;
	buffer_iter_remaining = size;
	segments_iter = blob->segments;

	for (i = 0; i < HYPX_MEMSEGS_NUM; i++) {
		uint64_t size = (1ULL << page_order) * PAGE_SIZE;
		void *out_buffer = kmalloc(size, 0);
		uint64_t tocopy = MIN(buffer_iter_remaining, size);

		int ret = 0;
		int source_vm[] = { VMID_HLOS };
		int dest_vm[] = { VMID_EXT_DSP, VMID_HLOS_FREE };
		int dest_perm[] = { PERM_READ | PERM_WRITE,
				    PERM_READ | PERM_WRITE };

		if (tocopy == 0) {
			kfree(out_buffer);
			break;
		}

		while (!out_buffer) {
			if (page_order == 0) {
				pr_err("Cannot allocate memory for copying data for hypx");
				goto exit;
			}
			page_order--;
			size = (1ULL << page_order) * PAGE_SIZE;
			out_buffer = kmalloc(size, 0);
			tocopy = MIN(buffer_iter_remaining, size);
		}

		copy_from_user(out_buffer, buffer_iter, tocopy);
		dma_sync_single_for_device(dev, virt_to_phys(out_buffer), size,
					   DMA_TO_DEVICE);

		/*
		 * In the future this hyp_assign call will be invoked for
		 * Camera buffers by camera stack itself.
		 * We will need to assign only calibration/debug buffers.
		 */
		ret = hyp_assign_phys(virt_to_phys(out_buffer), size, source_vm,
				      ARRAY_SIZE(source_vm), dest_vm, dest_perm,
				      ARRAY_SIZE(dest_vm));
		if (ret)
			pr_err("hyp_assign_phys returned an error %d\n", ret);

		segments_iter->addr = virt_to_phys(out_buffer) / PAGE_SIZE;
		segments_iter->pages = 1ULL << page_order;
		segments_iter++;

		buffer_iter_remaining -= tocopy;
		buffer_iter += tocopy;
	}

	if (buffer_iter_remaining) {
		pr_err("Memory allocator is fragmented so we were not able to fit %d into segments header\n",
		       size);
		goto exit;
	}

	dma_sync_single_for_device(dev, virt_to_phys(blob), PAGE_SIZE,
				   DMA_TO_DEVICE);

	return virt_to_phys(blob);

exit:
	hypx_free_blob_userbuf(virt_to_phys(blob), true);
	return 0;
}

/* Returns PHY address for the allocated buffer */
static dma_addr_t hypx_create_blob_dmabuf(struct device *dev,
					  struct faceauth_blob *blob,
					  int dmabuf_fd,
					  enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i, ret = 0;

	int source_vm[] = { VMID_HLOS };
	int dest_vm[] = { VMID_EXT_DSP, VMID_HLOS_FREE };
	int dest_perm[] = { PERM_READ | PERM_WRITE, PERM_READ | PERM_WRITE };

	blob->direction = dir;

	blob->dma_buf = dma_buf_get(dmabuf_fd);
	if (IS_ERR(blob->dma_buf)) {
		pr_err("dma_buf_get: %d\n", PTR_ERR(blob->dma_buf));
		goto err1;
	}

	/* prepare dma_buf for DMA */
	blob->attach = dma_buf_attach(blob->dma_buf, dev);
	if (IS_ERR(blob->attach)) {
		pr_err("dma_buf_attach: %d\n", PTR_ERR(blob->attach));
		goto err2;
	}

	/* map to get the sg_table */
	blob->sg_table = dma_buf_map_attachment(blob->attach, dir);
	if (IS_ERR(blob->sg_table)) {
		pr_err("dma_buf_map_attachment: %d\n", PTR_ERR(blob->sg_table));
		goto err3;
	}

	dma_sync_sg_for_device(dev, blob->sg_table->sgl, blob->sg_table->nents,
			       DMA_TO_DEVICE);

	/* struct hypx_blob struct have to be page aligned as we remap
	 * it to EL2 memory */
	blob->hypx_blob = (void *)get_zeroed_page(0);
	if (!blob->hypx_blob) {
		pr_err("Cannot allocate memory for hypx blob\n");
		goto err4;
	}
	if (WARN_ON(virt_to_phys(blob->hypx_blob) % PAGE_SIZE)) {
		pr_err("blob->hypx_blob is not PAGE aligned");
		goto err5;
	}
	for_each_sg (blob->sg_table->sgl, sg, blob->sg_table->nents, i) {
		WARN_ON(page_to_phys(sg_page(sg)) % PAGE_SIZE);
		blob->hypx_blob->segments[i].addr =
			page_to_phys(sg_page(sg)) / PAGE_SIZE;
		WARN_ON(sg->length % PAGE_SIZE);
		blob->hypx_blob->segments[i].pages = sg->length / PAGE_SIZE;
	}

	/*
	 * In the future this hyp_assign call will be invoked for
	 * Camera buffers by camera stack itself.
	 * We will need to assign only calibration/debug buffers.
	 */
	ret = hyp_assign_table(blob->sg_table, source_vm, ARRAY_SIZE(source_vm),
			       dest_vm, dest_perm, ARRAY_SIZE(dest_vm));
	if (ret) {
		pr_err("hyp_assign_table error: %d\n", ret);
		goto err5;
	}

	dma_sync_single_for_device(dev, virt_to_phys(blob->hypx_blob),
				   PAGE_SIZE, DMA_TO_DEVICE);
	return virt_to_phys(blob->hypx_blob);

err5:
	free_page((unsigned long)blob->hypx_blob);
err4:
	dma_buf_unmap_attachment(blob->attach, blob->sg_table, blob->direction);
	blob->sg_table = NULL;
err3:
	dma_buf_detach(blob->dma_buf, blob->attach);
err2:
	dma_buf_put(blob->dma_buf);
err1:
	return 0;
}

static void hypx_free_blob_dmabuf(struct device *dev,
				  struct faceauth_blob *blob)
{
	int source_vm[] = { VMID_EXT_DSP, VMID_HLOS_FREE };
	int dest_vm[] = { VMID_HLOS };
	int dest_perm[] = { PERM_READ | PERM_WRITE | PERM_EXEC };
	int ret = 0;

	ret = hyp_assign_table(blob->sg_table, source_vm, ARRAY_SIZE(source_vm),
			       dest_vm, dest_perm, ARRAY_SIZE(dest_vm));
	if (ret)
		pr_err("hyp_assign_table error: %d\n", ret);

	dma_buf_unmap_attachment(blob->attach, blob->sg_table, blob->direction);
	dma_buf_detach(blob->dma_buf, blob->attach);
	dma_buf_put(blob->dma_buf);
	free_page((unsigned long)blob->hypx_blob);
}

static dma_addr_t hypx_create_blob(struct device *dev,
				   struct faceauth_blob *blob,
				   void __user *buffer, int dma_fd, size_t size,
				   enum dma_data_direction dir)
{
	dma_addr_t hypx_blob_dma_addr;

	if (buffer) {
		blob->buffer = buffer;
		hypx_blob_dma_addr =
			hypx_create_blob_userbuf(dev, buffer, size);
		blob->hypx_blob = phys_to_virt(hypx_blob_dma_addr);
		return hypx_blob_dma_addr;
	} else {
		return hypx_create_blob_dmabuf(dev, blob, dma_fd, dir);
	}
}

static void hypx_free_blob(struct device *dev, struct faceauth_blob *blob)
{
	if (blob->buffer)
		return hypx_free_blob_userbuf(virt_to_phys(blob->hypx_blob),
					      true);
	else
		return hypx_free_blob_dmabuf(dev, blob);
}

int el2_faceauth_wait_pil_dma_over(void)
{
	struct scm_desc check_dma_desc = { 0 };
	int ret = 0;
	unsigned long stop;

	stop = jiffies + msecs_to_jiffies(PIL_DMA_TIMEOUT);

	do {
		check_dma_desc.arginfo = SCM_ARGS(0);

		ret = scm_call2(HYPX_SMC_FUNC_CHECK_PIL_COMPLETION,
				&check_dma_desc);
		if (ret)
			return ret;

		ret = check_dma_desc.ret[0];
		if (ret == 1) {
			/* DMA completed successfully */
			return 0;
		} else if (ret == 2) {
			/* DMA is still running */
			msleep(1);
		} else {
			/* EL2 call failed */
			return -EFAULT;
		}
		if (time_before(stop, jiffies)) {
			pr_err("PIL DMA timeout!\n");
			return -ETIME;
		}

	} while (true);
}

static void *bounce_buff;
static dma_addr_t bounce_buff_bus_addr;

int el2_faceauth_init(struct device *dev, struct faceauth_init_data *data,
		      uint64_t verbosity_level)
{
	int ret = 0;
	struct scm_desc desc = { 0 };
	struct hypx_fa_init *hypx_data;
	int source_vm[] = { VMID_HLOS };
	int dest_vm[] = { VMID_EXT_DSP, VMID_HLOS_FREE };
	int dest_perm[] = { PERM_READ | PERM_WRITE, PERM_READ | PERM_WRITE };

	hypx_data = (void *)get_zeroed_page(0);
	hypx_data->verbosity_level = verbosity_level;
	hypx_data->features = data->features;

	bounce_buff = dma_alloc_coherent(dev, PAGE_SIZE, &bounce_buff_bus_addr,
					 GFP_KERNEL);
	hypx_data->bounce_buff = bounce_buff_bus_addr;

	ret = hyp_assign_phys(bounce_buff_bus_addr, PAGE_SIZE, source_vm,
			      ARRAY_SIZE(source_vm), dest_vm, dest_perm,
			      ARRAY_SIZE(dest_vm));
	if (ret) {
		pr_err("hyp_assign_phys returned an error %d\n", ret);
		goto exit;
	}

	dma_sync_single_for_device(dev, virt_to_phys(hypx_data), PAGE_SIZE,
				   DMA_TO_DEVICE);

	desc.args[0] = virt_to_phys(hypx_data);
	desc.arginfo = SCM_ARGS(1);

	ret = scm_call2(HYPX_SMC_FUNC_INIT, &desc);
	if (ret) {
		pr_err("Failed scm_call %d\n", ret);
		goto exit;
	}

exit:
	free_page((unsigned long)hypx_data);
	return ret;
}

int el2_faceauth_cleanup(struct device *dev)
{
	int ret = 0;
	struct scm_desc desc = { 0 };
	int source_vm[] = { VMID_EXT_DSP, VMID_HLOS_FREE };
	int dest_vm[] = { VMID_HLOS };
	int dest_perm[] = { PERM_READ | PERM_WRITE | PERM_EXEC };

	desc.arginfo = SCM_ARGS(0);

	ret = scm_call2(HYPX_SMC_FUNC_CLEANUP, &desc);
	if (ret)
		pr_err("Failed scm_call %d\n", ret);

	ret = hyp_assign_phys(bounce_buff_bus_addr, PAGE_SIZE, source_vm,
			      ARRAY_SIZE(source_vm), dest_vm, dest_perm,
			      ARRAY_SIZE(dest_vm));
	if (ret)
		pr_err("hyp_assign_phys returned an error %d\n", ret);

	dma_free_coherent(dev, PAGE_SIZE, bounce_buff, bounce_buff_bus_addr);
	bounce_buff = NULL;
	bounce_buff_bus_addr = 0;

	return ret;
}

int el2_faceauth_process(struct device *dev, struct faceauth_start_data *data)
{
	int ret = 0;
	struct scm_desc desc = { 0 };
	bool pass_images_to_el2;
	struct hypx_fa_process *hypx_data;
	struct faceauth_blob image_dot_left = { 0 }, image_dot_right = { 0 },
			     image_flood = { 0 }, calibration = { 0 };

	pass_images_to_el2 = data->operation == FACEAUTH_OP_ENROLL ||
			     data->operation == FACEAUTH_OP_VALIDATE;

	hypx_data = (void *)get_zeroed_page(0);

	hypx_data->operation = data->operation;
	hypx_data->profile_id = data->profile_id;
	if (pass_images_to_el2) {
		hypx_data->image_dot_left = hypx_create_blob(
			dev, &image_dot_left, data->image_dot_left,
			data->image_dot_left_fd, data->image_dot_left_size,
			DMA_TO_DEVICE);
		hypx_data->image_dot_left_size = data->image_dot_left_size;
		if (!hypx_data->image_dot_left)
			goto err4;

		hypx_data->image_dot_right = hypx_create_blob(
			dev, &image_dot_right, data->image_dot_right,
			data->image_dot_right_fd, data->image_dot_right_size,
			DMA_TO_DEVICE);
		hypx_data->image_dot_right_size = data->image_dot_right_size;
		if (!hypx_data->image_dot_right)
			goto err3;

		hypx_data->image_flood =
			hypx_create_blob(dev, &image_flood, data->image_flood,
					 data->image_flood_fd,
					 data->image_flood_size, DMA_TO_DEVICE);
		hypx_data->image_flood_size = data->image_flood_size;
		if (!hypx_data->image_flood)
			goto err2;

		if (data->calibration || data->calibration_fd) {
			hypx_data->calibration = hypx_create_blob(
				dev, &calibration, data->calibration,
				data->calibration_fd, data->calibration_size,
				DMA_TO_DEVICE);
			hypx_data->calibration_size = data->calibration_size;
			if (!hypx_data->calibration)
				goto err1;
		}
	}

	dma_sync_single_for_device(dev, virt_to_phys(hypx_data), PAGE_SIZE,
				   DMA_TO_DEVICE);

	desc.args[0] = virt_to_phys(hypx_data);
	desc.arginfo = SCM_ARGS(1);

	ret = scm_call2(HYPX_SMC_FUNC_PROCESS, &desc);
	if (ret) {
		pr_err("Failed scm_call %d\n", ret);
	}

	if (data->calibration || data->calibration_fd)
		hypx_free_blob(dev, &calibration);

	if (pass_images_to_el2) {
	err1:
		hypx_free_blob(dev, &image_flood);
	err2:
		hypx_free_blob(dev, &image_dot_right);
	err3:
		hypx_free_blob(dev, &image_dot_left);
	}
err4:
	free_page((unsigned long)hypx_data);
	return ret;
}

int el2_faceauth_get_process_result(struct device *dev,
				    struct faceauth_start_data *data)
{
	int ret = 0;
	struct scm_desc desc = { 0 };
	struct hypx_fa_process_results *hypx_data;

	hypx_data = (void *)get_zeroed_page(0);

	dma_sync_single_for_device(dev, virt_to_phys(hypx_data), PAGE_SIZE,
				   DMA_FROM_DEVICE);

	desc.arginfo = SCM_ARGS(1);
	desc.args[0] = virt_to_phys(hypx_data);

	ret = scm_call2(HYPX_SMC_FUNC_CHECK_PROCESS_RESULT, &desc);
	if (ret) {
		pr_err("Failed scm_call %d\n", ret);
		goto exit;
	}
	dma_sync_single_for_cpu(dev, virt_to_phys(hypx_data), PAGE_SIZE,
				DMA_FROM_DEVICE);

	data->result = hypx_data->result;
	data->bin_bitmap = hypx_data->bin_result;
	data->fw_version = hypx_data->fw_version;
	data->error_code = hypx_data->error_code;

exit:
	free_page((unsigned long)hypx_data);
	return ret;
}

int el2_faceauth_gather_debug_log(struct device *dev,
				  struct faceauth_debug_data *data)
{
	int ret = 0;
	bool need_reassign = true;
	struct scm_desc desc = { 0 };
	struct hypx_fa_process_results *hypx_data;

	hypx_data = (void *)get_zeroed_page(0);

	hypx_data->debug_buffer = hypx_create_blob_userbuf(
		dev, data->debug_buffer, data->debug_buffer_size);
	hypx_data->debug_buffer_size = data->debug_buffer_size;

	if (!hypx_data->debug_buffer) {
		pr_err("Fail to alloc mem for debug_buffer");
		goto exit2;
	}

	dma_sync_single_for_device(dev, virt_to_phys(hypx_data), PAGE_SIZE,
				   DMA_BIDIRECTIONAL);

	desc.arginfo = SCM_ARGS(1);
	desc.args[0] = virt_to_phys(hypx_data);

	ret = scm_call2(HYPX_SMC_FUNC_GET_DEBUG_RESULT, &desc);
	if (ret) {
		pr_err("Failed scm_call %d\n", ret);
		goto exit1;
	}
	dma_sync_single_for_cpu(dev, virt_to_phys(hypx_data), PAGE_SIZE,
				DMA_FROM_DEVICE);

	ret = hypx_copy_from_blob_userbuf(data->debug_buffer,
					  hypx_data->debug_buffer,
					  data->debug_buffer_size, true);
	if (ret) {
		pr_err("Failed hypx_copy_from_blob_userbuf %d\n", ret);
		goto exit1;
	}
	need_reassign = false;

exit1:
	hypx_free_blob_userbuf(hypx_data->debug_buffer, need_reassign);
exit2:
	free_page((unsigned long)hypx_data);
	return ret;
}

int el2_gather_debug_data(struct device *dev, void *destination_buffer,
			  uint32_t buffer_size)
{
	int err = 0;

	struct faceauth_debug_entry *debug_entry = destination_buffer;
	uint32_t current_offset;
	struct faceauth_buffer_list *output_buffers;
	int buffer_idx;
	int buffer_list_size;
	bool need_reassign = true;
	struct hypx_fa_debug_data *hypx_data;
	struct scm_desc desc = { 0 };

	hypx_data = (void *)get_zeroed_page(0);
	hypx_data->offset_int_state =
		offsetof(struct faceauth_airbrush_state, internal_state_size);
	hypx_data->offset_ab_state =
		offsetof(struct faceauth_debug_entry, ab_state);

	/*
	 * We are going to copy things into these blobs,
	 * so we don't care their content. Re-using debug
	 * entry to reduce alloc mem. This works base on
	 * the fact that debug_entry is larger than image
	 * size.
	 */
	hypx_data->image_left_size = INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT;
	hypx_data->image_left = hypx_create_blob_userbuf(
		dev, debug_entry, hypx_data->image_left_size);
	if (!hypx_data->image_left)
		goto exit1;

	hypx_data->image_right_size = INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT;
	hypx_data->image_right = hypx_create_blob_userbuf(
		dev, debug_entry, hypx_data->image_right_size);
	if (!hypx_data->image_right)
		goto exit2;

	hypx_data->image_flood_size = INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT;
	hypx_data->image_flood = hypx_create_blob_userbuf(
		dev, debug_entry, hypx_data->image_flood_size);
	if (!hypx_data->image_flood)
		goto exit3;
	/*
	 * NOT sure the exact size, using a more than necessary size
	 */
	hypx_data->ab_state = hypx_create_blob_userbuf(
		dev, debug_entry, hypx_data->image_flood_size);
	if (!hypx_data->ab_state)
		goto exit;

	dma_sync_single_for_device(dev, virt_to_phys(hypx_data), PAGE_SIZE,
				   DMA_BIDIRECTIONAL);

	desc.args[0] = virt_to_phys(hypx_data);
	desc.arginfo = SCM_ARGS(1);

	err = scm_call2(HYPX_SMC_FUNC_GET_DEBUG_DATA, &desc);
	if (err) {
		pr_err("Failed scm_call %d\n", err);
		goto exit;
	}
	dma_sync_single_for_cpu(dev, virt_to_phys(hypx_data), PAGE_SIZE,
				DMA_FROM_DEVICE);

	err = hypx_copy_from_blob_userbuf(&(debug_entry->ab_state),
					  hypx_data->ab_state,
					  hypx_data->internal_state_struct_size,
					  false);
	if (err) {
		pr_err("Failed hypx_copy_from_blob_userbuf internal_state %d\n",
		       err);
		goto exit;
	}

	current_offset = offsetof(struct faceauth_debug_entry, ab_state) +
			 hypx_data->internal_state_struct_size;

	if (debug_entry->ab_state.command == FACEAUTH_OP_ENROLL ||
	    debug_entry->ab_state.command == FACEAUTH_OP_VALIDATE) {
		err = hypx_copy_from_blob_userbuf(
			(uint8_t *)debug_entry + current_offset,
			hypx_data->image_left, hypx_data->image_left_size,
			false);

		debug_entry->left_dot.offset_to_image = current_offset;
		debug_entry->left_dot.image_size =
			INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT;
		current_offset += INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT;

		if (err) {
			pr_err("Error saving left dot image\n");
			goto exit;
		}

		err = hypx_copy_from_blob_userbuf(
			(uint8_t *)debug_entry + current_offset,
			hypx_data->image_right, hypx_data->image_right_size,
			false);

		debug_entry->right_dot.offset_to_image = current_offset;
		debug_entry->right_dot.image_size =
			INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT;
		current_offset += INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT;
		if (err) {
			pr_err("Error saving right dot image\n");
			goto exit;
		}
		err = hypx_copy_from_blob_userbuf(
			(uint8_t *)debug_entry + current_offset,
			hypx_data->image_flood, hypx_data->image_flood_size,
			false);

		debug_entry->flood.offset_to_image = current_offset;
		debug_entry->flood.image_size =
			INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT;
		current_offset += INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT;
		if (err) {
			pr_err("Error saving flood image\n");
			goto exit;
		}
		need_reassign = false;
	} else {
		debug_entry->left_dot.offset_to_image = 0;
		debug_entry->left_dot.image_size = 0;
		debug_entry->right_dot.offset_to_image = 0;
		debug_entry->right_dot.image_size = 0;
		debug_entry->flood.offset_to_image = 0;
		debug_entry->flood.image_size = 0;
	}

	output_buffers = &(debug_entry->ab_state.output_buffers);
	buffer_idx = output_buffers->buffer_count - 1;
	buffer_list_size =
		output_buffers->buffers[buffer_idx].offset_to_buffer +
		output_buffers->buffers[buffer_idx].size;

	if (buffer_list_size + current_offset > DEBUG_DATA_BIN_SIZE) {
		err = -EMSGSIZE;
		pr_err("Wrong output buffer size\n");
		goto exit;
	}

	if (output_buffers->buffer_base != 0 && buffer_list_size > 0) {
		hypx_data->output_buffers = hypx_create_blob_userbuf(
			dev, debug_entry, buffer_list_size);
		hypx_data->buffer_list_size = buffer_list_size;
		hypx_data->buffer_base = output_buffers->buffer_base;

		if (!hypx_data->output_buffers)
			goto exit;

		dma_sync_single_for_device(dev, virt_to_phys(hypx_data),
					   PAGE_SIZE, DMA_BIDIRECTIONAL);

		err = scm_call2(HYPX_SMC_FUNC_GET_DEBUG_BUFFER, &desc);
		if (err)
			pr_err("Failed scm_call %d\n", err);

		dma_sync_single_for_cpu(dev, virt_to_phys(hypx_data), PAGE_SIZE,
					DMA_FROM_DEVICE);

		err = hypx_copy_from_blob_userbuf(
			(uint8_t *)debug_entry + current_offset,
			hypx_data->output_buffers, buffer_list_size, false);

		output_buffers->buffer_base = current_offset;
		current_offset += buffer_list_size;
		hypx_free_blob_userbuf(hypx_data->output_buffers, false);
	}

exit:
	hypx_free_blob_userbuf(hypx_data->ab_state, false);
exit3:
	hypx_free_blob_userbuf(hypx_data->image_flood, need_reassign);
exit2:
	hypx_free_blob_userbuf(hypx_data->image_right, need_reassign);
exit1:
	hypx_free_blob_userbuf(hypx_data->image_left, need_reassign);

	free_page((unsigned long)hypx_data);
	return err;
}
