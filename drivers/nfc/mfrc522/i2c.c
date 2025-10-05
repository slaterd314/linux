// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP MFRC522 NFC Reader - I2C Transport
 *
 * Thin I2C wrapper around the MFRC522 core driver.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/of_device.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "core.h"  /* local header for core probe/remove */

/* Datasheet FIFO depth is 64 bytes */
#define MFRC522_I2C_MAX_XFER	MFRC522_FIFO_SIZE

struct mfrc522_i2c {
	struct i2c_client *client;
	struct mutex lock; /* serialize bus access */
};

static int mfrc522_i2c_regmap_write(void *context, const void *data,
			       size_t count)
{
	struct mfrc522_i2c *mi2c = context;
	const unsigned char *buf = data;
	unsigned char tx_buf[MFRC522_I2C_MAX_XFER + 1];
	size_t val_len;
	int ret;

	if (count < 2)
		return -EINVAL;

	if (!mfrc522_reg_valid(buf[0]))
		return -EINVAL;

	val_len = count - 1;
	if (val_len > MFRC522_I2C_MAX_XFER)
		return -EINVAL;

	tx_buf[0] = mfrc522_encode_reg(buf[0]);
	memcpy(&tx_buf[1], &buf[1], val_len);

	mutex_lock(&mi2c->lock);
	ret = i2c_master_send(mi2c->client, tx_buf, val_len + 1);
	mutex_unlock(&mi2c->lock);

	if (ret != val_len + 1)
		return ret < 0 ? ret : -EIO;

	return 0;
}

static int mfrc522_i2c_regmap_read(void *context, const void *reg,
			      size_t reg_size, void *val, size_t val_size)
{
	struct mfrc522_i2c *mi2c = context;
	const unsigned char *addrp = reg;
	unsigned char addr;
	struct i2c_msg msgs[2];
	int ret;

	if (reg_size != 1 || !val_size)
		return -EINVAL;

	if (!mfrc522_reg_valid(*addrp))
		return -EINVAL;

	if (val_size > MFRC522_I2C_MAX_XFER)
		return -EINVAL;

	addr = mfrc522_encode_reg(*addrp);

	msgs[0].addr = mi2c->client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &addr;

	msgs[1].addr = mi2c->client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = val_size;
	msgs[1].buf = val;

	mutex_lock(&mi2c->lock);
	ret = i2c_transfer(mi2c->client->adapter, msgs, ARRAY_SIZE(msgs));
	mutex_unlock(&mi2c->lock);

	if (ret != ARRAY_SIZE(msgs))
		return ret < 0 ? ret : -EIO;

	return 0;
}

static const struct regmap_bus mfrc522_i2c_regmap_bus = {
	.write		= mfrc522_i2c_regmap_write,
	.read		= mfrc522_i2c_regmap_read,
	.max_raw_write	= MFRC522_I2C_MAX_XFER + 1,
	.max_raw_read	= MFRC522_I2C_MAX_XFER,
};

static const struct regmap_config mfrc522_i2c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MFRC522_REG_MASK,
	.cache_type = REGCACHE_NONE,
};

static int mfrc522_i2c_probe(struct i2c_client *client)
{
	struct regmap *regmap;
	struct mfrc522_i2c *mi2c;

	mi2c = devm_kzalloc(&client->dev, sizeof(*mi2c), GFP_KERNEL);
	if (!mi2c)
		return -ENOMEM;

	mi2c->client = client;
	mutex_init(&mi2c->lock);

	regmap = devm_regmap_init(&client->dev, &mfrc522_i2c_regmap_bus, mi2c,
			      &mfrc522_i2c_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "Failed to init regmap\n");
		return PTR_ERR(regmap);
	}

	return mfrc522_core_probe(&client->dev, regmap);
}

static void mfrc522_i2c_remove(struct i2c_client *client)
{
	mfrc522_core_remove(&client->dev);
}

static const struct of_device_id mfrc522_i2c_of_match[] = {
	{ .compatible = "nxp,mfrc522-i2c", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mfrc522_i2c_of_match);

static const struct i2c_device_id mfrc522_i2c_id[] = {
	{ "mfrc522-i2c", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mfrc522_i2c_id);

static struct i2c_driver mfrc522_i2c_driver = {
	.driver = {
		.name           = "mfrc522-i2c",
		.of_match_table = mfrc522_i2c_of_match,
	},
	.probe    = mfrc522_i2c_probe,
	.remove   = mfrc522_i2c_remove,
	.id_table = mfrc522_i2c_id,
};

module_i2c_driver(mfrc522_i2c_driver);

MODULE_DESCRIPTION("NXP MFRC522 NFC Reader I2C driver");
MODULE_AUTHOR("Daniel R. Slater <slaterd314@gmail.com>");
MODULE_LICENSE("GPL");

