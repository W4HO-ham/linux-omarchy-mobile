// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Samsung Electronics.
 *
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spi/spi.h>
#include <linux/slab.h>
#include <linux/swab.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "include/gnss.h"
#include "gnss_spi.h"


/*
 * omamobile test: 32-bit words, as stock, with each word byte-swapped in
 * software (stock: the vendor driver's swap-mode), so the bytes go on the
 * wire in memory order. CHPP pads its frames to a multiple of 4.
 */
static void gnss_spi_swab(void *buf, unsigned int size)
{
	u32 *w = buf;
	unsigned int i;

	for (i = 0; i < size / 4; i++)
		w[i] = swab32(w[i]);
}

int gnss_spi_send(char *buff, unsigned int size, char *rx_buff)
{
	char *tmp;
	int ret = 0;
	struct spi_message msg;
	struct spi_transfer tx;

	if (size % 4)
		return -EINVAL;
	tmp = kmemdup(buff, size, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	gnss_spi_swab(tmp, size);
	memset(&tx, 0, sizeof(struct spi_transfer));
	spi_message_init(&msg);
	tx.len = size;
	tx.tx_buf = tmp;
	tx.bits_per_word = SPI_BITS_PER_WORD;
	if (rx_buff)
		tx.rx_buf = rx_buff;
	spi_message_add_tail(&tx, &msg);
	ret = spi_sync(gnss_if.spi, &msg);
	if (ret < 0)
		gif_err("spi_sync() error:%d\n", ret);

	kfree(tmp);
	if (rx_buff)
		gnss_spi_swab(rx_buff, size);

	return ret;
}
EXPORT_SYMBOL(gnss_spi_send);

int gnss_spi_recv(char *buff, unsigned int size)
{
	int ret = 0;
	struct spi_message msg;
	struct spi_transfer rx;

	memset(&rx, 0, sizeof(struct spi_transfer));
	rx.len = size;
	rx.rx_buf = buff;
	rx.bits_per_word = SPI_BITS_PER_WORD;
	spi_message_init(&msg);
	spi_message_add_tail(&rx, &msg);
	ret = spi_sync(gnss_if.spi, &msg);
	if (ret < 0)
		gif_err("spi_sync() error:%d\n", ret);

	gnss_spi_swab(buff, size);

	return ret;

}
EXPORT_SYMBOL(gnss_spi_recv);

static int gnss_spi_probe(struct spi_device *spi)
{
	int ret = 0;

	gif_info("+++\n");
	mutex_init(&gnss_if.lock);

	if (spi_setup(spi)) {
		gif_err("ERR! spi_setup fail\n");
		ret = -EINVAL;
		goto err_setup;
	}
	spi_set_drvdata(spi, &gnss_if);
	gnss_if.spi = spi;
	gif_info("---\n");

	return 0;

err_setup:
	mutex_destroy(&gnss_if.lock);

	return ret;
}

static void gnss_spi_remove (struct spi_device *spi)
{
	struct gnss_spi *gnss_if = spi_get_drvdata(spi);

	mutex_destroy(&gnss_if->lock);
}

static const struct of_device_id gnss_spi_dt_match[] = {
	{ .compatible = "samsung,gnss-spi" },
	{},
};
MODULE_DEVICE_TABLE(of, gnss_spi_dt_match);

static struct spi_driver gnss_spi_driver = {
	.probe = gnss_spi_probe,
	.remove = gnss_spi_remove,
	.driver = {
		.name = "gnss_spi",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(gnss_spi_dt_match),
		.suppress_bind_attrs = true,
	},
};
module_spi_driver(gnss_spi_driver);

MODULE_DESCRIPTION("Exynos SPI driver for GNSS communication");
MODULE_LICENSE("GPL");

