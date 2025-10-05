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
#include <linux/interrupt.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/types.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/irq.h>

#include "core.h"   /* local header for shared structs/prototypes */

/* ------------------------------------------------------------------------- */
/* Core device structure                                                     */
/* ------------------------------------------------------------------------- */

struct mfrc522_dev {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock; /* serialize register access */
	struct gpio_desc *reset_gpiod;
	int irq;
	spinlock_t irq_lock;
	wait_queue_head_t irq_wq;
	u8 comm_irq_pending;
	u8 div_irq_pending;
	bool irq_enabled;
};

static irqreturn_t mfrc522_irq_thread(int irq, void *data)
{
	struct mfrc522_dev *chip = data;
	unsigned int comm = 0;
	unsigned int div = 0;
	int ret;

	mutex_lock(&chip->lock);
	ret = regmap_read(chip->regmap, MFRC522_REG_COMMIRQ, &comm);
	if (ret)
		goto out_unlock;

	ret = regmap_read(chip->regmap, MFRC522_REG_DIVIRQ, &div);
	if (ret)
		goto out_unlock;

	regmap_write(chip->regmap, MFRC522_REG_COMMIRQ,
		     comm & MFRC522_IRQ_STATUS_MASK);
	regmap_write(chip->regmap, MFRC522_REG_DIVIRQ,
		     div & MFRC522_IRQ_STATUS_MASK);

out_unlock:
	mutex_unlock(&chip->lock);

	if (!ret) {
		unsigned long flags;
		u8 comm_status = comm & MFRC522_IRQ_STATUS_MASK;
		u8 div_status = div & MFRC522_IRQ_STATUS_MASK;

		spin_lock_irqsave(&chip->irq_lock, flags);
		chip->comm_irq_pending |= comm_status;
		chip->div_irq_pending |= div_status;
		spin_unlock_irqrestore(&chip->irq_lock, flags);

		wake_up(&chip->irq_wq);
	}

	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------------- */
/* Core probe/remove helpers                                                 */
/* ------------------------------------------------------------------------- */

int mfrc522_core_probe(struct device *dev, struct regmap *regmap, int irq)
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

	chip->irq = irq;
	if (chip->irq <= 0) {
		chip->irq = fwnode_irq_get(dev_fwnode(dev), 0);
		if (chip->irq == -EPROBE_DEFER)
			return chip->irq;
		if (chip->irq < 0)
			chip->irq = 0;
	}
	chip->irq_enabled = false;
	chip->comm_irq_pending = 0;
	chip->div_irq_pending = 0;
	spin_lock_init(&chip->irq_lock);
	init_waitqueue_head(&chip->irq_wq);

	if (chip->irq > 0) {
		ret = devm_request_threaded_irq(dev, chip->irq, NULL,
					 mfrc522_irq_thread,
					 IRQF_ONESHOT,
					 dev_name(dev), chip);
		if (ret) {
			dev_warn(dev, "Failed to request IRQ %d: %d\n",
				 chip->irq, ret);
			chip->irq = 0;
		}
	}

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

	if (chip->irq_enabled)
		mfrc522_irq_disable(dev);

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

int mfrc522_irq_enable(struct device *dev, u8 comm_mask, u8 div_mask)
{
	struct mfrc522_dev *chip = dev_get_drvdata(dev);
	int ret;
	unsigned long flags;

	if (!chip)
		return -ENODEV;

	if (chip->irq <= 0)
		return -ENOTSUPP;

	mutex_lock(&chip->lock);
	ret = regmap_write(chip->regmap, MFRC522_REG_COMMIEN, comm_mask);
	if (ret)
		goto out_unlock;

	ret = regmap_write(chip->regmap, MFRC522_REG_DIVLEN, div_mask);
	if (ret)
		goto out_unlock;

	ret = regmap_write(chip->regmap, MFRC522_REG_COMMIRQ,
			   MFRC522_IRQ_STATUS_MASK);
	if (ret)
		goto out_unlock;

	ret = regmap_write(chip->regmap, MFRC522_REG_DIVIRQ,
			   MFRC522_IRQ_STATUS_MASK);
	if (!ret)
		chip->irq_enabled = true;

out_unlock:
	mutex_unlock(&chip->lock);

	if (!ret) {
		spin_lock_irqsave(&chip->irq_lock, flags);
		chip->comm_irq_pending = 0;
		chip->div_irq_pending = 0;
		spin_unlock_irqrestore(&chip->irq_lock, flags);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(mfrc522_irq_enable);

int mfrc522_irq_disable(struct device *dev)
{
	struct mfrc522_dev *chip = dev_get_drvdata(dev);
	unsigned long flags;

	if (!chip)
		return -ENODEV;

	if (chip->irq <= 0)
		return -ENOTSUPP;

	mutex_lock(&chip->lock);
	regmap_write(chip->regmap, MFRC522_REG_COMMIEN, 0x00);
	regmap_write(chip->regmap, MFRC522_REG_DIVLEN, 0x00);
	chip->irq_enabled = false;
	mutex_unlock(&chip->lock);

	spin_lock_irqsave(&chip->irq_lock, flags);
	chip->comm_irq_pending = 0;
	chip->div_irq_pending = 0;
	spin_unlock_irqrestore(&chip->irq_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(mfrc522_irq_disable);

int mfrc522_irq_wait(struct device *dev, u8 comm_mask, u8 div_mask,
		       unsigned long timeout_jiffies,
		       u8 *comm_status, u8 *div_status)
{
	struct mfrc522_dev *chip = dev_get_drvdata(dev);
	unsigned long flags;
	long ret;
	u8 comm = 0;
	u8 div = 0;

	if (!chip)
		return -ENODEV;

	if (chip->irq <= 0)
		return -ENOTSUPP;

	if (!chip->irq_enabled)
		return -EIO;

	ret = wait_event_interruptible_timeout(chip->irq_wq,
			(chip->comm_irq_pending & comm_mask) ||
			(chip->div_irq_pending & div_mask),
			timeout_jiffies);
	if (ret == 0)
		return -ETIMEDOUT;
	if (ret < 0)
		return (int)ret;

	spin_lock_irqsave(&chip->irq_lock, flags);
	comm = chip->comm_irq_pending & comm_mask;
	div = chip->div_irq_pending & div_mask;
	chip->comm_irq_pending &= ~comm;
	chip->div_irq_pending &= ~div;
	spin_unlock_irqrestore(&chip->irq_lock, flags);

	if (comm_status)
		*comm_status = comm;
	if (div_status)
		*div_status = div;

	return (comm || div) ? 0 : -EAGAIN;
}
EXPORT_SYMBOL_GPL(mfrc522_irq_wait);

#if IS_ENABLED(CONFIG_MFRC522_KUNIT_TEST)
int mfrc522_test_set_irq(struct device *dev, int irq)
{
	struct mfrc522_dev *chip = dev_get_drvdata(dev);

	if (!chip)
		return -ENODEV;

	chip->irq = irq;
	return 0;
}
EXPORT_SYMBOL_GPL(mfrc522_test_set_irq);

int mfrc522_test_set_irq_pending(struct device *dev, u8 comm_status,
			      u8 div_status)
{
	struct mfrc522_dev *chip = dev_get_drvdata(dev);
	unsigned long flags;

	if (!chip)
		return -ENODEV;

	spin_lock_irqsave(&chip->irq_lock, flags);
	chip->comm_irq_pending |= comm_status & MFRC522_IRQ_STATUS_MASK;
	chip->div_irq_pending |= div_status & MFRC522_IRQ_STATUS_MASK;
	spin_unlock_irqrestore(&chip->irq_lock, flags);

	wake_up(&chip->irq_wq);

	return 0;
}
EXPORT_SYMBOL_GPL(mfrc522_test_set_irq_pending);
#endif

/* ------------------------------------------------------------------------- */
/* Module boilerplate                                                        */
/* ------------------------------------------------------------------------- */

MODULE_DESCRIPTION("NXP MFRC522 NFC Reader Core");
MODULE_AUTHOR("Daniel R. Slater <slaterd314@gmail.com>");
MODULE_LICENSE("GPL");

