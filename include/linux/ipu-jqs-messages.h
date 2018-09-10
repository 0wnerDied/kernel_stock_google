/*
 * Jqs message definitions for the Paintbox programmable IPU
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

#ifndef __PAINTBOX_JQS_MESSAGES_H__
#define __PAINTBOX_JQS_MESSAGES_H__

#include <linux/types.h>

enum jqs_message_type {
	/* Jqs <-> Host messages */

	/* Jqs <- Host messages  (all host -> jqs messages currently get ack'd)
	 * maybe not all are necessary? (log_info, for example)
	 */
	JQS_MESSAGE_TYPE_OPEN_SESSION       = 0x80001001,
	/* struct jqs_message_close_session -> struct jqs_message_ack */
	JQS_MESSAGE_TYPE_CLOSE_SESSION      = 0x80001002,
	/* struct jqs_message_alloc_queue -> struct jqs_message_ack */
	JQS_MESSAGE_TYPE_ALLOC_QUEUE        = 0x80001003,
	/* struct jqs_message_free_queue -> struct jqs_message_ack */
	JQS_MESSAGE_TYPE_FREE_QUEUE         = 0x80001004,
	/* struct jqs_message_register_buffer -> struct jqs_message_ack */
	JQS_MESSAGE_TYPE_REGISTER_BUFFER    = 0x80001005,
	/* struct jqs_message_unregister_buffer -> struct jqs_message_ack */
	JQS_MESSAGE_TYPE_UNREGISTER_BUFFER  = 0x80001006,
	/* struct jqs_message_alloc_resources -> struct jqs_message_ack */
	JQS_MESSAGE_TYPE_ALLOC_RESOURCES    = 0x80001007,
	/* struct jqs_message_release_resources -> struct jqs_message_ack */
	JQS_MESSAGE_TYPE_RELEASE_RESOURCES  = 0x80001008,
	/* struct jqs_message_enter_replay_mode -> n/a */
	JQS_MESSAGE_TYPE_ENTER_REPLAY_MODE  = 0x80001009,
	/* struct jqs_message_clock_rate -> n/a */
	JQS_MESSAGE_TYPE_CLOCK_RATE         = 0x8000100a,
	/* struct jqs_message_set_log_info -> n/a */
	JQS_MESSAGE_TYPE_SET_LOG_INFO       = 0x8000100b,

	/* Jqs -> Host messages */
	JQS_MESSAGE_TYPE_ACK                = 0x80002001,
	JQS_MESSAGE_TYPE_LOG                = 0x80002002,

	JQS_MESSAGE_TYPE_FORCE_32_BIT       = 0xFFFFFFFF,
};

enum jqs_log_level {
	JQS_LOG_LEVEL_NONE,
	JQS_LOG_LEVEL_INFO,
	JQS_LOG_LEVEL_WARNING,
	JQS_LOG_LEVEL_ERROR,
	JQS_LOG_LEVEL_FATAL
};

#define JQS_LOG_SINK_NONE    (0x0)
#define JQS_LOG_SINK_UART    (0x1 << 0)
#define JQS_LOG_SINK_MESSAGE (0x1 << 1)
#define JQS_LOG_SINK_MEMORY  (0x1 << 2)

#define MAX_LOG_SIZE 256

#define INIT_JQS_MSG(msg, t) \
	msg.header.type = t; \
	msg.header.size = sizeof(msg)

struct jqs_message {
	uint32_t size;
	enum jqs_message_type type;
};

/* host -> jqs */

struct jqs_message_entry_replay_mode {
	struct jqs_message header;
};

struct jqs_message_open_session {
	struct jqs_message header;
	uint32_t session_id;
	uint32_t session_memory_addr;
	uint32_t session_memory_bytes;
};

struct jqs_message_close_session {
	struct jqs_message header;
	uint32_t session_id;
};

struct jqs_message_alloc_queue {
	struct jqs_message header;
	uint32_t session_id;
	uint32_t q_id;
};

struct jqs_message_free_queue {
	struct jqs_message header;
	uint32_t session_id;
	uint32_t q_id;
};

struct jqs_message_register_buffer {
	struct jqs_message header;
	uint32_t session_id;
	uint32_t buffer_id;
	uint64_t buffer_addr;
	uint32_t buffer_size;
};

struct jqs_message_unregister_buffer {
	struct jqs_message header;
	uint32_t session_id;
	uint32_t buffer_id;
};

struct jqs_message_alloc_resources {
	struct jqs_message header;
	uint32_t session_id;
	uint32_t stp_id_mask;
	uint32_t lbp_id_mask;
	uint32_t dma_channel_id_mask;
};

struct jqs_message_release_resources {
	struct jqs_message header;
	uint32_t session_id;
	uint32_t stp_id_mask;
	uint32_t lbp_id_mask;
	uint32_t dma_channel_id_mask;
};

struct jqs_message_set_log_info {
	struct jqs_message header;
	enum jqs_log_level log_level;
	enum jqs_log_level interrupt_level; /* for kernel messages only */
	uint32_t log_sinks;
	uint32_t uart_baud_rate;
};

struct jqs_message_clock_rate {
	struct jqs_message header;
	uint32_t clock_rate;
};

/* Jqs -> Host */

enum jqs_error {
	JQS_ERROR_NONE,
	JQS_ERROR_BUSY,
};

struct jqs_message_ack {
	struct jqs_message header;
	enum jqs_message_type msg_type;
	enum jqs_error error;
};

struct jqs_message_log {
	struct jqs_message header;
	enum jqs_log_level log_level;
	uint32_t data_length;
	char data[MAX_LOG_SIZE];
};

#endif /* __PAINTBOX_JQS_MESSAGES_H__ */
