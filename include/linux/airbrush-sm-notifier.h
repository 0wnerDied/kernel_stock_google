/*
 * Airbrush Driver Notifier Interface
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

#ifndef _AIRBRUSH_SM_NOTIFIER_H
#define _AIRBRUSH_SM_NOTIFIER_H

#include <linux/bitops.h>
#include <linux/notifier.h>

/* Notifier types for Airbrush clock rate changes */
#define AB_IPU_PRE_RATE_CHANGE		BIT(0)
#define AB_IPU_POST_RATE_CHANGE		BIT(1)
#define AB_IPU_ABORT_RATE_CHANGE	BIT(2)
#define AB_TPU_PRE_RATE_CHANGE		BIT(3)
#define AB_TPU_POST_RATE_CHANGE		BIT(4)
#define AB_TPU_ABORT_RATE_CHANGE	BIT(5)

/**
 * struct ab_clk_notifier_data - rate data to pass to the notifier callback
 * @old_rate: old rate of this clk
 * @new_rate: new rate of this clk
 */
struct ab_clk_notifier_data {
	unsigned long		old_rate;
	unsigned long		new_rate;
};

int ab_sm_register_clk_event(struct notifier_block *nb);
int ab_sm_unregister_clk_event(struct notifier_block *nb);

/* Provider functions */
int ab_sm_clk_notify(unsigned long event,
		     unsigned long old_rate,
		     unsigned long new_rate);

#endif /* _AIRBRUSH_SM_NOTIFIER_H */
