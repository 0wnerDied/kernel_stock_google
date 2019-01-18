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
#include <linux/firmware.h>

#include <linux/mfd/abc-pcie.h>

#include <linux/miscdevice.h>
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
#define JQS_DEPTH_ADDR 0x22000000
#define JQS_AFFINE_16_ADDR 0x22100000
#define JQS_AFFINE_RGB_ADDR 0x22200000
#define JQS_AFFINE_8_ADDR 0x22300000
#define DOT_IMAGE_LEFT_ADDR 0x22800000
#define DOT_IMAGE_RIGHT_ADDR 0x22900000
#define FLOOD_IMAGE_ADDR 0x23000000

#define AB_INTERNAL_STATE_ADDR 0x23e00000

#define DEBUG_PRINT_ADDR 0x23f00000
#define DEBUG_PRINT_SIZE 0x00100000

/* ABC FW and workload path */
#define M0_FIRMWARE_PATH "m0_workload.fw"
#define JQS_DEPTH_PATH "depth.fw"
#define JQS_AFFINE_8_PATH "affine_8.fw"
#define JQS_AFFINE_16_PATH "affine_16.fw"
#define JQS_AFFINE_RGB_PATH "affine_rgb.fw"

/* Timeout */
#define FACEAUTH_TIMEOUT 3000
#define M0_POLLING_PAUSE 400
#define M0_POLLING_INTERVAL 12

/*
 * Result codes from AB firmware
 * Keep it in sync with fw/include/defines.h
 */
#define AB_WORKLOAD_STATUS_NO_STATUS 0
#define AB_WORKLOAD_STATUS_PASS 1
#define AB_WORKLOAD_STATUS_FAIL 2
#define AB_WORKLOAD_STATUS_ERROR 3

struct airbrush_state {
	uint32_t faceauth_version;
	int32_t error_code;
} __attribute__((packed));

static int dma_xfer(void *buf, int size, const int remote_addr,
		    enum dma_data_direction dir);
static int dma_xfer_vmalloc(void *buf, int size, const int remote_addr,
			    enum dma_data_direction dir);
static int dma_send_fw(const char *path, const int remote_addr);
static int dma_write_dw(struct file *file, const int remote_addr,
			const int val);
static int dma_read_dw(struct file *file, const int remote_addr, int *val);
static int dma_send_images(struct faceauth_start_data *data);
static int dma_send_workloads(void);
static int dma_gather_debug(struct faceauth_debug_data *data);
static int pio_write_qw(const int remote_addr, const uint64_t val);

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
	int err = 0;
	int polling_interval = M0_POLLING_INTERVAL;
	struct faceauth_start_data start_step_data = { 0 };
	struct faceauth_continue_data continue_step_data = { 0 };
	struct faceauth_debug_data debug_step_data = { 0 };
	unsigned long stop, ioctl_start;
	uint32_t ab_input;
	uint32_t ab_result;
	uint32_t bin_result;
	uint32_t dma_read_value;

	ioctl_start = jiffies;

	switch (cmd) {
	case FACEAUTH_DEV_IOC_INIT:
		pr_info("faceauth init IOCTL\nSend faceauth workloads\n");

		err = dma_send_workloads();
		if (err) {
			pr_err("Error in sending M0 firmware\n");
			goto exit;
		}

		pio_write_qw(M0_VERBOSITY_LEVEL_FLAG_ADDR, m0_verbosity_level);

		break;
	case FACEAUTH_DEV_IOC_START:
		pr_info("faceauth start IOCTL\n");

		if (copy_from_user(&start_step_data, (const void __user *)arg,
				   sizeof(start_step_data))) {
			err = -EFAULT;
			goto exit;
		}

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
			/* TODO(kramm): This also needs to transfer the
			 * calibration data, once we have a combined Halide
			 * generator that includes rectification.
			 */
			pr_info("Send images\n");
			err = dma_send_images(&start_step_data);
			if (err) {
				pr_err("Error in sending workload\n");
				goto exit;
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

			if (ab_result != AB_WORKLOAD_STATUS_NO_STATUS) {
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

		break;
	case FACEAUTH_DEV_IOC_CONTINUE:
		pr_info("faceauth continue IOCTL\n");

		err = aon_config_read(AB_RESULT_FLAG_ADDR, 4, &ab_result);
		if (err) {
			pr_err("Error reading AB result flag\n");
			goto exit;
		}
		continue_step_data.result =
			ab_result == AB_WORKLOAD_STATUS_PASS ?
				FACEAUTH_RESULT_SUCCESS :
				FACEAUTH_RESULT_FAILURE;
		continue_step_data.completed = 1;

		err = aon_config_read(BIN_RESULT_FLAG_ADDR, 4, &bin_result);
		if (err) {
			pr_err("Error reading Bin result flag\n");
			goto exit;
		}
		continue_step_data.bin_bitmap = bin_result;

		pr_info("Read ab error code\n");
		dma_read_dw(file,
			    AB_INTERNAL_STATE_ADDR +
				    offsetof(struct airbrush_state, error_code),
			    &dma_read_value);
		continue_step_data.faceauth_error_code = dma_read_value;

		pr_info("Read ab firmware version\n");
		dma_read_dw(file,
			    AB_INTERNAL_STATE_ADDR +
				    offsetof(struct airbrush_state,
					     faceauth_version),
			    &dma_read_value);
		continue_step_data.faceauth_fw_version = dma_read_value;

		if (copy_to_user((void __user *)arg, &continue_step_data,
				 sizeof(continue_step_data)))
			err = -EFAULT;
		goto exit;
		break;
	case FACEAUTH_DEV_IOC_CLEANUP:
		/* TODO cleanup Airbrush DRAM */
		pr_info("faceauth cleanup IOCTL\n");
		break;
	case FACEAUTH_DEV_IOC_DEBUG:
		pr_info("faceauth debug IOCTL\n");
		if (copy_from_user(&debug_step_data, (const void __user *)arg,
				   sizeof(debug_step_data))) {
			err = -EFAULT;
			goto exit;
		}
		err = dma_gather_debug(&debug_step_data);
		break;
	default:
		err = -EFAULT;
		goto exit;
	}

exit:
	pr_info("Faceauth action took %dus\n",
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
	memset((void *)&dma_desc, 0, sizeof(dma_desc));
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
	memset((void *)&dma_desc, 0, sizeof(dma_desc));
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

	err = dma_xfer_vmalloc((void *)(fw_entry->data), fw_entry->size,
			       remote_addr, DMA_TO_DEVICE);
	if (err)
		pr_err("Error from abc_pcie_issue_dma_xfer: %d\n", err);
	release_firmware(fw_entry);
	return err;
}

/**
 * Local function to write one DW to Airbrush memory via PCIE
 * @param[in] file File struct of this module
 * @param[in] remote_addr Address of Airbrush memory
 * @param[in] val DW value to write
 * @return Status, zero if succeed, non-zero if fail
 */
__attribute__((unused))
static int dma_write_dw(struct file *file, const int remote_addr, const int val)
{
	int err = 0;
	struct faceauth_data *data = file->private_data;

	data->dma_dw_buf = val;
	err = dma_xfer_vmalloc((void *)&(data->dma_dw_buf), sizeof(val),
			       remote_addr, DMA_TO_DEVICE);
	if (err)
		pr_err("Error from abc_pcie_issue_dma_xfer: %d\n", err);
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

	err = dma_xfer_vmalloc((void *)&(data->dma_dw_buf), sizeof(*val),
			       remote_addr, DMA_FROM_DEVICE);
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

/**
 * Local function to send all FaceAuth firmwares to Airbrush memory via PCIE
 * @return Status, zero if succeed, non-zero if fail
 */
static int dma_send_workloads(void)
{
	int err = 0;

	/* Send IPU workload */
	pr_info("Set JQS Depth addr = 0x%08x\n", JQS_DEPTH_ADDR);
	err = dma_send_fw(JQS_DEPTH_PATH, JQS_DEPTH_ADDR);
	if (err) {
		pr_err("Error during JQS binary transfer: %d\n", err);
		return err;
	}

	pr_info("Set JQS Affine16 addr = 0x%08x\n", JQS_AFFINE_16_ADDR);
	err = dma_send_fw(JQS_AFFINE_16_PATH, JQS_AFFINE_16_ADDR);
	if (err) {
		pr_err("Error during JQS binary transfer: %d\n", err);
		return err;
	}

	pr_info("Set JQS Affine RGB addr = 0x%08x\n", JQS_AFFINE_RGB_ADDR);
	err = dma_send_fw(JQS_AFFINE_RGB_PATH, JQS_AFFINE_RGB_ADDR);
	if (err) {
		pr_err("Error during JQS binary transfer: %d\n", err);
		return err;
	}

	pr_info("Set JQS Affine8 addr = 0x%08x\n", JQS_AFFINE_8_ADDR);
	err = dma_send_fw(JQS_AFFINE_8_PATH, JQS_AFFINE_8_ADDR);
	if (err) {
		pr_err("Error during JQS binary transfer: %d\n", err);
		return err;
	}

	/* Send M0 firmware */
	pr_info("Send M0 firmware to addr 0x%08x\n", M0_FIRMWARE_ADDR);
	err = dma_send_fw(M0_FIRMWARE_PATH, M0_FIRMWARE_ADDR);
	if (err) {
		pr_err("Error during M0 firmware transfer: %d\n", err);
		return err;
	}

	return err;
}

static int dma_gather_debug(struct faceauth_debug_data *data)
{
	int err = 0;

	err = dma_xfer((void *)data->print_buffer,
		       min((uint32_t)DEBUG_PRINT_SIZE, data->print_buffer_size),
		       DEBUG_PRINT_ADDR, DMA_FROM_DEVICE);

	return err;
}

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
