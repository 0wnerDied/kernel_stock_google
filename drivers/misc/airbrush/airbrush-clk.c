/*
 * Copyright (c) 2018 Samsung Electronics Co., Ltd.
 *
 * Author: Raman Kumar Banka <raman.k2@samsung.com>
 *
 * Clock controller for airbrush state manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/airbrush-sm-ctrl.h>
#include <linux/airbrush-sm-notifier.h>
#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/mfd/abc-pcie.h>

#include "airbrush-clk.h"

#define OSC_RATE 19200000

#define GAT_CLK_BLK_IPU_UID_IPU_IPCLKPORT_CLK_IPU	0x1024202c
#define GAT_CLK_BLK_TPU_UID_TPU_IPCLKPORT_CLK_TPU	0x10042034

static struct ab_sm_clk_ops clk_ops;
static int ab_clk_pcie_link_listener(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct ab_clk_context *clk_ctx = container_of(nb,
			struct ab_clk_context, pcie_link_blocking_nb);

	switch (action) {
	case ABC_PCIE_LINK_POST_ENABLE:
		mutex_lock(&clk_ctx->pcie_link_lock);
		clk_ctx->pcie_link_ready = true;
		mutex_unlock(&clk_ctx->pcie_link_lock);
		break;
	case ABC_PCIE_LINK_PRE_DISABLE:
		mutex_lock(&clk_ctx->pcie_link_lock);
		clk_ctx->pcie_link_ready = false;
		mutex_unlock(&clk_ctx->pcie_link_lock);
		break;
	default:
		return NOTIFY_DONE;  /* Don't care */
	}

	return NOTIFY_OK;
}

static int ab_clk_ipu_pll_enable_handler(void *ctx)
{
	int ret;

	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	dev_dbg(clk_ctx->dev, "%s: enable IPU PLL\n", __func__);

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		ret = clk_prepare_enable(clk_ctx->ipu_pll);
		if (ret < 0)
			dev_err(clk_ctx->dev,
				"Unable to prepare_enable ipu clk (err %d)\n",
				ret);

	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		ret = -ENODEV;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return ret;
}

static int ab_clk_ipu_pll_disable_handler(void *ctx)
{
	int ret = 0;
	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	dev_dbg(clk_ctx->dev, "%s: disable IPU PLL\n", __func__);

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		clk_disable_unprepare(clk_ctx->ipu_pll);
	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		ret = -ENODEV;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return ret;
}

/* Caller must hold clk_ctx->pcie_link_lock */
static void __ab_clk_ipu_gate_handler(struct ab_clk_context *clk_ctx)
{
	uint32_t val;
	unsigned long old_rate = clk_get_rate(clk_ctx->ipu_switch_mux);
	unsigned long new_rate = 0;
	ab_sm_clk_notify(AB_IPU_PRE_RATE_CHANGE, old_rate, new_rate);

	/* NOTE: This is guarded against PCIE going down
	 * since it is only called via ops structure. Ops structure is
	 * unregistered by mfd when link goes down
	 */
	ABC_READ(GAT_CLK_BLK_IPU_UID_IPU_IPCLKPORT_CLK_IPU, &val);
	val |= (1 << 20);
	val &= ~(1 << 21);
	ABC_WRITE(GAT_CLK_BLK_IPU_UID_IPU_IPCLKPORT_CLK_IPU, val);

	ab_sm_clk_notify(AB_IPU_POST_RATE_CHANGE, old_rate, new_rate);
}

static int ab_clk_ipu_gate_handler(void *ctx)
{
	int ret = 0;
	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	dev_dbg(clk_ctx->dev, "%s: gate IPU clock\n", __func__);

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		__ab_clk_ipu_gate_handler(clk_ctx);
	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		ret = -ENODEV;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return ret;
}

/* Caller must hold clk_ctx->pcie_link_lock */
static void __ab_clk_ipu_ungate_handler(struct ab_clk_context *clk_ctx)
{
	uint32_t val;
	unsigned long old_rate = 0;
	unsigned long new_rate = clk_get_rate(clk_ctx->ipu_switch_mux);

	ab_sm_clk_notify(AB_IPU_PRE_RATE_CHANGE, old_rate, new_rate);

	/* NOTE: This is guarded against PCIE going down
	 * since it is only called via ops structure. Ops structure is
	 * unregistered by mfd when link goes down
	 */
	ABC_READ(GAT_CLK_BLK_IPU_UID_IPU_IPCLKPORT_CLK_IPU, &val);
	val |= (1 << 20);
	val |= (1 << 21);
	ABC_WRITE(GAT_CLK_BLK_IPU_UID_IPU_IPCLKPORT_CLK_IPU, val);

	ab_sm_clk_notify(AB_IPU_POST_RATE_CHANGE, old_rate, new_rate);
}

static int ab_clk_ipu_ungate_handler(void *ctx)
{
	int ret = 0;
	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	dev_dbg(clk_ctx->dev, "%s: ungate IPU clock\n", __func__);

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		__ab_clk_ipu_ungate_handler(clk_ctx);
	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		ret = -ENODEV;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return ret;
}

/* Caller must hold clk_ctx->pcie_link_lock */
static u64 __ab_clk_ipu_set_rate_handler(struct ab_clk_context *clk_ctx,
		u64 rate)
{
	u64 old_rate = clk_get_rate(clk_ctx->ipu_switch_mux);
	u64 new_rate = rate;

	if (rate == 0) {
		rate = OSC_RATE;
		new_rate = rate;
	}

	dev_dbg(clk_ctx->dev,
		"%s: set IPU clock rate to %llu\n", __func__, rate);

	ab_sm_clk_notify(AB_IPU_PRE_RATE_CHANGE, old_rate, new_rate);

	if (rate == OSC_RATE) {
		clk_set_parent(clk_ctx->ipu_pll_mux, clk_ctx->osc_clk);
		clk_set_rate(clk_ctx->ipu_pll, OSC_RATE);
		clk_set_rate(clk_ctx->ipu_pll_div, OSC_RATE);
		clk_set_parent(clk_ctx->ipu_switch_mux,
				clk_ctx->ipu_pll_div);

		new_rate = clk_get_rate(clk_ctx->ipu_switch_mux);
		ab_sm_clk_notify(AB_IPU_POST_RATE_CHANGE,
				old_rate, new_rate);
		return new_rate;
	}

	clk_set_parent(clk_ctx->ipu_pll_mux, clk_ctx->ipu_pll);
	clk_set_parent(clk_ctx->ipu_switch_mux,
			clk_ctx->shared_div_aon_pll);
	clk_set_rate(clk_ctx->ipu_pll, rate);
	clk_set_rate(clk_ctx->ipu_pll_div, rate);
	clk_set_parent(clk_ctx->ipu_switch_mux, clk_ctx->ipu_pll_div);

	new_rate = clk_get_rate(clk_ctx->ipu_switch_mux);
	ab_sm_clk_notify(AB_IPU_POST_RATE_CHANGE, old_rate, new_rate);

	return new_rate;
}

static u64 ab_clk_ipu_set_rate_handler(void *ctx, u64 rate)
{
	u64 new_rate = rate;
	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		new_rate = __ab_clk_ipu_set_rate_handler(clk_ctx, new_rate);
	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		new_rate = 0;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return new_rate;
}

/* Caller must hold clk_ctx->pcie_link_lock */
static void __ab_clk_tpu_gate_handler(struct ab_clk_context *clk_ctx)
{
	uint32_t val;
	unsigned long old_rate = clk_get_rate(clk_ctx->tpu_switch_mux);
	unsigned long new_rate = 0;

	ab_sm_clk_notify(AB_TPU_PRE_RATE_CHANGE, old_rate, new_rate);

	/* NOTE: This is guarded against PCIE going down
	 * since it is only called via ops structure. Ops structure is
	 * unregistered by mfd when link goes down
	 */
	ABC_READ(GAT_CLK_BLK_TPU_UID_TPU_IPCLKPORT_CLK_TPU, &val);
	val |= (1 << 20);
	val &= ~(1 << 21);
	ABC_WRITE(GAT_CLK_BLK_TPU_UID_TPU_IPCLKPORT_CLK_TPU, val);

	ab_sm_clk_notify(AB_TPU_POST_RATE_CHANGE, old_rate, new_rate);
}

static int ab_clk_tpu_gate_handler(void *ctx)
{
	int ret = 0;
	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	dev_dbg(clk_ctx->dev, "%s: gate TPU clocks\n", __func__);

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		__ab_clk_tpu_gate_handler(clk_ctx);
	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		ret = -ENODEV;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return ret;
}

/* Caller must hold clk_ctx->pcie_link_lock */
static void __ab_clk_tpu_ungate_handler(struct ab_clk_context *clk_ctx)
{
	uint32_t val;
	unsigned long old_rate = 0;
	unsigned long new_rate = clk_get_rate(clk_ctx->tpu_switch_mux);

	ab_sm_clk_notify(AB_TPU_PRE_RATE_CHANGE, old_rate, new_rate);

	/* NOTE: This is guarded against PCIE going down
	 * since it is only called via ops structure. Ops structure is
	 * unregistered by mfd when link goes down
	 */
	ABC_READ(GAT_CLK_BLK_TPU_UID_TPU_IPCLKPORT_CLK_TPU, &val);
	val |= (1 << 20);
	val |= (1 << 21);
	ABC_WRITE(GAT_CLK_BLK_TPU_UID_TPU_IPCLKPORT_CLK_TPU, val);

	ab_sm_clk_notify(AB_TPU_POST_RATE_CHANGE, old_rate, new_rate);
}

static int ab_clk_tpu_ungate_handler(void *ctx)
{
	int ret = 0;
	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	dev_dbg(clk_ctx->dev, "%s: ungate TPU clocks\n", __func__);

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		__ab_clk_tpu_ungate_handler(clk_ctx);
	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		ret = -ENODEV;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return ret;
}

static int ab_clk_tpu_pll_enable_handler(void *ctx)
{
	int ret = 0;
	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	dev_dbg(clk_ctx->dev, "%s: enable TPU PLL\n", __func__);

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		ret = clk_prepare_enable(clk_ctx->tpu_pll);
		if (ret < 0)
			dev_err(clk_ctx->dev,
				"Unable to prepare_enable tpu clk (err %d)\n",
				ret);

	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		ret = -ENODEV;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return ret;
}

static int ab_clk_tpu_pll_disable_handler(void *ctx)
{
	int ret = 0;
	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	dev_dbg(clk_ctx->dev, "%s: disable TPU PLL\n", __func__);

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		clk_disable_unprepare(clk_ctx->tpu_pll);

	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		ret = -ENODEV;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return ret;
}

/* Caller must hold clk_ctx->pcie_link_lock */
static u64 __ab_clk_tpu_set_rate_handler(struct ab_clk_context *clk_ctx,
		u64 rate)
{
	u64 old_rate = clk_get_rate(clk_ctx->tpu_switch_mux);
	u64 new_rate = rate;

	if (rate == 0) {
		rate = OSC_RATE;
		new_rate = rate;
	}

	dev_dbg(clk_ctx->dev,
		"%s: set TPU clock rate to %llu\n", __func__, rate);

	ab_sm_clk_notify(AB_TPU_PRE_RATE_CHANGE, old_rate, new_rate);

	if (rate == OSC_RATE) {
		clk_set_parent(clk_ctx->tpu_pll_mux, clk_ctx->osc_clk);
		clk_set_rate(clk_ctx->tpu_pll, OSC_RATE);
		clk_set_rate(clk_ctx->tpu_pll_div, OSC_RATE);
		clk_set_parent(clk_ctx->tpu_switch_mux,
				clk_ctx->tpu_pll_div);

		new_rate = clk_get_rate(clk_ctx->tpu_switch_mux);
		ab_sm_clk_notify(AB_TPU_POST_RATE_CHANGE,
				old_rate, new_rate);
		return new_rate;
	}

	clk_set_parent(clk_ctx->tpu_pll_mux, clk_ctx->tpu_pll);
	clk_set_parent(clk_ctx->tpu_switch_mux,
			clk_ctx->shared_div_aon_pll);
	clk_set_rate(clk_ctx->tpu_pll, rate);
	clk_set_rate(clk_ctx->tpu_pll_div, rate);
	clk_set_parent(clk_ctx->tpu_switch_mux, clk_ctx->tpu_pll_div);

	new_rate = clk_get_rate(clk_ctx->tpu_switch_mux);
	ab_sm_clk_notify(AB_TPU_POST_RATE_CHANGE, old_rate, new_rate);

	return new_rate;
}

static u64 ab_clk_tpu_set_rate_handler(void *ctx, u64 rate)
{
	u64 new_rate;
	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		new_rate = __ab_clk_tpu_set_rate_handler(clk_ctx, rate);
	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		new_rate = 0;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return new_rate;
}

/* Caller must hold clk_ctx->pcie_link_lock */
static u64 __ab_clk_aon_set_rate_handler(struct ab_clk_context *clk_ctx,
		u64 rate)
{
	u64 ret = 0;

	if (rate == 0)
		rate = OSC_RATE;

	dev_dbg(clk_ctx->dev,
		"%s: set AON clock rate to %llu\n", __func__, rate);

	if (rate == OSC_RATE) {
		clk_set_parent(clk_ctx->aon_pll_mux, clk_ctx->osc_clk);
		clk_set_rate(clk_ctx->aon_pll, rate);
		return clk_get_rate(clk_ctx->aon_pll_mux);
	}

	clk_set_parent(clk_ctx->aon_pll_mux, clk_ctx->aon_pll);
	clk_set_rate(clk_ctx->aon_pll, rate);
	ret = clk_get_rate(clk_ctx->aon_pll_mux);

	return ret;
}

static u64 ab_clk_aon_set_rate_handler(void *ctx, u64 rate)
{
	u64 ret;
	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		ret = __ab_clk_aon_set_rate_handler(clk_ctx, rate);
	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		ret = 0;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return ret;
}

/* TODO(b/119189465): remove when clk framework method is available */
static int ab_clk_attach_mif_clk_ref_handler(void *ctx)
{
	int ret = 0;
	uint32_t val;
	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		ABC_READ(MIF_PLL_CONTROL0, &val);
		val &= ~(1 << 4);
		val &= ~(1 << 31);
		ABC_WRITE(MIF_PLL_CONTROL0, val);

	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		ret = -ENODEV;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return ret;
}

/* TODO(b/119189465): remove when clk framework method is available */
static int ab_clk_deattach_mif_clk_ref_handler(void *ctx)
{
	int ret = 0;
	uint32_t val;
	uint32_t timeout = MIF_PLL_TIMEOUT;
	struct ab_clk_context *clk_ctx = (struct ab_clk_context *)ctx;

	mutex_lock(&clk_ctx->pcie_link_lock);
	if (clk_ctx->pcie_link_ready) {
		ABC_READ(MIF_PLL_CONTROL0, &val);
		val |= (1 << 4);
		val |= (1 << 31);
		ABC_WRITE(MIF_PLL_CONTROL0, val);
		do {
			ABC_READ(MIF_PLL_CONTROL0, &val);
		} while (!(val & 0x20000000) && --timeout > 0);

		if (timeout == 0) {
			dev_err(clk_ctx->dev,
					"Timeout waiting for AIRBRUSH MIF PLL lock\n");
			ret = -E_STATUS_TIMEOUT;
		}

	} else {
		dev_err(clk_ctx->dev,
				"%s: pcie link down during clk request\n",
				__func__);
		ret = -ENODEV;
	}
	mutex_unlock(&clk_ctx->pcie_link_lock);

	return ret;
}

static struct ab_sm_clk_ops clk_ops = {
	.ipu_pll_enable = &ab_clk_ipu_pll_enable_handler,
	.ipu_pll_disable = &ab_clk_ipu_pll_disable_handler,
	.ipu_gate = &ab_clk_ipu_gate_handler,
	.ipu_ungate = &ab_clk_ipu_ungate_handler,
	.ipu_set_rate = &ab_clk_ipu_set_rate_handler,

	.tpu_pll_enable = &ab_clk_tpu_pll_enable_handler,
	.tpu_pll_disable = &ab_clk_tpu_pll_disable_handler,
	.tpu_gate = &ab_clk_tpu_gate_handler,
	.tpu_ungate = &ab_clk_tpu_ungate_handler,
	.tpu_set_rate = &ab_clk_tpu_set_rate_handler,

	.aon_set_rate = &ab_clk_aon_set_rate_handler,

	.attach_mif_clk_ref = &ab_clk_attach_mif_clk_ref_handler,
	.deattach_mif_clk_ref = &ab_clk_deattach_mif_clk_ref_handler,
};

static int ab_clk_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct device_node *child;
	struct device_node *ab_clk_nd;
	struct ab_clk_context *clk_ctx;

	clk_ctx = devm_kzalloc(dev, sizeof(struct ab_clk_context), GFP_KERNEL);
	if (!clk_ctx)
		return -ENOMEM;

	clk_ctx->dev = dev;

	ab_clk_nd = of_find_node_by_name(dev->of_node, "abc-clk");
	if (!ab_clk_nd)
		return -ENODEV;

	/* Registering CMUs to Common Clock Framework.
	 * Parse through the ab device node and scan for cmu nodes.
	 * once found, register the same with the common clock framework
	 */
	for_each_child_of_node(ab_clk_nd, child) {
		if (of_device_is_compatible(child,
					"diablo,abc-clock-aon")) {
			abc_clk_aon_init(child);
		} else if (of_device_is_compatible(child,
					"diablo,abc-clock-core")) {
			abc_clk_core_init(child);
		} else if (of_device_is_compatible(child,
					"diablo,abc-clock-fsys")) {
			abc_clk_fsys_init(child);
		} else if (of_device_is_compatible(child,
					"diablo,abc-clock-mif")) {
			abc_clk_mif_init(child);
		} else if (of_device_is_compatible(child,
					"diablo,abc-clock-ipu")) {
			abc_clk_ipu_init(child);
		} else if (of_device_is_compatible(child,
					"diablo,abc-clock-tpu")) {
			abc_clk_tpu_init(child);
		} else {
			dev_err(clk_ctx->dev,
				"incompatible child node (%s)\n", child->name);
		}
	}

	clk_ctx->ipu_pll =
		of_clk_get_by_name(ab_clk_nd, "ipu_pll");
	clk_ctx->ipu_pll_mux =
		of_clk_get_by_name(ab_clk_nd, "ipu_pll_mux");
	clk_ctx->ipu_pll_div =
		of_clk_get_by_name(ab_clk_nd, "ipu_pll_div");
	clk_ctx->ipu_switch_mux =
		of_clk_get_by_name(ab_clk_nd, "ipu_switch_mux");

	clk_ctx->tpu_pll =
		of_clk_get_by_name(ab_clk_nd, "tpu_pll");
	clk_ctx->tpu_pll_mux =
		of_clk_get_by_name(ab_clk_nd, "tpu_pll_mux");
	clk_ctx->tpu_pll_div =
		of_clk_get_by_name(ab_clk_nd, "tpu_pll_div");
	clk_ctx->tpu_switch_mux =
		of_clk_get_by_name(ab_clk_nd, "tpu_switch_mux");

	clk_ctx->osc_clk =
		of_clk_get_by_name(ab_clk_nd, "osc_clk");
	clk_ctx->shared_div_aon_pll =
		of_clk_get_by_name(ab_clk_nd, "shared_div_aon_pll");
	clk_ctx->aon_pll =
		of_clk_get_by_name(ab_clk_nd, "aon_pll");
	clk_ctx->aon_pll_mux =
		of_clk_get_by_name(ab_clk_nd, "aon_pll_mux");

	if (IS_ERR(clk_ctx->ipu_pll) ||
			IS_ERR(clk_ctx->ipu_pll_mux) ||
			IS_ERR(clk_ctx->ipu_pll_div) ||
			IS_ERR(clk_ctx->ipu_switch_mux) ||
			IS_ERR(clk_ctx->tpu_pll) ||
			IS_ERR(clk_ctx->tpu_pll_mux) ||
			IS_ERR(clk_ctx->tpu_pll_div) ||
			IS_ERR(clk_ctx->tpu_switch_mux) ||
			IS_ERR(clk_ctx->osc_clk) ||
			IS_ERR(clk_ctx->shared_div_aon_pll) ||
			IS_ERR(clk_ctx->aon_pll) ||
			IS_ERR(clk_ctx->aon_pll_mux)) {
		dev_err(clk_ctx->dev, "could not register all clocks\n");

		// TODO: unregister clocks?
		return -ENODEV;
	}

	platform_set_drvdata(pdev, clk_ctx);

	mutex_init(&clk_ctx->pcie_link_lock);
	clk_ctx->pcie_link_ready = true;
	clk_ctx->pcie_link_blocking_nb.notifier_call =
			ab_clk_pcie_link_listener;
	ret = abc_register_pcie_link_blocking_event(
			&clk_ctx->pcie_link_blocking_nb);
	if (ret) {
		dev_err(dev,
				"failed to subscribe to PCIe blocking link event, ret %d\n",
				ret);
		return ret;
	}

	clk_ops.ctx = clk_ctx;
	ab_sm_register_clk_ops(&clk_ops);

	return 0;
}

static int ab_clk_remove(struct platform_device *pdev)
{
	struct ab_clk_context *clk_ctx = platform_get_drvdata(pdev);

	ab_sm_unregister_clk_ops();

	// TODO: unregister clocks?
	if (clk_ctx->ipu_pll)
		clk_put(clk_ctx->ipu_pll);
	if (clk_ctx->ipu_pll_mux)
		clk_put(clk_ctx->ipu_pll_mux);
	if (clk_ctx->ipu_pll_div)
		clk_put(clk_ctx->ipu_pll_div);
	if (clk_ctx->ipu_switch_mux)
		clk_put(clk_ctx->ipu_switch_mux);

	if (clk_ctx->tpu_pll)
		clk_put(clk_ctx->tpu_pll);
	if (clk_ctx->tpu_pll_mux)
		clk_put(clk_ctx->tpu_pll_mux);
	if (clk_ctx->tpu_pll_div)
		clk_put(clk_ctx->tpu_pll_div);
	if (clk_ctx->tpu_switch_mux)
		clk_put(clk_ctx->tpu_switch_mux);

	if (clk_ctx->osc_clk)
		clk_put(clk_ctx->osc_clk);
	if (clk_ctx->shared_div_aon_pll)
		clk_put(clk_ctx->shared_div_aon_pll);
	if (clk_ctx->aon_pll)
		clk_put(clk_ctx->aon_pll);
	if (clk_ctx->aon_pll_mux)
		clk_put(clk_ctx->aon_pll_mux);

	return 0;
}

static const struct of_device_id ab_clk_of_match[] = {
	{ .compatible = "abc,airbrush-clk", },
	{ }
};

static struct platform_driver ab_clk_driver = {
	.probe = ab_clk_probe,
	.remove = ab_clk_remove,
	.driver = {
		.name = "ab-clk",
		.of_match_table = ab_clk_of_match,
	},
};
module_platform_driver(ab_clk_driver);
