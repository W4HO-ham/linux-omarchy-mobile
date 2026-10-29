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
#include <linux/platform_data/spi-s3c64xx.h>
#include <linux/io.h>
#include <linux/delay.h>

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

	if (SPI_BITS_PER_WORD != 32)
		return;

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
#define GNSS_SPI_PIECE	SZ_64K

/* omamobile test: override the controller's feedback delay (FB_CLK_SEL) */
static int fb_delay = -1;
module_param(fb_delay, int, 0444);

/*
 * omamobile test: CMU_HSI0 USI3. The clock driver keeps the mux read-only
 * on its 399.36 MHz parent and the divider stops at /16, so the SPI runs at
 * 24.96 MHz / 4 = 6.24 MHz whatever it asks for. osc=1 moves the mux to
 * OSCCLK (24.576 MHz), so the divider the driver then picks gives a slower
 * clock; speed_hz sets the rate the driver asks for.
 */
#define CMU_HSI0_USI3_MUX	0x11001020
#define CMU_HSI0_USI3_DIV	0x11001820
static int osc;
module_param(osc, int, 0444);
static int speed_hz;
module_param(speed_hz, int, 0444);
/* div >= 0: hold the USI3 divider at this value (stock 12 MHz: osc=1, div=1) */
static int div = -1;
/* rx_cpha=1: read-only transfers in SPI mode 1, writes stay in mode 0 */
static int rx_cpha;
module_param(rx_cpha, int, 0444);
/*
 * realign=1: Kepler's data comes in exactly one bit late with feedback
 * delay 3 (MODEM-PLAN G3), so shift every received buffer left one bit.
 */
static int realign;
module_param(realign, int, 0444);

/*
 * realign=2: Kepler's data is on time at first and one bit late later, so
 * choose per read: a CHPP preamble (68 43) at bit 0 keeps the read as it
 * is, one at bit 1 shifts it; a read without one keeps the last choice.
 */
static bool gnss_spi_late;

static int gnss_spi_preamble_at(const u8 *b, unsigned int n)
{
	unsigned int i;

	for (i = 0; i + 2 < n; i++) {
		u32 w = b[i] << 16 | b[i + 1] << 8 | b[i + 2];

		if ((w >> 8) == 0x6843)
			return 0;
		if (((w >> 7) & 0xffff) == 0x6843)
			return 1;
	}
	return -1;
}

static void gnss_spi_shl1(u8 *b, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		b[i] = (b[i] << 1) | (i + 1 < n ? b[i + 1] >> 7 : 0);
}
module_param(div, int, 0444);
static void __iomem *gnss_spi_div;

static void gnss_spi_hold_div(void)
{
	u32 v;
	int i;

	if (div < 0 || !gnss_spi_div)
		return;
	v = readl(gnss_spi_div);
	if ((v & 0xf) == div)
		return;
	writel((v & ~0xf) | div, gnss_spi_div);
	for (i = 0; i < 1000 && (readl(gnss_spi_div) & BIT(16)); i++)
		udelay(1);
	gif_info("usi3 div 0x%08x -> 0x%08x\n", v, readl(gnss_spi_div));
}

static void gnss_spi_cmu(void)
{
	void __iomem *mux, *div;

	mux = ioremap(CMU_HSI0_USI3_MUX, 4);
	div = ioremap(CMU_HSI0_USI3_DIV, 4);
	if (!mux || !div)
		goto out;
	gif_info("usi3 mux 0x%08x div 0x%08x\n", readl(mux), readl(div));
	if (osc) {
		writel(readl(mux) | 1, mux);
		gif_info("usi3 mux now 0x%08x\n", readl(mux));
	}
out:
	if (mux)
		iounmap(mux);
	if (div)
		iounmap(div);
}

static void __maybe_unused gnss_spi_cmu_div(void)
{
	void __iomem *div = ioremap(CMU_HSI0_USI3_DIV, 4);

	if (div) {
		gif_info("usi3 div 0x%08x\n", readl(div));
		iounmap(div);
	}
}

/* omamobile test: microseconds from chip select to the first clock */
static int cs_setup_us;
module_param(cs_setup_us, int, 0444);

static int gnss_spi_xfer(const char *tx, char *rx, unsigned int size)
{
	unsigned int n = DIV_ROUND_UP(size, GNSS_SPI_PIECE), i, off;
	struct spi_transfer *t;
	struct spi_message msg;
	char *tmp = NULL;
	int ret;

	if (SPI_BITS_PER_WORD == 32 && size % 4)
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
	gnss_spi_hold_div();
	if (rx_cpha)
		mutex_lock(&gnss_if.lock);
	if (rx_cpha) {
		/*
		 * spi-s3c64xx reconfigures on every transfer here (its
		 * cur_speed is the 6.24 MHz it gets, never the rate asked),
		 * and takes cur_mode from spi->mode when it does.
		 */
		if (tx)
			gnss_if.spi->mode &= ~SPI_CPHA;
		else
			gnss_if.spi->mode |= SPI_CPHA;
	}
	ret = spi_sync(gnss_if.spi, &msg);
	if (rx_cpha)
		mutex_unlock(&gnss_if.lock);
	if (ret < 0)
		gif_err("spi_sync() error:%d\n", ret);
	gnss_spi_hold_div();
	{
		static bool once;

		if (!once) {
			once = true;
			gnss_spi_cmu_div();
		}
	}
	if (rx && realign == 1 && ret >= 0)
		gnss_spi_shl1(rx, size);
	if (rx && realign == 2 && ret >= 0) {
		int at = gnss_spi_preamble_at(rx, size);

		if (at >= 0 && gnss_spi_late != at) {
			gnss_spi_late = at;
			gif_info("realign: Kepler data %s\n", at ? "one bit late" : "on time");
		}
		if (gnss_spi_late)
			gnss_spi_shl1(rx, size);
	}
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

	gnss_spi_cmu();
	if (div >= 0) {
		gnss_spi_div = ioremap(CMU_HSI0_USI3_DIV, 4);
		gnss_spi_hold_div();
	}
	if (speed_hz > 0)
		spi->max_speed_hz = speed_hz;
	if (spi_setup(spi)) {
		gif_err("ERR! spi_setup fail\n");
		ret = -EINVAL;
		goto err_setup;
	}
	if (fb_delay >= 0 && spi->controller_data) {
		struct s3c64xx_spi_csinfo *cs = spi->controller_data;

		gif_info("feedback delay %u -> %d\n", cs->fb_delay, fb_delay);
		cs->fb_delay = fb_delay;
	}
	if (cs_setup_us > 0) {
		spi->cs_setup.value = cs_setup_us;
		spi->cs_setup.unit = SPI_DELAY_UNIT_USECS;
		gif_info("cs_setup %d us\n", cs_setup_us);
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

