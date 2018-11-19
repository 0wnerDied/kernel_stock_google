
/*
 * iaxxx-module.h  --  IAXXX module header file
 *
 * Copyright 2018 Knowles, Inc.
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#ifndef __IAXXX_MODULE_H__
#define __IAXXX_MODULE_H__

#define MAX_FILE_CHAR_SIZE	256

struct iaxxx_sensor_info {
	uint32_t block_id;
	uint32_t inst_id;
};

struct iaxxx_sensor_param {
	uint32_t inst_id;
	uint32_t param_id;
	uint32_t param_val;
	uint8_t block_id;
};

struct iaxxx_script_info {
	char script_name[MAX_FILE_CHAR_SIZE];
	uint32_t script_id;
};

/* IOCTL Magic character */
#define IAXXX_IOCTL_MAGIC 'I'

/* Create IOCTL */
#define MODULE_SENSOR_ENABLE _IO(IAXXX_IOCTL_MAGIC, 0x51)
#define MODULE_SENSOR_DISABLE _IO(IAXXX_IOCTL_MAGIC, 0x52)
#define MODULE_SENSOR_SET_PARAM _IO(IAXXX_IOCTL_MAGIC, 0x53)
#define MODULE_SENSOR_GET_PARAM _IO(IAXXX_IOCTL_MAGIC, 0x54)

#define SCRIPT_LOAD _IO(IAXXX_IOCTL_MAGIC, 0x61)
#define SCRIPT_UNLOAD _IO(IAXXX_IOCTL_MAGIC, 0x62)
#define SCRIPT_TRIGGER _IO(IAXXX_IOCTL_MAGIC, 0x63)
#endif /* __IAXXX_MODULE_H__ */
