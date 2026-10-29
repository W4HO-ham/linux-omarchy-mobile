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

/*
 * omamobile test: mainline spi-s3c64xx cuts a PIO transfer of 64 bytes or
 * more into 63-byte pieces, which a 32-bit word can't be split into (the
 * -110 timeouts of the plain 32-bit test). Send 32-byte transfers of one
 * message instead (chip select held): each fits the polling path, as stock's
 * 64-byte reads do in Google's driver.
 */
#define GNSS_SPI_PIECE	32

static int gnss_spi_xfer(const char *tx, char *rx, unsigned int size)
{
	unsigned int n = DIV_ROUND_UP(size, GNSS_SPI_PIECE), i, off;
	struct spi_transfer *t;
	struct spi_message msg;
	char *tmp = NULL;
	int ret;

	if (size % 4)
		return -EINVAL;
	if (tx) {
		tmp = kmemdup(tx, size, GFP_KERNEL);
		if (!tmp)
			return -ENOMEM;
		gnss_spi_swab(tmp, size);
	}
	t = kcalloc(n, sizeof(*t), GFP_KERNEL);
	if (!t) {
		kfree(tmp);
		return -ENOMEM;
	}
	spi_message_init(&msg);
	for (i = 0, off = 0; i < n; i++, off += GNSS_SPI_PIECE) {
		t[i].len = min(GNSS_SPI_PIECE, size - off);
		t[i].tx_buf = tmp ? tmp + off : NULL;
		t[i].rx_buf = rx ? rx + off : NULL;
		t[i].bits_per_word = SPI_BITS_PER_WORD;
		spi_message_add_tail(&t[i], &msg);
	}
	ret = spi_sync(gnss_if.spi, &msg);
	if (ret < 0)
		gif_err("spi_sync() error:%d\n", ret);
	if (rx)
		gnss_spi_swab(rx, size);
	kfree(t);
	kfree(tmp);
	return ret;
}

int gnss_spi_send(char *buff, unsigned int size, char *rx_buff)
{
	return gnss_spi_xfer(buff, rx_buff, size);
}
EXPORT_SYMBOL(gnss_spi_send);

int gnss_spi_recv(char *buff, unsigned int size)
{
	return gnss_spi_xfer(NULL, buff, size);
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

