// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP MFRC522 NFC Reader - UART Transport
 *
 * Thin UART/serdev wrapper around the MFRC522 core driver.
 */

#include <linux/types.h>
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/gfp.h>
#include <linux/regmap.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "core.h"  /* local header for core probe/remove */

#define MFRC522_UART_MAX_XFER		MFRC522_FIFO_SIZE
#define MFRC522_UART_TIMEOUT_MS	50
#define MFRC522_UART_READ_FLAG	BIT(7)

struct mfrc522_uart {
	struct serdev_device *serdev;
	struct mutex lock; /* serialize transactions */
	spinlock_t rx_lock; /* protect receive state */
	struct completion rx_complete;

	unsigned char *rx_buf;
	size_t rx_expected;
	size_t rx_count;
};

static int mfrc522_uart_send(struct mfrc522_uart *uart,
			       const unsigned char *buf, size_t len)
{
	size_t written = 0;

	while (written < len) {
		int ret = serdev_device_write_buf(uart->serdev, buf + written,
						       len - written);
		if (ret < 0)
			return ret;
		if (!ret)
			return -EIO;
		written += ret;
	}

	serdev_device_wait_until_sent(uart->serdev,
		msecs_to_jiffies(MFRC522_UART_TIMEOUT_MS));

	return 0;
}

static size_t mfrc522_uart_receive_buf(struct serdev_device *serdev,
					     const unsigned char *buf, size_t count)
{
	struct mfrc522_uart *uart = serdev_device_get_drvdata(serdev);
	unsigned long flags;
	int complete_xfer = 0;
	size_t i;

	spin_lock_irqsave(&uart->rx_lock, flags);

	if (!uart->rx_buf || !uart->rx_expected) {
		spin_unlock_irqrestore(&uart->rx_lock, flags);
		return count;
	}

	for (i = 0; i < count && uart->rx_count < uart->rx_expected; i++)
		uart->rx_buf[uart->rx_count++] = buf[i];

	if (uart->rx_count >= uart->rx_expected)
		complete_xfer = 1;

	spin_unlock_irqrestore(&uart->rx_lock, flags);

	if (complete_xfer)
		complete(&uart->rx_complete);

	return count;
}

static const struct serdev_device_ops mfrc522_uart_client_ops = {
	.receive_buf = mfrc522_uart_receive_buf,
};

static int mfrc522_uart_regmap_write(void *context,
			     const void *data, size_t count)
{
	struct mfrc522_uart *uart = context;
	const unsigned char *buf = data;
	unsigned char *tx;
	int ret;

	if (!count)
		return -EINVAL;

	tx = kmemdup(buf, count, GFP_KERNEL);
	if (!tx)
		return -ENOMEM;

	/* Ensure reserved bits stay cleared. */
	tx[0] = mfrc522_encode_reg(tx[0]);

	mutex_lock(&uart->lock);
	ret = mfrc522_uart_send(uart, tx, count);
	mutex_unlock(&uart->lock);

	kfree(tx);

	return ret;
}

static int mfrc522_uart_regmap_read(void *context,
			    const void *reg, size_t reg_size,
			    void *val, size_t val_size)
{
	struct mfrc522_uart *uart = context;
	const unsigned char *addrp = reg;
	unsigned long timeout;
	unsigned long flags;
	unsigned char addr;
	unsigned char *rx_tmp;
	int ret = 0;
	struct device *dev = &uart->serdev->dev;

	if (reg_size != 1 || !val_size)
		return -EINVAL;

	if (!mfrc522_reg_valid(*addrp))
		return -EINVAL;

	if (val_size > MFRC522_UART_MAX_XFER)
		dev_dbg(dev, "large read (%zu bytes) requested over UART\n",
			val_size);

	rx_tmp = kmalloc(val_size, GFP_KERNEL);
	if (!rx_tmp)
		return -ENOMEM;

	addr = mfrc522_encode_reg(*addrp) | MFRC522_UART_READ_FLAG;

	mutex_lock(&uart->lock);

	reinit_completion(&uart->rx_complete);
	spin_lock_irqsave(&uart->rx_lock, flags);
	uart->rx_buf = rx_tmp;
	uart->rx_expected = val_size;
	uart->rx_count = 0;
	spin_unlock_irqrestore(&uart->rx_lock, flags);

	ret = mfrc522_uart_send(uart, &addr, sizeof(addr));
	if (ret)
		goto out_clear_state;

	timeout = wait_for_completion_timeout(&uart->rx_complete,
			msecs_to_jiffies(MFRC522_UART_TIMEOUT_MS));
	if (!timeout) {
		ret = -ETIMEDOUT;
		goto out_clear_state;
	}

	spin_lock_irqsave(&uart->rx_lock, flags);
	if (uart->rx_count != val_size)
		ret = -EIO;
	else
		memcpy(val, rx_tmp, val_size);
	uart->rx_buf = NULL;
	uart->rx_expected = 0;
	uart->rx_count = 0;
	spin_unlock_irqrestore(&uart->rx_lock, flags);

out_unlock:
	mutex_unlock(&uart->lock);
	kfree(rx_tmp);
	return ret;

out_clear_state:
	spin_lock_irqsave(&uart->rx_lock, flags);
	uart->rx_buf = NULL;
	uart->rx_expected = 0;
	uart->rx_count = 0;
	spin_unlock_irqrestore(&uart->rx_lock, flags);
	goto out_unlock;
}

static const struct regmap_bus mfrc522_uart_regmap_bus = {
	.write = mfrc522_uart_regmap_write,
	.read  = mfrc522_uart_regmap_read,
};

static const struct regmap_config mfrc522_uart_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MFRC522_REG_MASK,
	.read_flag_mask = MFRC522_UART_READ_FLAG,
};

static int mfrc522_uart_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct mfrc522_uart *uart;
	struct regmap *regmap;
	unsigned int baud = 9600;
	int ret;

	uart = devm_kzalloc(dev, sizeof(*uart), GFP_KERNEL);
	if (!uart)
		return -ENOMEM;

	mutex_init(&uart->lock);
	spin_lock_init(&uart->rx_lock);
	init_completion(&uart->rx_complete);
	uart->serdev = serdev;

	serdev_device_set_drvdata(serdev, uart);
	serdev_device_set_client_ops(serdev, &mfrc522_uart_client_ops);

	of_property_read_u32(dev->of_node, "current-speed", &baud);

	ret = devm_serdev_device_open(dev, serdev);
	if (ret)
		return ret;

	serdev_device_set_baudrate(serdev, baud);
	serdev_device_set_flow_control(serdev, 0);
	ret = serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
	if (ret && ret != -EOPNOTSUPP)
		dev_warn(dev, "failed to set parity: %d\n", ret);

	regmap = devm_regmap_init(dev, &mfrc522_uart_regmap_bus, uart,
			      &mfrc522_uart_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(dev, "Failed to init regmap\n");
		return PTR_ERR(regmap);
	}

	return mfrc522_core_probe(dev, regmap);
}

static void mfrc522_uart_remove(struct serdev_device *serdev)
{
	struct mfrc522_uart *uart = serdev_device_get_drvdata(serdev);
	unsigned long flags;

	spin_lock_irqsave(&uart->rx_lock, flags);
	uart->rx_buf = NULL;
	uart->rx_expected = 0;
	uart->rx_count = 0;
	spin_unlock_irqrestore(&uart->rx_lock, flags);

	mfrc522_core_remove(&serdev->dev);
}

static const struct of_device_id mfrc522_uart_of_match[] = {
	{ .compatible = "nxp,mfrc522-uart", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mfrc522_uart_of_match);

static struct serdev_device_driver mfrc522_uart_driver = {
	.probe  = mfrc522_uart_probe,
	.remove = mfrc522_uart_remove,
	.driver = {
		.name           = "mfrc522-uart",
		.of_match_table = mfrc522_uart_of_match,
	},
};

module_serdev_device_driver(mfrc522_uart_driver);

MODULE_DESCRIPTION("NXP MFRC522 NFC Reader UART driver");
MODULE_AUTHOR("Daniel R. Slater <slaterd314@gmail.com>");
MODULE_LICENSE("GPL");

