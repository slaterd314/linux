// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP MFRC522 NFC Reader - SPI Transport
 *
 * Thin SPI wrapper around the MFRC522 core driver.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

#include "core.h"  /* local header for core probe/remove */

/* MFRC522 FIFO holds 64 bytes; keep transfers within that window */
#define MFRC522_SPI_MAX_XFER	MFRC522_FIFO_SIZE

static int mfrc522_spi_regmap_write(void *context, const void *data,
			       size_t count)
{
	struct spi_device *spi = context;
	const unsigned char *buf = data;
	size_t val_len;
	unsigned char tx_buf[MFRC522_SPI_MAX_XFER + 1];
	int ret;

	if (count < 2)
		return -EINVAL;

	if (!mfrc522_reg_valid(buf[0]))
		return -EINVAL;

	val_len = count - 1;
	if (val_len > MFRC522_SPI_MAX_XFER)
		return -EINVAL;

	tx_buf[0] = mfrc522_encode_reg(buf[0]) << 1; /* write command */
	memcpy(&tx_buf[1], &buf[1], val_len);

	ret = spi_write(spi, tx_buf, val_len + 1);

	return ret;
}

static int mfrc522_spi_regmap_read(void *context, const void *reg,
			      size_t reg_size, void *val, size_t val_size)
{
	struct spi_device *spi = context;
	const unsigned char *addrp = reg;
	unsigned char cmd;
	int ret;

	if (reg_size != 1 || !val_size)
		return -EINVAL;

	if (!mfrc522_reg_valid(*addrp))
		return -EINVAL;

	if (val_size > MFRC522_SPI_MAX_XFER)
		return -EINVAL;

	cmd = (mfrc522_encode_reg(*addrp) << 1) | 0x80; /* read command */

	ret = spi_write_then_read(spi, &cmd, 1, val, val_size);

	return ret;
}

static const struct regmap_bus mfrc522_spi_regmap_bus = {
	.write		= mfrc522_spi_regmap_write,
	.read		= mfrc522_spi_regmap_read,
	.max_raw_write	= MFRC522_SPI_MAX_XFER,
	.max_raw_read	= MFRC522_SPI_MAX_XFER,
};

static const struct regmap_config mfrc522_spi_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MFRC522_REG_MASK,
	.cache_type = REGCACHE_NONE,
};

static int mfrc522_spi_probe(struct spi_device *spi)
{
	struct regmap *regmap;
	int ret;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	regmap = devm_regmap_init(&spi->dev, &mfrc522_spi_regmap_bus, spi,
			      &mfrc522_spi_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&spi->dev, "Failed to init regmap\n");
		return PTR_ERR(regmap);
	}

	return mfrc522_core_probe(&spi->dev, regmap, spi->irq);
}

static void mfrc522_spi_remove(struct spi_device *spi)
{
	mfrc522_core_remove(&spi->dev);
}

static const struct of_device_id mfrc522_spi_of_match[] = {
	{ .compatible = "nxp,mfrc522-spi", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mfrc522_spi_of_match);

static struct spi_driver mfrc522_spi_driver = {
	.driver = {
		.name           = "mfrc522-spi",
		.of_match_table = mfrc522_spi_of_match,
	},
	.probe  = mfrc522_spi_probe,
	.remove = mfrc522_spi_remove,
};

module_spi_driver(mfrc522_spi_driver);

MODULE_DESCRIPTION("NXP MFRC522 NFC Reader SPI driver");
MODULE_AUTHOR("Daniel R. Slater <slaterd314@gmail.com>");
MODULE_LICENSE("GPL");

