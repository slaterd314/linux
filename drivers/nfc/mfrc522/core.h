/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MFRC522_CORE_H__
#define __MFRC522_CORE_H__

#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/types.h>

#include "mfrc522_regs.h"

static inline bool mfrc522_reg_valid(unsigned int reg)
{
	return reg <= MFRC522_REG_MASK;
}

static inline u8 mfrc522_encode_reg(unsigned int reg)
{
	return reg & MFRC522_REG_MASK;
}

/* Shared probe/remove helpers for bus drivers */
int mfrc522_core_probe(struct device *dev, struct regmap *regmap);
void mfrc522_core_remove(struct device *dev);
int mfrc522_hw_reset(struct device *dev);

int mfrc522_fifo_write(struct device *dev, const u8 *buf, size_t len);
int mfrc522_fifo_read(struct device *dev, u8 *buf, size_t len);
int mfrc522_fifo_flush(struct device *dev);
int mfrc522_fifo_level(struct device *dev, u8 *level);

#endif /* __MFRC522_CORE_H__ */

