/*
 * FaceAuth firmware address definition
 *
 * Copyright (C) 2019 Google, Inc.
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

#ifndef __FACEAUTH_ADDRESSES_H__
#define __FACEAUTH_ADDRESSES_H__

/* input image sizes */
#define INPUT_IMAGE_WIDTH 480
#define INPUT_IMAGE_HEIGHT 640
#define INPUT_IMAGE_SIZE (INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT)

/*
 * Registers accessible through BAR0
 * task input/output addresses
 */
#ifndef CONFIG_FACEAUTH
#define RESULT_FLAG_ADDR SYSREG_AON_IPU_REG0
#define ANGLE_RESULT_FLAG_ADDR SYSREG_AON_IPU_REG1
#define INPUT_FLAG_ADDR SYSREG_AON_IPU_REG2
#define INPUT_COMMAND_ADDR SYSREG_AON_IPU_REG3
#define INPUT_COUNTER_ADDR SYSREG_AON_IPU_REG4
#define ACK_TO_HOST_ADDR SYSREG_AON_IPU_REG5
#else
#define SYSREG_AON 0x30000
#define AON_REG(reg) (SYSREG_AON + reg)
#define SYSREG_REG_GP_INT0 AON_REG(0x37C)
#define RESULT_FLAG_ADDR AON_REG(0x3C4)
#define ANGLE_RESULT_FLAG_ADDR (RESULT_FLAG_ADDR + 0x4)
#define INPUT_FLAG_ADDR (ANGLE_RESULT_FLAG_ADDR + 0x4)
#define INPUT_COMMAND_ADDR (INPUT_FLAG_ADDR + 0x4)
#define INPUT_COUNTER_ADDR (INPUT_COMMAND_ADDR + 0x4)
#define ACK_TO_HOST_ADDR (INPUT_COUNTER_ADDR + 0x4)
/* used registers 0x3C4 -> 0x3D8 */
#define SYSREG_AON_IPU_REG29 AON_REG(0x438)
#endif

/* AB DRAM Addresses */
/* FW Binary 0x20000000, ~32MB for FW */
#define DYNAMIC_VERBOSITY_RAM_ADDR 0x21fffff0
#define DISABLE_FEATURES_ADDR 0x21fffff8

/* input image addresses */
/* 0x22000000 -> 0x2212C000 input images */
#define DOT_LEFT_IMAGE_ADDR 0x22000000
#define DOT_RIGHT_IMAGE_ADDR (DOT_LEFT_IMAGE_ADDR + INPUT_IMAGE_SIZE)
#define FLOOD_IMAGE_ADDR (DOT_RIGHT_IMAGE_ADDR + INPUT_IMAGE_SIZE)
#define RIGHT_FLOOD_IMAGE_ADDR (FLOOD_IMAGE_ADDR + INPUT_IMAGE_SIZE)

/* 0x2212C000 -> 0x2212C400 Calibration
 */
#define CALIBRATION_DATA_ADDR (RIGHT_FLOOD_IMAGE_ADDR + INPUT_IMAGE_SIZE)
#define INPUT_ADDR_END (CALIBRATION_DATA_ADDR + 0x400)

/* 0x2212C400 -> 0x2214C400 Embedding database
*/
#define FACE_EMBEDDING_DATABASE_ADDR INPUT_ADDR_END
#define FACE_EMBEDDING_DATABASE_SIZE (256 * 512)


/* 0x2214C400 -> 0x2214C500 Cache Flush indexes
*/
#define CACHE_FLUSH_INDEXES_ADDR                                               \
	(FACE_EMBEDDING_DATABASE_ADDR + FACE_EMBEDDING_DATABASE_SIZE)
#define CACHE_FLUSH_ADDR_END (FACE_EMBEDDING_DATABASE_ADDR + 0x100)

/* 0x2214C500 -> 0x2224C500 Logs */
#define PRINTF_LOG_ADDR CACHE_FLUSH_ADDR_END
#define PRINTF_LOG_SIZE 0x00100000
#define PRINTF_LOG_ADDR_END ((PRINTF_LOG_ADDR) + (PRINTF_LOG_SIZE))

/* 0x2224C500 -> 0x2224D500 Internal State */
#define INTERNAL_STATE_ADDR PRINTF_LOG_ADDR_END
#define INTERNAL_STATE_SIZE 0x000001000

#define END_PUBLIC_MEMORY_ADDR                                                 \
	(INTERNAL_STATE_ADDR + INTERNAL_STATE_SIZE)

#endif /* __FACEAUTH_ADDRESSES_H__ */
