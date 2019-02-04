/*
 * Google FaceAuth driver
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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/faceauth.h>
#include <linux/faceauth_shared.h>
#include <linux/firmware.h>
#include <misc/faceauth_hypx.h>

#include <linux/mfd/abc-pcie.h>

#include <linux/miscdevice.h>
#include <linux/time.h>
#include <linux/uio.h>
#include <linux/uaccess.h>

#include <abc-pcie-dma.h>
#include "../mfd/abc-pcie-dma.h"

/* ABC AON config regisetr offsets */
#define SYSREG_AON 0x30000
#define SYSREG_REG_GP_INT0 (SYSREG_AON + 0x37C)
#define SYSREG_AON_IPU_REG29 (SYSREG_AON + 0x438)
#define AB_RESULT_FLAG_ADDR (SYSREG_AON + 0x3C4)
#define BIN_RESULT_FLAG_ADDR (SYSREG_AON + 0x3C8)
#define INPUT_FLAG_ADDR (SYSREG_AON + 0x3CC)

/* ABC FW and workload binary offsets */
#define M0_FIRMWARE_ADDR 0x20000000
#define M0_VERBOSITY_LEVEL_FLAG_ADDR 0x21fffff0
#define FEATURES_FLAG_ADDR 0x21fffff8
#define DOT_IMAGE_LEFT_ADDR 0x22800000
#define DOT_IMAGE_RIGHT_ADDR 0x22900000
#define FLOOD_IMAGE_ADDR 0x23000000
#define CALIBRATION_ADDR 0x22C00000
#define CALIBRATION_SIZE 0x2000

#define AB_INTERNAL_STATE_ADDR 0x23e00000

#define DEBUG_PRINT_ADDR 0x23f00000
#define DEBUG_PRINT_SIZE 0x00100000

/* This is to be enabled for dog food only */
#define ENABLE_AIRBRUSH_DEBUG (1)

#if ENABLE_AIRBRUSH_DEBUG
#define DEBUG_DATA_BIN_SIZE (2 * 1024 * 1024)
#define DEBUG_DATA_NUM_BINS (5)
#endif

/* ABC FW and workload path */
#define M0_FIRMWARE_PATH "m0_workload.fw"

/* Timeout */
#define FACEAUTH_TIMEOUT 3000
#define M0_POLLING_PAUSE 100
#define M0_POLLING_INTERVAL 12

#define INPUT_IMAGE_WIDTH 480
#define INPUT_IMAGE_HEIGHT 640

static int dma_xfer(void *buf, int size, const int remote_addr,
		    enum dma_data_direction dir);
static int dma_xfer_vmalloc(void *buf, int size, const int remote_addr,
			    enum dma_data_direction dir);
static int dma_send_fw(const char *path, const int remote_addr);
static int dma_read_dw(struct file *file, const int remote_addr, int *val);
static int dma_send_images(struct faceauth_start_data *data);
static int pio_write_qw(const int remote_addr, const uint64_t val);
#if ENABLE_AIRBRUSH_DEBUG
static int dma_gather_debug_log(struct faceauth_debug_data *data);
static int dma_gather_debug_data(struct file *file, void *destination_buffer,
				 uint32_t buffer_size, uint32_t user_buffer);
static void enqueue_debug_data(struct file *file);
static int dequeue_debug_data(struct faceauth_debug_data *debug_step_data);

struct {
	int head_idx;
	int tail_idx;
	int count;
	char *data_buffer;
} debug_data_queue;
#endif // #if ENABLE_AIRBRUSH_DEBUG

static long faceauth_dev_ioctl_el1(struct file *file, unsigned int cmd,
				   unsigned long arg);
static long faceauth_dev_ioctl_el2(struct file *file, unsigned int cmd,
				   unsigned long arg);

struct faceauth_data {
	int dma_dw_buf;
};
bool hypx_enable;
uint64_t m0_verbosity_level;
struct dentry *faceauth_debugfs_root;
uint16_t session_id;
struct platform_device *faceauth_pdev;

/* M0 Verbosity Level Encoding

64 bits wide allocated as follows:
	Bit  0   Errors
	Bits 1-3 Performance
	Bits 4-7 Scheduler
	Bits 8-11 IPU
	Bits 12-15 TPU
	Bits 16-19 Post Process
	Bits 20-63 Reserved

In these slots, the debug levels are specified as follows:
	Level 0: 0b0000
	Level 1: 0b1000
	Level 2: 0b0100
	Level 3: 0b0010
	Level 4: 0b0001

Level 0 means errors only. The other levels yield increasingly more information.

To set all these levels, you must write the number in either
unsigned hexadecimal format or unisigned decimal format
to a certain file: /d/faceauth/m0_verbosity_level
If using hexadecimal, you need to put "0x" in front.
For example:
either
adb shell "echo 0x108248 > /d/faceauth/m0_verbosity_level"
or
adb shell "echo 1081928 > /d/faceauth/m0_verbosity_level"
will result in the following settings:
	general errors level 0 (meaning ON)
	performance level 2
	scheduler level 3
	IPU level 1
	TPU level 0
	post process level 4
*/

static long faceauth_dev_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	if (hypx_enable)
		return faceauth_dev_ioctl_el2(file, cmd, arg);
	else
		return faceauth_dev_ioctl_el1(file, cmd, arg);
}

static long faceauth_dev_ioctl_el1(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	int err = 0;
	int polling_interval = M0_POLLING_INTERVAL;
	struct faceauth_init_data init_step_data = { 0 };
	struct faceauth_start_data start_step_data = { 0 };
	unsigned long stop, ioctl_start, save_debug_jiffies;
	unsigned long save_debug_start = 0;
	unsigned long save_debug_end = 0;
	uint32_t ab_input;
	uint32_t ab_result;
	uint32_t bin_result;
	uint32_t dma_read_value;
#if ENABLE_AIRBRUSH_DEBUG
	struct faceauth_debug_data debug_step_data = { { 0 } };
	struct timeval timestamp;
	struct faceauth_debug_entry *debug_entry_ptr;
#endif

	ioctl_start = jiffies;

	switch (cmd) {
	case FACEAUTH_DEV_IOC_INIT:
		pr_info("faceauth init IOCTL\n");

		if (copy_from_user(&init_step_data, (const void __user *)arg,
				   sizeof(init_step_data))) {
			err = -EFAULT;
			goto exit;
		}

		err = dma_send_fw(M0_FIRMWARE_PATH, M0_FIRMWARE_ADDR);
		if (err) {
			pr_err("Error during M0 firmware transfer: %d\n", err);
			goto exit;
		}

		pio_write_qw(M0_VERBOSITY_LEVEL_FLAG_ADDR, m0_verbosity_level);
		pio_write_qw(FEATURES_FLAG_ADDR, init_step_data.features);

		break;
	case FACEAUTH_DEV_IOC_START:
		pr_info("faceauth start IOCTL\n");

		if (copy_from_user(&start_step_data, (const void __user *)arg,
				   sizeof(start_step_data))) {
			err = -EFAULT;
			goto exit;
		}

		start_step_data.result = 0;
		start_step_data.error_code = 0;

		if (start_step_data.operation == FACEAUTH_OP_ENROLL ||
		    start_step_data.operation == FACEAUTH_OP_VALIDATE) {
			if (!start_step_data.image_dot_left_size) {
				err = -EINVAL;
				goto exit;
			}
			if (!start_step_data.image_dot_right_size) {
				err = -EINVAL;
				goto exit;
			}
			if (!start_step_data.image_flood_size) {
				err = -EINVAL;
				goto exit;
			}
			if (start_step_data.calibration) {
				if (!start_step_data.calibration_size ||
				    start_step_data.calibration_size >
					    CALIBRATION_SIZE) {
					err = -EINVAL;
					goto exit;
				}
			}

			pr_info("Send images\n");
			err = dma_send_images(&start_step_data);
			if (err) {
				pr_err("Error in sending workload\n");
				goto exit;
			}

			if (start_step_data.calibration) {
				pr_info("Send calibration data\n");
				err = dma_xfer(start_step_data.calibration,
					       start_step_data.calibration_size,
					       CALIBRATION_ADDR, DMA_TO_DEVICE);
				if (err) {
					pr_err("Error sending calibration data\n");
					goto exit;
				}
			}
		}

		/* Set M0 firmware address */
		pr_info("Set M0 firmware addr = 0x%08x\n", M0_FIRMWARE_ADDR);
		err = aon_config_write(SYSREG_AON_IPU_REG29, 4,
				       M0_FIRMWARE_ADDR);
		if (err) {
			pr_err("Error setting faceauth FW address\n");
			goto exit;
		}

		/* Set input flag */
		ab_input = ((++session_id & 0xFFFF) << 16) |
			   ((start_step_data.profile_id & 0xFF) << 8) |
			   ((start_step_data.operation & 0xFF));
		pr_info("Set faceauth input flag = 0x%08x\n", ab_input);
		err = aon_config_write(INPUT_FLAG_ADDR, 4, ab_input);
		if (err) {
			pr_err("Error setting faceauth FW address\n");
			goto exit;
		}

		/* Reset completion flag */
		pr_info("Clearing completion flag at 0x%08x\n",
			AB_RESULT_FLAG_ADDR);
		err = aon_config_write(AB_RESULT_FLAG_ADDR, 4, 0);
		if (err) {
			pr_err("Error clearing completion flag\n");
			goto exit;
		}

		/* Trigger M0 Interrupt */
		pr_info("Interrupting M0\n");
		err = aon_config_write(SYSREG_REG_GP_INT0, 4, 1);
		if (err) {
			pr_err("Error interrupting AB\n");
			goto exit;
		}

		/* Check completion flag */
		pr_info("Waiting for completion.\n");
		msleep(M0_POLLING_PAUSE);
		stop = jiffies + msecs_to_jiffies(FACEAUTH_TIMEOUT);
		for (;;) {
			err = aon_config_read(AB_RESULT_FLAG_ADDR, 4,
					      &ab_result);
			if (err) {
				pr_err("Error reading completion flag\n");
				goto exit;
			}

			if (ab_result != WORKLOAD_STATUS_NO_STATUS) {
				pr_info("Faceauth workflow completes.\n");
				break;
			}
			if (time_before(stop, jiffies)) {
				pr_err("Faceauth workflow timeout!\n");
				err = -ETIME;
				goto exit;
			}
			msleep(polling_interval);
			polling_interval = polling_interval > 1 ?
						   polling_interval >> 1 :
						   1;
		}

#if ENABLE_AIRBRUSH_DEBUG
		save_debug_start = jiffies;
		enqueue_debug_data(file);
		save_debug_end = jiffies;
#endif

		err = aon_config_read(BIN_RESULT_FLAG_ADDR, 4, &bin_result);
		if (err) {
			pr_err("Error reading Bin result flag\n");
			goto exit;
		}
		start_step_data.result = ab_result;
		start_step_data.bin_bitmap = bin_result;

		dma_read_dw(file,
			    AB_INTERNAL_STATE_ADDR +
				    offsetof(struct faceauth_airbrush_state,
					     error_code),
			    &dma_read_value);
		start_step_data.error_code = dma_read_value;

		dma_read_dw(file,
			    AB_INTERNAL_STATE_ADDR +
				    offsetof(struct faceauth_airbrush_state,
					     faceauth_version),
			    &dma_read_value);
		start_step_data.fw_version = dma_read_value;

		if (copy_to_user((void __user *)arg, &start_step_data,
				 sizeof(start_step_data)))
			err = -EFAULT;
		goto exit;
		break;
	case FACEAUTH_DEV_IOC_CLEANUP:
		/* TODO cleanup Airbrush DRAM */
		pr_info("faceauth cleanup IOCTL\n");
		break;
	case FACEAUTH_DEV_IOC_DEBUG:
#if ENABLE_AIRBRUSH_DEBUG
		pr_info("faceauth debug log IOCTL\n");
		if (copy_from_user(&debug_step_data, (const void __user *)arg,
				   sizeof(debug_step_data))) {
			err = -EFAULT;
			goto exit;
		}
		err = dma_gather_debug_log(&debug_step_data);
#else
		err = -EOPNOTSUPP;
#endif // #if ENABLE_AIRBRUSH_DEBUG
		break;
	case FACEAUTH_DEV_IOC_DEBUG_DATA:
#if ENABLE_AIRBRUSH_DEBUG
		pr_info("faceauth debug data IOCTL\n");

		if (copy_from_user(&debug_step_data, (const void __user *)arg,
				   sizeof(debug_step_data))) {
			err = -EFAULT;
			goto exit;
		}

		if (debug_step_data.debug_buffer_size < DEBUG_DATA_BIN_SIZE) {
			err = -EINVAL;
			goto exit;
		}

		if (debug_step_data.flags == GET_DEBUG_DATA_FROM_AB_DRAM) {
			do_gettimeofday(&timestamp);
			debug_entry_ptr = (struct faceauth_debug_entry *)
						  debug_step_data.debug_buffer;
			copy_to_user(&(debug_entry_ptr->timestamp), &timestamp,
				     sizeof(struct timeval));

			err = dma_gather_debug_data(
				file, debug_step_data.debug_buffer,
				debug_step_data.debug_buffer_size, 1);
		} else {
			err = dequeue_debug_data(&debug_step_data);
		}
#else
		err = -EOPNOTSUPP;
#endif // #if ENABLE_AIRBRUSH_DEBUG
		break;
	default:
		err = -EFAULT;
		goto exit;
	}

exit:
	save_debug_jiffies = save_debug_end - save_debug_start;

	if (save_debug_jiffies > 0) {
		pr_info("Faceauth action took %dus, debug data save took %dus\n",
			jiffies_to_usecs(jiffies - ioctl_start -
					 save_debug_jiffies),
			jiffies_to_usecs(save_debug_jiffies));
	} else {
		pr_info("Faceauth action took %dus\n",
			jiffies_to_usecs(jiffies - ioctl_start));
	}

	return err;
}

static long faceauth_dev_ioctl_el2(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	int err = 0;
	int polling_interval = M0_POLLING_INTERVAL;
	struct faceauth_start_data start_step_data = { 0 };
	struct faceauth_init_data init_step_data = { 0 };
	unsigned long stop, ioctl_start;
	bool send_images_data;

	ioctl_start = jiffies;

	switch (cmd) {
	case FACEAUTH_DEV_IOC_INIT:
		pr_info("el2: faceauth init IOCTL\n");

		if (copy_from_user(&init_step_data, (const void __user *)arg,
				   sizeof(init_step_data))) {
			err = -EFAULT;
			goto exit;
		}

		err = el2_faceauth_wait_pil_dma_over();
		if (err < 0)
			goto exit;

		err = el2_faceauth_init(&faceauth_pdev->dev, &init_step_data,
					m0_verbosity_level);
		if (err < 0)
			goto exit;

		break;
	case FACEAUTH_DEV_IOC_START:
		pr_info("el2: faceauth start IOCTL\n");

		if (copy_from_user(&start_step_data, (const void __user *)arg,
				   sizeof(start_step_data))) {
			err = -EFAULT;
			goto exit;
		}

		send_images_data =
			start_step_data.operation == FACEAUTH_OP_ENROLL ||
			start_step_data.operation == FACEAUTH_OP_VALIDATE;
		if (send_images_data) {
			if (!start_step_data.image_dot_left_size) {
				err = -EINVAL;
				goto exit;
			}
			if (!start_step_data.image_dot_right_size) {
				err = -EINVAL;
				goto exit;
			}
			if (!start_step_data.image_flood_size) {
				err = -EINVAL;
				goto exit;
			}
		}

		err = el2_faceauth_process(&start_step_data);
		if (err)
			goto exit;

		/* Check completion flag */
		pr_info("Waiting for completion.\n");
		msleep(M0_POLLING_PAUSE);
		stop = jiffies + msecs_to_jiffies(FACEAUTH_TIMEOUT);
		for (;;) {
			err = el2_faceauth_get_process_result(&start_step_data);

			if (err) {
				pr_err("Failed to get process results from EL2 %d\n",
				       err);
				goto exit;
			}

			if (start_step_data.result !=
			    WORKLOAD_STATUS_NO_STATUS) {
				/* We've got a non-zero status from AB executor
				 * Faceauth processing is completed
				 */
				break;
			}
			if (time_before(stop, jiffies)) {
				err = -ETIME;
				goto exit;
			}

			msleep(polling_interval);
			polling_interval = polling_interval > 1 ?
						   polling_interval >> 1 :
						   1;
		}

		if (copy_to_user((void __user *)arg, &start_step_data,
				 sizeof(start_step_data))) {
			err = -EFAULT;
			goto exit;
		}

		break;
	case FACEAUTH_DEV_IOC_CLEANUP:
		/* In case of EL2 cleanup happens in PIL callback */
		/* TODO cleanup Airbrush DRAM */
		pr_info("el2: faceauth cleanup IOCTL\n");
		el2_faceauth_cleanup(&faceauth_pdev->dev);
		break;
	default:
		err = -EOPNOTSUPP;
		goto exit;
	}

exit:
	pr_info("Faceauth action took %d us\n",
		jiffies_to_usecs(jiffies - ioctl_start));
	return err;
}

static int faceauth_open(struct inode *inode, struct file *file)
{
	struct faceauth_data *data;

	data = vmalloc(sizeof(*data));
	if (!data) {
		pr_err("Failed to vmalloc DW buffer\n");
		return -ENOMEM;
	}
	file->private_data = (void *)data;

	return 0;
}

static int faceauth_free(struct inode *inode, struct file *file)
{
	struct faceauth_data *data = file->private_data;

	vfree(data);
	return 0;
}

static const struct file_operations faceauth_dev_operations = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = faceauth_dev_ioctl,
	.compat_ioctl = faceauth_dev_ioctl,
	.open = faceauth_open,
	.release = faceauth_free,
};

static struct miscdevice faceauth_miscdevice = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "faceauth",
	.fops = &faceauth_dev_operations,
};

/**
 * Local function to transfer data between user space memory and Airbrush via
 * PCIE
 * @param[in] buf Address of user space buffer
 * @param[in] size Size of buffer
 * @param[in] remote_addr Address of Airbrush memory
 * @param[in] dir Direction of data transfer
 * @return Status, zero if succeed, non-zero if fail
 */
static int dma_xfer(void *buf, int size, const int remote_addr,
		    enum dma_data_direction dir)
{
	struct abc_pcie_dma_desc dma_desc;
	int err = 0;

	/* Transfer workload to target memory in Airbrush */
	memset(&dma_desc, 0, sizeof(dma_desc));
	dma_desc.local_buf = buf;
	dma_desc.local_buf_type = DMA_BUFFER_USER;
	dma_desc.remote_buf = remote_addr;
	dma_desc.remote_buf_type = DMA_BUFFER_USER;
	dma_desc.size = size;
	dma_desc.dir = dir;
	err = abc_pcie_issue_dma_xfer(&dma_desc);
	return err;
}

/**
 * Local function to transfer data between kernel vmalloc memory and Airbrush
 * via PCIE
 * @param[in] buf Address of kernel vmalloc memory buffer
 * @param[in] size Size of buffer
 * @param[in] remote_addr Address of Airbrush memory
 * @param[in] dir Direction of data transfer
 * @return Status, zero if succeed, non-zero if fail
 */
static int dma_xfer_vmalloc(void *buf, int size, const int remote_addr,
			    enum dma_data_direction dir)
{
	struct abc_pcie_dma_desc dma_desc;
	int err = 0;

	/* Transfer workload to target memory in Airbrush */
	memset(&dma_desc, 0, sizeof(dma_desc));
	dma_desc.local_buf = buf;
	dma_desc.local_buf_type = DMA_BUFFER_USER;
	dma_desc.remote_buf = remote_addr;
	dma_desc.remote_buf_type = DMA_BUFFER_USER;
	dma_desc.size = size;
	dma_desc.dir = dir;
	err = abc_pcie_issue_dma_xfer_vmalloc(&dma_desc);
	return err;
}

/**
 * Local function to send firmware to Airbrush memory via PCIE
 * @param[in] path Firmware
 * @param[in] remote_addr Address of Airbrush memory
 * @return Status, zero if succeed, non-zero if fail
 */
static int dma_send_fw(const char *path, const int remote_addr)
{
	int err = 0;
	const struct firmware *fw_entry;
	int fw_status;

	fw_status = request_firmware(&fw_entry, path,
				     faceauth_miscdevice.this_device);
	if (fw_status != 0) {
		pr_err("Firmware Not Found: %d\n", fw_status);
		return -EIO;
	}

	err = dma_xfer_vmalloc((void *)fw_entry->data, fw_entry->size,
			       remote_addr, DMA_TO_DEVICE);
	if (err)
		pr_err("Error from abc_pcie_issue_dma_xfer: %d\n", err);
	release_firmware(fw_entry);
	return err;
}

/**
 * Local function to read one DW to Airbrush memory via PCIE
 * @param[in] file File struct of this module
 * @param[in] remote_addr Address of Airbrush memory
 * @param[in] val Variable to store read-back DW
 * @return Status, zero if succeed, non-zero if fail
 */
static int dma_read_dw(struct file *file, const int remote_addr, int *val)
{
	int err = 0;
	struct faceauth_data *data = file->private_data;

	err = dma_xfer_vmalloc(&data->dma_dw_buf, sizeof(*val), remote_addr,
			       DMA_FROM_DEVICE);
	if (err) {
		pr_err("Error from abc_pcie_issue_dma_xfer: %d\n", err);
		return err;
	}
	*val = data->dma_dw_buf;
	return 0;
}

/**
 * Local function to send FaceAuth input data to Airbrush memory via PCIE
 * @param[in] data Data structure copied from user space
 * @return Status, zero if succeed, non-zero if fail
 */
static int dma_send_images(struct faceauth_start_data *data)
{
	int err = 0;

	pr_info("Send left dot image\n");
	err = dma_xfer(data->image_dot_left, data->image_dot_left_size,
		       DOT_IMAGE_LEFT_ADDR, DMA_TO_DEVICE);
	if (err) {
		pr_err("Error sending left dot image\n");
		return err;
	}

	pr_info("Send right dot image\n");
	err = dma_xfer(data->image_dot_right, data->image_dot_right_size,
		       DOT_IMAGE_RIGHT_ADDR, DMA_TO_DEVICE);
	if (err) {
		pr_err("Error sending right dot image\n");
		return err;
	}

	/* This is data to feed individual TPU stages */
	pr_info("Send flood image\n");
	err = dma_xfer(data->image_flood, data->image_flood_size,
		       FLOOD_IMAGE_ADDR, DMA_TO_DEVICE);
	if (err) {
		pr_err("Error sending flood image\n");
		return err;
	}

	return err;
}

#if ENABLE_AIRBRUSH_DEBUG
static int dma_gather_debug_log(struct faceauth_debug_data *data)
{
	int err = 0;

	err = dma_xfer((void *)data->debug_buffer,
		       min((uint32_t)DEBUG_PRINT_SIZE, data->debug_buffer_size),
		       DEBUG_PRINT_ADDR, DMA_FROM_DEVICE);

	return err;
}

static void enqueue_debug_data(struct file *file)
{
	void *bin_addr;
	int err;

	bin_addr = debug_data_queue.data_buffer +
		   (DEBUG_DATA_BIN_SIZE * debug_data_queue.head_idx);
	err = dma_gather_debug_data(file, bin_addr, DEBUG_DATA_BIN_SIZE, 0);
	if (err) {
		return;
	}

	debug_data_queue.head_idx =
		(debug_data_queue.head_idx + 1) % DEBUG_DATA_NUM_BINS;

	if (debug_data_queue.count == DEBUG_DATA_NUM_BINS) {
		debug_data_queue.tail_idx =
			(debug_data_queue.tail_idx + 1) % DEBUG_DATA_NUM_BINS;
	} else {
		debug_data_queue.count++;
	}

	do_gettimeofday(
		&(((struct faceauth_debug_entry *)bin_addr)->timestamp));
}

static int dequeue_debug_data(struct faceauth_debug_data *debug_step_data)
{
	void *bin_addr;

	if (debug_data_queue.count == 0) {
		return -ENODATA;
	}

	if (debug_step_data->debug_buffer_size < DEBUG_DATA_BIN_SIZE) {
		return -ENOBUFS;
	}

	bin_addr = debug_data_queue.data_buffer +
		   (DEBUG_DATA_BIN_SIZE * debug_data_queue.tail_idx);
	debug_data_queue.tail_idx =
		(debug_data_queue.tail_idx + 1) % DEBUG_DATA_NUM_BINS;
	debug_data_queue.count--;

	if (copy_to_user((void __user *)(debug_step_data->debug_buffer),
			 bin_addr, DEBUG_DATA_BIN_SIZE)) {
		return -EFAULT;
	}

	return 0;
}

static int dma_gather_debug_data(struct file *file, void *destination_buffer,
				 uint32_t buffer_size, uint32_t user_buffer)
{
	int err = 0;
	uint32_t internal_state_struct_size;
	uint32_t command;
	struct faceauth_debug_entry *debug_entry = destination_buffer;
	uint32_t current_offset;
	struct faceauth_debug_image input_image;

	dma_read_dw(file,
		    AB_INTERNAL_STATE_ADDR +
			    offsetof(struct faceauth_airbrush_state,
				     internal_state_size),
		    &internal_state_struct_size);

	current_offset = offsetof(struct faceauth_debug_entry, ab_state) +
			 internal_state_struct_size;

	if (current_offset + (3 * INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT) >
	    buffer_size) {
		err = -EINVAL;
		pr_err("");
		return err;
	}

	if (user_buffer) {
		// save the AB internal state
		err = dma_xfer(&(debug_entry->ab_state),
			       internal_state_struct_size,
			       AB_INTERNAL_STATE_ADDR, DMA_FROM_DEVICE);
		if (err) {
			pr_err("failed to gather debug data, err %d\n", err);
			return err;
		}

		dma_read_dw(file,
			    AB_INTERNAL_STATE_ADDR +
				    offsetof(struct faceauth_airbrush_state,
					     command),
			    &command);

		if (command == FACEAUTH_OP_ENROLL ||
		    command == FACEAUTH_OP_VALIDATE) {
			// save left dot image
			err = dma_xfer(((uint8_t *)debug_entry) +
					       current_offset,
				       (INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT),
				       DOT_IMAGE_LEFT_ADDR, DMA_FROM_DEVICE);
			input_image.offset_to_image = current_offset;
			input_image.image_size =
				(INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT);
			current_offset += input_image.image_size;

			if (copy_to_user(&(debug_entry->left_dot), &input_image,
					 sizeof(input_image))) {
				err = -EFAULT;
			}

			if (err) {
				pr_err("Error saving left dot image\n");
				return err;
			}

			// save right dot image
			err = dma_xfer(((uint8_t *)debug_entry) +
					       current_offset,
				       (INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT),
				       DOT_IMAGE_RIGHT_ADDR, DMA_FROM_DEVICE);
			input_image.offset_to_image = current_offset;
			current_offset +=
				(INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT);

			if (copy_to_user(&(debug_entry->right_dot),
					 &input_image, sizeof(input_image))) {
				err = -EFAULT;
			}

			if (err) {
				pr_err("Error saving right dot image\n");
				return err;
			}

			// save the flood image
			err = dma_xfer(((uint8_t *)debug_entry) +
					       current_offset,
				       (INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT),
				       FLOOD_IMAGE_ADDR, DMA_FROM_DEVICE);
			input_image.offset_to_image = current_offset;
			current_offset +=
				(INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT);

			if (copy_to_user(&(debug_entry->flood), &input_image,
					 sizeof(input_image))) {
				err = -EFAULT;
			}

			if (err) {
				pr_err("Error saving flood image\n");
				return err;
			}
		} else {
			input_image.offset_to_image = 0;
			input_image.image_size = 0;
			if (copy_to_user(&(debug_entry->left_dot), &input_image,
					 sizeof(input_image)) ||
			    copy_to_user(&(debug_entry->right_dot),
					 &input_image, sizeof(input_image)) ||
			    copy_to_user(&(debug_entry->flood), &input_image,
					 sizeof(input_image))) {
				pr_err("Error saving input image data\n");
				return -EFAULT;
			}
		}
	} else {
		err = dma_xfer_vmalloc(&(debug_entry->ab_state),
				       internal_state_struct_size,
				       AB_INTERNAL_STATE_ADDR, DMA_FROM_DEVICE);
		if (err) {
			pr_err("failed to gather debug data, err %d\n", err);
			return err;
		}

		if (debug_entry->ab_state.command == FACEAUTH_OP_ENROLL ||
		    debug_entry->ab_state.command == FACEAUTH_OP_VALIDATE) {
			err = dma_xfer_vmalloc(
				((uint8_t *)debug_entry) + current_offset,
				(INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT),
				DOT_IMAGE_LEFT_ADDR, DMA_FROM_DEVICE);
			debug_entry->left_dot.offset_to_image = current_offset;
			debug_entry->left_dot.image_size =
				(INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT);
			current_offset +=
				(INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT);
			if (err) {
				pr_err("Error saving left dot image\n");
				return err;
			}

			err = dma_xfer_vmalloc(
				((uint8_t *)debug_entry) + current_offset,
				(INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT),
				DOT_IMAGE_RIGHT_ADDR, DMA_FROM_DEVICE);
			debug_entry->right_dot.offset_to_image = current_offset;
			debug_entry->right_dot.image_size =
				(INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT);
			current_offset +=
				(INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT);
			if (err) {
				pr_err("Error saving right dot image\n");
				return err;
			}

			err = dma_xfer_vmalloc(
				((uint8_t *)debug_entry) + current_offset,
				(INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT),
				FLOOD_IMAGE_ADDR, DMA_FROM_DEVICE);
			debug_entry->flood.offset_to_image = current_offset;
			debug_entry->flood.image_size =
				(INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT);
			current_offset +=
				(INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT);
			if (err) {
				pr_err("Error saving flood image\n");
				return err;
			}
		} else {
			debug_entry->left_dot.offset_to_image = 0;
			debug_entry->left_dot.image_size = 0;
			debug_entry->right_dot.offset_to_image = 0;
			debug_entry->right_dot.image_size = 0;
			debug_entry->flood.offset_to_image = 0;
			debug_entry->flood.image_size = 0;
		}
	}

	return err;
}
#endif // #if ENABLE_AIRBRUSH_DEBUG

/*
 * Local function to write a QW from user space memory to Airbrush via
 * PCIE by PIO.
 * @param[in] remote Airbrush physical address
 * @param[in] QW data (64 bits) to write
 * @return Status, zero if succeed, non-zero if fail
 */
static int pio_write_qw(const int remote_addr, const uint64_t val)
{
	// This has to be performed as two separate 32 bit writes because
	// Lassen's driver has a bug. See drivers/mfd/abc_pcie.c line 368
	uint32_t lower = (uint32_t)val;
	uint32_t upper = (uint32_t)(val >> 32);

	int err = 0;

	err = memory_config_write(remote_addr, 4, lower);

	if (err) {
		pr_err("Error in writing data to Airbrush at address 0x%08x\n",
		       remote_addr);
		return err;
	}

	err = memory_config_write(remote_addr + 4, 4, upper);

	if (err) {
		pr_err("Error in writing data to Airbrush at address 0x%08x\n",
		       remote_addr);
		return err;
	}

	return 0;
}

static int faceauth_hypx_enable_set(void *data, u64 val)
{
	hypx_enable = !!val;
	return 0;
}

static int faceauth_hypx_enable_get(void *data, u64 *val)
{
	*val = hypx_enable;
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_hypx_enable, faceauth_hypx_enable_get,
			 faceauth_hypx_enable_set, "%llu\n");

static int faceauth_m0_verbosity_set(void *data, u64 val)
{
	m0_verbosity_level = val;
	return 0;
}

static int faceauth_m0_verbosity_get(void *data, u64 *val)
{
	*val = m0_verbosity_level;
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_m0_verbosity, faceauth_m0_verbosity_get,
			 faceauth_m0_verbosity_set, "0x%016llx\n");

static int faceauth_probe(struct platform_device *pdev)
{
	int err;
	struct dentry *hypx, *m0_verbosity_level;
#if ENABLE_AIRBRUSH_DEBUG
	int i;
#endif

	hypx_enable = false;
	m0_verbosity_level = 0;

	err = misc_register(&faceauth_miscdevice);
	if (err)
		goto exit1;

	faceauth_debugfs_root = debugfs_create_dir("faceauth", NULL);
	if (IS_ERR_OR_NULL(faceauth_debugfs_root)) {
		pr_err("Failed to create faceauth debugfs");
		err = -EIO;
		goto exit2;
	}

	hypx = debugfs_create_file("hypx_enable", 0400, faceauth_debugfs_root,
				   NULL, &fops_hypx_enable);
	if (!hypx) {
		err = -EIO;
		goto exit3;
	}

	m0_verbosity_level = debugfs_create_file("m0_verbosity_level", 0400,
						 faceauth_debugfs_root, NULL,
						 &fops_m0_verbosity);
	if (!m0_verbosity_level) {
		err = -EIO;
		goto exit3;
	}

#if ENABLE_AIRBRUSH_DEBUG
	debug_data_queue.count = 0;
	debug_data_queue.head_idx = 0;
	debug_data_queue.tail_idx = 0;
	debug_data_queue.data_buffer =
		vmalloc(DEBUG_DATA_BIN_SIZE * DEBUG_DATA_NUM_BINS);

	if (debug_data_queue.data_buffer == NULL) {
		err = -ENOMEM;
		goto exit3;
	}

	for (i = 0; i < DEBUG_DATA_NUM_BINS; i++) {
		memset(debug_data_queue.data_buffer + (DEBUG_DATA_BIN_SIZE * i),
		       0, sizeof(struct faceauth_debug_entry));
	}
#endif

	return 0;

exit3:
	debugfs_remove_recursive(faceauth_debugfs_root);

exit2:
	misc_deregister(&faceauth_miscdevice);

exit1:
	return err;
}

static int faceauth_remove(struct platform_device *pdev)
{
	misc_deregister(&faceauth_miscdevice);
	debugfs_remove_recursive(faceauth_debugfs_root);

#if ENABLE_AIRBRUSH_DEBUG
	debug_data_queue.count = 0;
	debug_data_queue.head_idx = 0;
	debug_data_queue.tail_idx = 0;
	vfree(debug_data_queue.data_buffer);
#endif

	return 0;
}

static struct platform_driver faceauth_driver = {
	.probe = faceauth_probe,
	.remove = faceauth_remove,
	.driver = {
		.name = "faceauth",
		.owner = THIS_MODULE,
	},
};

static int __init faceauth_init(void)
{
	int ret;

	faceauth_pdev =
		platform_device_register_simple("faceauth", -1, NULL, 0);
	if (IS_ERR(faceauth_pdev))
		return PTR_ERR(faceauth_pdev);
	arch_setup_dma_ops(&faceauth_pdev->dev, 0, U64_MAX, NULL, false);

	ret = platform_driver_register(&faceauth_driver);
	if (ret)
		platform_device_unregister(faceauth_pdev);

	return 0;
}
module_init(faceauth_init);

static void __exit faceauth_exit(void)
{
	platform_driver_unregister(&faceauth_driver);
	platform_device_unregister(faceauth_pdev);
}
module_exit(faceauth_exit);

MODULE_AUTHOR("Anatol Pomazau <anatol@google.com>, Lei Liu <leliu@google.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Google FaceAuth driver");
