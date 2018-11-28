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

#include <linux/airbrush-clk.h>
#include <linux/airbrush-sm-ctrl.h>
#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/mfd/abc-pcie.h>

#define OSC_RATE 19200000

#define GAT_CLK_BLK_IPU_UID_IPU_IPCLKPORT_CLK_IPU	0x1024202c
#define GAT_CLK_BLK_TPU_UID_TPU_IPCLKPORT_CLK_TPU	0x10042034

int ipu_pll_enable(struct ab_state_context *sc)
{
	if (!atomic_read(&sc->clocks_registered)) {
		dev_err(sc->dev, "clocks not registered\n");
		return -ENODEV;
	}

	dev_dbg(sc->dev, "%s: enable IPU PLL\n", __func__);
	return clk_prepare_enable(sc->ipu_pll);
}

void ipu_pll_disable(struct ab_state_context *sc)
{
	if (!atomic_read(&sc->clocks_registered)) {
		dev_err(sc->dev, "clocks not registered\n");
		return;
	}

	dev_dbg(sc->dev, "%s: disable IPU PLL\n", __func__);
	clk_disable_unprepare(sc->ipu_pll);
}

// TODO: Pull this out to clock driver
int ipu_gate(void *ctx)
{
	struct ab_state_context *sc = (struct ab_state_context *)ctx;
	uint32_t val;

	dev_dbg(sc->dev, "%s: gate IPU clock\n", __func__);

	// TODO: Guard against pcie link going down
	ABC_READ(GAT_CLK_BLK_IPU_UID_IPU_IPCLKPORT_CLK_IPU, &val);
	val |= (1 << 20);
	val &= ~(1 << 21);
	ABC_WRITE(GAT_CLK_BLK_IPU_UID_IPU_IPCLKPORT_CLK_IPU, val);

	return 0;
}

// TODO: Pull this out to clock driver
int ipu_ungate(void *ctx)
{
	struct ab_state_context *sc = (struct ab_state_context *)ctx;
	uint32_t val;

	dev_dbg(sc->dev, "%s: ungate IPU clock\n", __func__);

	// TODO: Guard against pcie link going down
	ABC_READ(GAT_CLK_BLK_IPU_UID_IPU_IPCLKPORT_CLK_IPU, &val);
	val |= (1 << 20);
	val |= (1 << 21);
	ABC_WRITE(GAT_CLK_BLK_IPU_UID_IPU_IPCLKPORT_CLK_IPU, val);

	return 0;
}

u64 ipu_set_rate(struct ab_state_context *sc, u64 rate)
{
	if (!atomic_read(&sc->clocks_registered)) {
		dev_err(sc->dev, "clocks not registered\n");
		return 0;
	}

	if (rate == 0)
		rate = OSC_RATE;

	dev_dbg(sc->dev, "%s: set IPU clock rate to %llu\n", __func__, rate);


	if (rate == OSC_RATE) {
		clk_set_parent(sc->ipu_pll_mux, sc->osc_clk);
		clk_set_rate(sc->ipu_pll_div, OSC_RATE);
		clk_set_parent(sc->ipu_switch_mux, sc->ipu_pll_div);
		return clk_get_rate(sc->ipu_switch_mux);
	}

	clk_set_parent(sc->ipu_pll_mux, sc->ipu_pll);
	clk_set_parent(sc->ipu_switch_mux, sc->shared_div_aon_pll);
	clk_set_rate(sc->ipu_pll, rate);
	clk_set_rate(sc->ipu_pll_div, rate);
	clk_set_parent(sc->ipu_switch_mux, sc->ipu_pll_div);

	return clk_get_rate(sc->ipu_switch_mux);
}

// TODO: Pull this out to clock driver
int tpu_gate(void *ctx)
{
	struct ab_state_context *sc = (struct ab_state_context *)ctx;
	uint32_t val;

	dev_dbg(sc->dev, "%s: gate TPU clocks\n", __func__);

	// TODO: Guard against pcie link going down
	ABC_READ(GAT_CLK_BLK_TPU_UID_TPU_IPCLKPORT_CLK_TPU, &val);
	val |= (1 << 20);
	val &= ~(1 << 21);
	ABC_WRITE(GAT_CLK_BLK_TPU_UID_TPU_IPCLKPORT_CLK_TPU, val);

	return 0;
}

// TODO: Pull this out to clock driver
int tpu_ungate(void *ctx)
{
	struct ab_state_context *sc = (struct ab_state_context *)ctx;
	uint32_t val;

	dev_dbg(sc->dev, "%s: ungate TPU clocks\n", __func__);

	// TODO: Guard against pcie link going down
	ABC_READ(GAT_CLK_BLK_TPU_UID_TPU_IPCLKPORT_CLK_TPU, &val);
	val |= (1 << 20);
	val |= (1 << 21);
	ABC_WRITE(GAT_CLK_BLK_TPU_UID_TPU_IPCLKPORT_CLK_TPU, val);

	return 0;
}

int tpu_pll_enable(struct ab_state_context *sc)
{
	if (!atomic_read(&sc->clocks_registered)) {
		dev_err(sc->dev, "clocks not registered\n");
		return -ENODEV;
	}

	dev_dbg(sc->dev, "%s: enable TPU PLL\n", __func__);

	return clk_prepare_enable(sc->tpu_pll);
}

void tpu_pll_disable(struct ab_state_context *sc)
{
	if (!atomic_read(&sc->clocks_registered)) {
		dev_err(sc->dev, "clocks not registered\n");
		return;
	}

	dev_dbg(sc->dev, "%s: disable TPU PLL\n", __func__);
	clk_disable_unprepare(sc->tpu_pll);
}

u64 tpu_set_rate(struct ab_state_context *sc, u64 rate)
{
	if (!atomic_read(&sc->clocks_registered)) {
		dev_err(sc->dev, "clocks not registered\n");
		return 0;
	}

	if (rate == 0)
		rate = OSC_RATE;

	dev_dbg(sc->dev, "%s: set TPU clock rate to %llu\n", __func__, rate);

	if (rate == OSC_RATE) {
		clk_set_parent(sc->tpu_pll_mux, sc->osc_clk);
		clk_set_rate(sc->tpu_pll_div, OSC_RATE);
		clk_set_parent(sc->tpu_switch_mux, sc->tpu_pll_div);
		return clk_get_rate(sc->tpu_switch_mux);
	}

	clk_set_parent(sc->tpu_pll_mux, sc->tpu_pll);
	clk_set_parent(sc->tpu_switch_mux, sc->shared_div_aon_pll);
	clk_set_rate(sc->tpu_pll, rate);
	clk_set_rate(sc->tpu_pll_div, rate);
	clk_set_parent(sc->tpu_switch_mux, sc->tpu_pll_div);

	return clk_get_rate(sc->tpu_switch_mux);
}

u64 aon_set_rate(struct ab_state_context *sc, u64 rate)
{
	if (!atomic_read(&sc->clocks_registered)) {
		dev_err(sc->dev, "clocks not registered\n");
		return 0;
	}

	if (rate == 0)
		rate = OSC_RATE;

	dev_dbg(sc->dev, "%s: set AON clock rate to %llu\n", __func__, rate);

	if (rate == OSC_RATE) {
		clk_set_parent(sc->aon_pll_mux, sc->osc_clk);
		return clk_get_rate(sc->aon_pll_mux);
	}

	clk_set_parent(sc->aon_pll_mux, sc->aon_pll);
	clk_set_rate(sc->aon_pll, rate);

	return clk_get_rate(sc->aon_pll_mux);
}

/* TODO: Pull this out to clock driver */
/* TODO(b/119189465): remove when clk framework method is available */
int attach_mif_clk_ref(void *ctx)
{
	uint32_t val;
	ABC_READ(MIF_PLL_CONTROL0, &val);
	val &= ~(1 << 4);
	val &= ~(1 << 31);
	ABC_WRITE(MIF_PLL_CONTROL0, val);
	return 0;
}

/* TODO: Pull this out to clock driver */
/* TODO(b/119189465): remove when clk framework method is available */
int deattach_mif_clk_ref(void *ctx)
{
	uint32_t val;
	uint32_t timeout = MIF_PLL_TIMEOUT;
	ABC_READ(MIF_PLL_CONTROL0, &val);
	val |= (1 << 4);
	val |= (1 << 31);
	ABC_WRITE(MIF_PLL_CONTROL0, val);
	do {
		ABC_READ(MIF_PLL_CONTROL0, &val);
	} while (!(val & 0x20000000) && --timeout > 0);

	if (timeout == 0) {
		pr_err("Timeout waiting for AIRBRUSH MIF PLL lock\n");
		return -E_STATUS_TIMEOUT;
	}

	return 0;
}

/* TODO: Pull this out to clk driver.
 * To be called during probe/pci link up? */
void abc_clk_register(struct ab_state_context *sc)
{
	struct platform_device *pdev = sc->pdev;
	struct device_node *child = NULL;

	/* TODO(Lassen):  There are a lot of initialization state checks in the
	 * Airbrush State Manager.  These should not be necessary and the code
	 * should initialize the resources in the right order for all entry
	 * points into the code.
	 */

	if (!atomic_cmpxchg(&sc->clocks_registered, 0, 1)) {
		/* Registering CMUs to Common Clock Framework.
		 * Parse through the ab device node and scan for cmu nodes.
		 * once found, register the same with the common clock framework
		 */
		for_each_child_of_node(pdev->dev.of_node, child) {
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
			}
		}

		sc->ipu_pll			= clk_get(sc->dev, "ipu_pll");
		sc->ipu_pll_mux		= clk_get(sc->dev, "ipu_pll_mux");
		sc->ipu_pll_div		= clk_get(sc->dev, "ipu_pll_div");
		sc->ipu_switch_mux	= clk_get(sc->dev, "ipu_switch_mux");

		sc->tpu_pll			= clk_get(sc->dev, "tpu_pll");
		sc->tpu_pll_mux		= clk_get(sc->dev, "tpu_pll_mux");
		sc->tpu_pll_div		= clk_get(sc->dev, "tpu_pll_div");
		sc->tpu_switch_mux	= clk_get(sc->dev, "tpu_switch_mux");

		sc->osc_clk			= clk_get(sc->dev, "osc_clk");
		sc->shared_div_aon_pll = clk_get(sc->dev, "shared_div_aon_pll");
		sc->aon_pll			= clk_get(sc->dev, "aon_pll");
		sc->aon_pll_mux		= clk_get(sc->dev, "aon_pll_mux");

		if (IS_ERR(sc->ipu_pll) ||
				IS_ERR(sc->ipu_pll_mux) ||
				IS_ERR(sc->ipu_pll_div) ||
				IS_ERR(sc->ipu_switch_mux) ||
				IS_ERR(sc->tpu_pll) ||
				IS_ERR(sc->tpu_pll_mux) ||
				IS_ERR(sc->tpu_pll_div) ||
				IS_ERR(sc->tpu_switch_mux) ||
				IS_ERR(sc->osc_clk) ||
				IS_ERR(sc->shared_div_aon_pll) ||
				IS_ERR(sc->aon_pll) ||
				IS_ERR(sc->aon_pll_mux)) {
			dev_err(sc->dev, "could not register all clocks\n");
			atomic_set(&sc->clocks_registered, 0);
		}
	}
}
