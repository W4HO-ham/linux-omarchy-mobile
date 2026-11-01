// SPDX-License-Identifier: GPL-2.0
/*
 * Bring-up only (Pixel 9a SLEEP-PLAN S1): keep one SoC watchdog counting
 * through s2idle, so a sleep that hangs ends in a watchdog reset (which keeps
 * ramoops) instead of needing Power + Volume Up (which kept nothing).
 *
 * s3c2410_wdt stops its watchdogs in its suspend callback. This module's
 * device runs in the noirq phase, after every device's normal suspend, and
 * starts the chosen watchdog again with `timeout` seconds and the PMU reset
 * path unmasked, as the driver's own enable does; its resume_noirq stops it
 * before the driver's resume restores the saved state.
 *
 * Built against the exact tree the palmtop runs: struct s3c2410_wdt and the
 * variant below are copied from drivers/watchdog/s3c2410_wdt.c of that tree.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/watchdog.h>
#include <linux/clk.h>

struct s3c2410_wdt_variant {
	int disable_reg;
	int mask_reset_reg;
	bool mask_reset_inv;
	int mask_bit;
	int rst_stat_reg;
	int rst_stat_bit;
	int cnt_en_reg;
	int cnt_en_bit;
	u32 quirks;
};

struct s3c2410_wdt {
	struct device		*dev;
	struct clk		*bus_clk;
	struct clk		*src_clk;
	void __iomem		*reg_base;
	unsigned int		count;
	spinlock_t		lock;
	unsigned long		wtcon_save;
	unsigned long		wtdat_save;
	struct watchdog_device	wdt_device;
	struct notifier_block	freq_transition;
	const struct s3c2410_wdt_variant *drv_data;
	struct regmap *pmureg;
	u32 max_cnt;
};

#define QUIRK_HAS_PMU_MASK_RESET	BIT(1)	/* as in s3c2410_wdt.c */
#define QUIRK_HAS_PMU_CNT_EN		BIT(4)

static char *wdt_name = "10060000.watchdog";
module_param_named(dev, wdt_name, charp, 0444);
static unsigned int timeout = 60;
module_param(timeout, uint, 0644);
static bool enable = true;
module_param(enable, bool, 0644);
static unsigned int armed_count;
module_param(armed_count, uint, 0444);

static struct s3c2410_wdt *wdt;
static unsigned int saved_timeout;
static bool armed;

static int wdt_s2idle_suspend_noirq(struct device *dev)
{
	struct watchdog_device *wdd;
	const struct s3c2410_wdt_variant *v;
	int ret;

	if (!enable || !wdt)
		return 0;
	wdd = &wdt->wdt_device;
	v = wdt->drv_data;
	saved_timeout = wdd->timeout;
	ret = wdd->ops->set_timeout(wdd, min(timeout, wdd->max_timeout));
	if (ret)
		return 0;
	if (v->quirks & QUIRK_HAS_PMU_MASK_RESET)
		regmap_update_bits(wdt->pmureg, v->mask_reset_reg, BIT(v->mask_bit),
				   v->mask_reset_inv ? BIT(v->mask_bit) : 0);
	if (v->quirks & QUIRK_HAS_PMU_CNT_EN)
		regmap_update_bits(wdt->pmureg, v->cnt_en_reg, BIT(v->cnt_en_bit),
				   BIT(v->cnt_en_bit));
	wdd->ops->start(wdd);
	armed = true;
	armed_count++;
	pr_info("tegu-wdt-s2idle: %s armed for %u s through the sleep\n",
		wdt_name, min(timeout, wdd->max_timeout));
	return 0;
}

static int wdt_s2idle_resume_noirq(struct device *dev)
{
	struct watchdog_device *wdd;

	if (!armed)
		return 0;
	wdd = &wdt->wdt_device;
	wdd->ops->stop(wdd);
	wdd->ops->set_timeout(wdd, saved_timeout);
	armed = false;
	pr_info("tegu-wdt-s2idle: %s stopped after the sleep\n", wdt_name);
	return 0;
}

static const struct dev_pm_ops wdt_s2idle_pm = {
	.suspend_noirq = wdt_s2idle_suspend_noirq,
	.resume_noirq = wdt_s2idle_resume_noirq,
};

static struct platform_driver wdt_s2idle_driver = {
	.driver = { .name = "tegu-wdt-s2idle", .pm = &wdt_s2idle_pm },
};

static struct platform_device *pdev;

static int __init wdt_s2idle_init(void)
{
	struct device *d;
	int ret;

	d = bus_find_device_by_name(&platform_bus_type, NULL, wdt_name);
	if (!d)
		return -ENODEV;
	wdt = dev_get_drvdata(d);
	put_device(d);
	if (!wdt || wdt->dev != d || !wdt->wdt_device.ops || !wdt->pmureg) {
		pr_err("tegu-wdt-s2idle: %s does not look like s3c2410_wdt's data\n", wdt_name);
		return -EINVAL;
	}
	pr_info("tegu-wdt-s2idle: %s, max timeout %u s, current %u s\n", wdt_name,
		wdt->wdt_device.max_timeout, wdt->wdt_device.timeout);
	ret = platform_driver_register(&wdt_s2idle_driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("tegu-wdt-s2idle", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&wdt_s2idle_driver);
		return PTR_ERR(pdev);
	}
	return 0;
}

static void __exit wdt_s2idle_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&wdt_s2idle_driver);
}

module_init(wdt_s2idle_init);
module_exit(wdt_s2idle_exit);
MODULE_DESCRIPTION("Bring-up: keep an s3c2410 watchdog running through s2idle");
MODULE_LICENSE("GPL");
