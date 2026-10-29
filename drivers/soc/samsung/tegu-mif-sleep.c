// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pixel 9a bring-up: lower the memory (MIF) rate for system sleep. s2idle
 * keeps the SoC's memory interface at whatever ACPM last set (2028 MHz
 * from boot), and at 421 MHz the palmtop drew 57 mA less asleep (371 vs
 * 314 mA, cable in, 2026-10-28). The previous rate comes back at resume.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/suspend.h>

static unsigned int sleep_khz = 421000;
module_param(sleep_khz, uint, 0644);

static struct clk *mif;
static unsigned long awake_hz;

static int tegu_mif_sleep_pm(struct notifier_block *nb, unsigned long action, void *v)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
		awake_hz = clk_get_rate(mif);
		if (sleep_khz && clk_set_rate(mif, sleep_khz * 1000UL))
			awake_hz = 0;
		break;
	case PM_POST_SUSPEND:
		if (awake_hz)
			clk_set_rate(mif, awake_hz);
		awake_hz = 0;
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block tegu_mif_sleep_nb = {
	.notifier_call = tegu_mif_sleep_pm,
};

static int __init tegu_mif_sleep_init(void)
{
	struct of_phandle_args args = { .args_count = 1, .args = { 0 } };

	args.np = of_find_compatible_node(NULL, NULL, "google,zumapro-acpm-ipc");
	if (!args.np)
		return -ENODEV;
	mif = of_clk_get_from_provider(&args);
	of_node_put(args.np);
	if (IS_ERR(mif))
		return PTR_ERR(mif);

	return register_pm_notifier(&tegu_mif_sleep_nb);
}

static void __exit tegu_mif_sleep_exit(void)
{
	unregister_pm_notifier(&tegu_mif_sleep_nb);
	clk_put(mif);
}

module_init(tegu_mif_sleep_init);
module_exit(tegu_mif_sleep_exit);
MODULE_DESCRIPTION("Pixel 9a bring-up: memory rate for system sleep");
MODULE_LICENSE("GPL");
