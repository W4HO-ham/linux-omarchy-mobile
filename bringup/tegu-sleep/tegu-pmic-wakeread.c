// SPDX-License-Identifier: GPL-2.0
/*
 * Bring-up only (Pixel 9a SLEEP-PLAN S1): which S2MPG14 interrupt source
 * wakes s2idle about a second in? In resume_noirq, before the PMIC's
 * interrupt thread runs, read INT1-INT5 and the masks INT1M-INT5M through
 * the PMIC's own regmap (ACPM) and log them. INT1-INT5 clear on read, so the
 * driver sees nothing for that wake; nothing here writes the PMIC.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

static char *pmic = "firmware:power-management:pmic-1";
module_param(pmic, charp, 0444);
static bool enable = true;
module_param(enable, bool, 0644);

static struct regmap *map;

static void dump(const char *when)
{
	unsigned int v[10];
	int i, ret;

	for (i = 0; i < 10; i++) {
		ret = regmap_read(map, i, &v[i]);
		if (ret) {
			pr_info("tegu-pmic-wakeread %s: read 0x%02x failed %d\n", when, i, ret);
			return;
		}
	}
	pr_info("tegu-pmic-wakeread %s: INT1-5 %02x %02x %02x %02x %02x  masks %02x %02x %02x %02x %02x\n",
		when, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
}

static int wakeread_suspend_noirq(struct device *dev)
{
	if (enable)
		dump("at sleep");
	return 0;
}

static int wakeread_resume_noirq(struct device *dev)
{
	if (enable)
		dump("at wake");
	return 0;
}

static const struct dev_pm_ops wakeread_pm = {
	.suspend_noirq = wakeread_suspend_noirq,
	.resume_noirq = wakeread_resume_noirq,
};

static struct platform_driver wakeread_driver = {
	.driver = { .name = "tegu-pmic-wakeread", .pm = &wakeread_pm },
};

static struct platform_device *pdev;

static int __init wakeread_init(void)
{
	struct device *d;
	int ret;

	d = bus_find_device_by_name(&platform_bus_type, NULL, pmic);
	if (!d)
		return -ENODEV;
	map = dev_get_regmap(d, "pmic");
	put_device(d);
	if (!map)
		return -ENODEV;
	ret = platform_driver_register(&wakeread_driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("tegu-pmic-wakeread", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&wakeread_driver);
		return PTR_ERR(pdev);
	}
	pr_info("tegu-pmic-wakeread: %s's pmic regmap found\n", pmic);
	return 0;
}

static void __exit wakeread_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&wakeread_driver);
}

module_init(wakeread_init);
module_exit(wakeread_exit);
MODULE_DESCRIPTION("Bring-up: log the S2MPG14 interrupt registers around s2idle");
MODULE_LICENSE("GPL");
