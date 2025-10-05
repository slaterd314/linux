// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP MFRC522 NFC Reader - Core Driver
 *
 * Provides bus-agnostic core logic for MFRC522 devices.
 * Bus-specific frontends (SPI, I2C, UART) should call into
 * these helpers for probe/remove and register access.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include "core.h"   /* local header for shared structs/prototypes */

/* ------------------------------------------------------------------------- */
/* Core device structure                                                     */
/* ------------------------------------------------------------------------- */

struct mfrc522_dev {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock; /* serialize register access */
	struct gpio_desc *reset_gpiod;
	/* Future: add IRQ handling, FIFO mgmt, crypto1 state, etc. */
};

/* ------------------------------------------------------------------------- */
/* Core probe/remove helpers                                                 */
/* ------------------------------------------------------------------------- */

int mfrc522_core_probe(struct device *dev, struct regmap *regmap)
{
	struct mfrc522_dev *chip;
	int ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = dev;
	chip->regmap = regmap;
	mutex_init(&chip->lock);

	chip->reset_gpiod = devm_gpiod_get_optional(dev, "reset",
					     GPIOD_OUT_HIGH);
	if (IS_ERR(chip->reset_gpiod))
		return dev_err_probe(dev, PTR_ERR(chip->reset_gpiod),
				   "Failed to acquire reset GPIO\n");

	dev_set_drvdata(dev, chip);

	ret = mfrc522_hw_reset(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Hardware reset failed\n");

	dev_info(dev, "MFRC522 core initialized\n");
	return 0;
}
EXPORT_SYMBOL_GPL(mfrc522_core_probe);

void mfrc522_core_remove(struct device *dev)
{
	struct mfrc522_dev *chip = dev_get_drvdata(dev);

	if (!chip)
		return;

	if (chip->reset_gpiod)
		gpiod_set_value_cansleep(chip->reset_gpiod, 0);

	dev_info(dev, "MFRC522 core removed\n");
}
EXPORT_SYMBOL_GPL(mfrc522_core_remove);

int mfrc522_hw_reset(struct device *dev)
{
	struct mfrc522_dev *chip = dev_get_drvdata(dev);

	if (!chip)
		return -ENODEV;

	if (!chip->reset_gpiod)
		return 0;

	mutex_lock(&chip->lock);
	gpiod_set_value_cansleep(chip->reset_gpiod, 0);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(chip->reset_gpiod, 1);
	usleep_range(5000, 10000);
	mutex_unlock(&chip->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mfrc522_hw_reset);

int mfrc522_fifo_write(struct device *dev, const u8 *buf, size_t len)
{
	struct mfrc522_dev *chip = dev_get_drvdata(dev);
	int ret;

	if (!chip)
		return -ENODEV;

	if (!len)
		return 0;

	if (!buf)
		return -EINVAL;

	if (len > MFRC522_FIFO_SIZE)
		return -EINVAL;

	mutex_lock(&chip->lock);
	ret = regmap_raw_write(chip->regmap, MFRC522_REG_FIFO_DATA, buf, len);
	mutex_unlock(&chip->lock);

	if (ret)
		return ret;

	return len;
}
EXPORT_SYMBOL_GPL(mfrc522_fifo_write);

int mfrc522_fifo_read(struct device *dev, u8 *buf, size_t len)
{
	struct mfrc522_dev *chip = dev_get_drvdata(dev);
	unsigned int level;
	size_t to_read;
	int ret;

	if (!chip)
		return -ENODEV;

	if (!buf)
		return -EINVAL;

	if (!len)
		return 0;

	if (len > MFRC522_FIFO_SIZE)
		return -EINVAL;

	mutex_lock(&chip->lock);
	ret = regmap_read(chip->regmap, MFRC522_REG_FIFO_LEVEL, &level);
	if (ret)
		goto out_unlock;

	to_read = min_t(size_t, len, level & MFRC522_FIFO_LEVEL_MASK);
	if (!to_read) {
		ret = 0;
		goto out_unlock;
	}

	ret = regmap_raw_read(chip->regmap, MFRC522_REG_FIFO_DATA, buf, to_read);
	if (ret)
		goto out_unlock;

	ret = to_read;

out_unlock:
	mutex_unlock(&chip->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mfrc522_fifo_read);

int mfrc522_fifo_flush(struct device *dev)
{
	struct mfrc522_dev *chip = dev_get_drvdata(dev);
	int ret;

	if (!chip)
		return -ENODEV;

	mutex_lock(&chip->lock);
	ret = regmap_write(chip->regmap, MFRC522_REG_FIFO_LEVEL,
			   MFRC522_FIFO_FLUSH);
	mutex_unlock(&chip->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mfrc522_fifo_flush);

int mfrc522_fifo_level(struct device *dev, u8 *level)
{
	struct mfrc522_dev *chip = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	if (!chip)
		return -ENODEV;

	if (!level)
		return -EINVAL;

	mutex_lock(&chip->lock);
	ret = regmap_read(chip->regmap, MFRC522_REG_FIFO_LEVEL, &value);
	mutex_unlock(&chip->lock);
	if (ret)
		return ret;

	*level = value & MFRC522_FIFO_LEVEL_MASK;

	return 0;
}
EXPORT_SYMBOL_GPL(mfrc522_fifo_level);

/* ------------------------------------------------------------------------- */
/* Module boilerplate                                                        */
/* ------------------------------------------------------------------------- */

MODULE_DESCRIPTION("NXP MFRC522 NFC Reader Core");
MODULE_AUTHOR("Daniel R. Slater <slaterd314@gmail.com>");
MODULE_LICENSE("GPL");

