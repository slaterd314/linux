// SPDX-License-Identifier: GPL-2.0-only
//
// KUnit tests for the MFRC522 core helpers.
//

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "core.h"

#define MFRC522_TEST_REG_COUNT	(MFRC522_REG_MASK + 1)

struct mfrc522_test_hw {
	u8 regs[MFRC522_TEST_REG_COUNT];
	u8 fifo[MFRC522_FIFO_SIZE];
	unsigned int fifo_len;
};

struct mfrc522_core_test_context {
	struct device *dev;
	struct regmap *regmap;
	struct mfrc522_test_hw *hw;
};

static unsigned int mfrc522_test_get_reg(const void *reg_buf, size_t reg_size)
{
	const u8 *buf = reg_buf;
	unsigned int reg = 0;
	unsigned int i;

	for (i = 0; i < reg_size; i++)
		reg = (reg << 8) | buf[i];

	return reg & MFRC522_REG_MASK;
}

static void mfrc522_test_fifo_reset(struct mfrc522_test_hw *hw)
{
	hw->fifo_len = 0;
	memset(hw->fifo, 0, sizeof(hw->fifo));
	hw->regs[MFRC522_REG_FIFO_LEVEL] = 0;
}

static void mfrc522_test_fifo_push(struct mfrc522_test_hw *hw, u8 value)
{
	if (hw->fifo_len >= MFRC522_FIFO_SIZE)
		return;

	hw->fifo[hw->fifo_len++] = value;
	hw->regs[MFRC522_REG_FIFO_LEVEL] = hw->fifo_len & MFRC522_FIFO_LEVEL_MASK;
}

static u8 mfrc522_test_fifo_pop(struct mfrc522_test_hw *hw)
{
	u8 value = 0;

	if (hw->fifo_len) {
		value = hw->fifo[0];
		memmove(&hw->fifo[0], &hw->fifo[1], hw->fifo_len - 1);
		hw->fifo_len--;
	}

	hw->regs[MFRC522_REG_FIFO_LEVEL] = hw->fifo_len & MFRC522_FIFO_LEVEL_MASK;

	return value;
}

static void mfrc522_test_write_one(struct mfrc522_test_hw *hw,
				      unsigned int reg, u8 val)
{
	switch (reg) {
	case MFRC522_REG_FIFO_DATA:
		mfrc522_test_fifo_push(hw, val);
		break;
	case MFRC522_REG_FIFO_LEVEL:
		if (val & MFRC522_FIFO_FLUSH)
			mfrc522_test_fifo_reset(hw);
		break;
	case MFRC522_REG_COMMIRQ:
	case MFRC522_REG_DIVIRQ:
		hw->regs[reg] &= ~(val & MFRC522_IRQ_STATUS_MASK);
		break;
	default:
		hw->regs[reg] = val;
		break;
	}
}

static u8 mfrc522_test_read_one(struct mfrc522_test_hw *hw, unsigned int reg)
{
	switch (reg) {
	case MFRC522_REG_FIFO_DATA:
		return mfrc522_test_fifo_pop(hw);
	default:
		return hw->regs[reg];
	}
}

static int mfrc522_test_bus_write(void *context, const void *val_buf,
			       size_t val_size)
{
	struct mfrc522_test_hw *hw = context;
	const u8 *buf = val_buf;
	unsigned int reg;
	unsigned int i;

	if (val_size < 2)
		return -EINVAL;

	reg = buf[0];

	for (i = 1; i < val_size; i++)
		mfrc522_test_write_one(hw, reg, buf[i]);

	return 0;
}

static int mfrc522_test_bus_read(void *context, const void *reg_buf,
		      size_t reg_size, void *val_buf, size_t val_size)
{
	struct mfrc522_test_hw *hw = context;
	u8 *out = val_buf;
	unsigned int base;
	unsigned int i;

	if (!val_size || !reg_size)
		return -EINVAL;

	base = mfrc522_test_get_reg(reg_buf, reg_size);

	for (i = 0; i < val_size; i++)
		out[i] = mfrc522_test_read_one(hw, base);

	return 0;
}

static const struct regmap_bus mfrc522_test_bus = {
	.write = mfrc522_test_bus_write,
	.read = mfrc522_test_bus_read,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

static const struct regmap_config mfrc522_test_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MFRC522_REG_MASK,
	.cache_type = REGCACHE_NONE,
};

static int mfrc522_core_test_init(struct kunit *test)
{
	struct mfrc522_core_test_context *ctx;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->hw = kunit_kzalloc(test, sizeof(*ctx->hw), GFP_KERNEL);
	if (!ctx->hw)
		return -ENOMEM;

	mfrc522_test_fifo_reset(ctx->hw);

	ctx->dev = kunit_device_register(test, "mfrc522-core-test");
	if (IS_ERR(ctx->dev))
		return PTR_ERR(ctx->dev);

	ctx->regmap = devm_regmap_init(ctx->dev, &mfrc522_test_bus,
					 ctx->hw, &mfrc522_test_regmap_config);
	if (IS_ERR(ctx->regmap))
		return PTR_ERR(ctx->regmap);

	ret = mfrc522_core_probe(ctx->dev, ctx->regmap, 0);
	if (ret)
		return ret;

	test->priv = ctx;

	return 0;
}

static void mfrc522_core_test_exit(struct kunit *test)
{
	struct mfrc522_core_test_context *ctx = test->priv;

	if (!ctx)
		return;

	mfrc522_core_remove(ctx->dev);
	kunit_device_unregister(test, ctx->dev);
}

static void mfrc522_test_fifo_roundtrip(struct kunit *test)
{
	struct mfrc522_core_test_context *ctx = test->priv;
	u8 tx[] = { 0x11, 0x22, 0x33, 0x44 };
	u8 rx[ARRAY_SIZE(tx)] = { 0 };
	u8 level = 0;
	int ret;

	ret = mfrc522_fifo_write(ctx->dev, tx, ARRAY_SIZE(tx));
	KUNIT_ASSERT_EQ(test, ret, (int)ARRAY_SIZE(tx));

	ret = mfrc522_fifo_level(ctx->dev, &level);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, (int)level, (int)ARRAY_SIZE(tx));

	ret = mfrc522_fifo_read(ctx->dev, rx, ARRAY_SIZE(rx));
	KUNIT_ASSERT_EQ(test, ret, (int)ARRAY_SIZE(tx));
	KUNIT_EXPECT_MEMEQ(test, rx, tx, ARRAY_SIZE(tx));

	ret = mfrc522_fifo_level(ctx->dev, &level);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, level, 0);
}

static void mfrc522_test_fifo_flush(struct kunit *test)
{
	struct mfrc522_core_test_context *ctx = test->priv;
	u8 tx[MFRC522_FIFO_SIZE] = { 0 };
	u8 level = 0;
	int ret;

	memset(tx, 0x5a, sizeof(tx));

	ret = mfrc522_fifo_write(ctx->dev, tx, sizeof(tx));
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(tx));

	ret = mfrc522_fifo_flush(ctx->dev);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = mfrc522_fifo_level(ctx->dev, &level);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, level, 0);
}

static void mfrc522_test_fifo_invalid(struct kunit *test)
{
	struct mfrc522_core_test_context *ctx = test->priv;
	u8 buffer[MFRC522_FIFO_SIZE + 1];
	u8 level = 0;
	int ret;

	ret = mfrc522_fifo_write(ctx->dev, NULL, 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = mfrc522_fifo_write(ctx->dev, buffer, sizeof(buffer));
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = mfrc522_fifo_read(ctx->dev, NULL, 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = mfrc522_fifo_read(ctx->dev, buffer, sizeof(buffer));
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = mfrc522_fifo_level(ctx->dev, NULL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = mfrc522_fifo_level(ctx->dev, &level);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, level, 0);
}

static void mfrc522_test_irq_enable_wait(struct kunit *test)
{
	struct mfrc522_core_test_context *ctx = test->priv;
	u8 comm_status = 0;
	u8 div_status = 0;
	unsigned long timeout = msecs_to_jiffies(10);
	int ret;
	const u8 comm_mask = BIT(0);
	const u8 div_mask = BIT(1);

	ret = mfrc522_test_set_irq(ctx->dev, 1);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = mfrc522_irq_enable(ctx->dev, comm_mask, div_mask);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx->hw->regs[MFRC522_REG_COMMIEN], comm_mask);
	KUNIT_EXPECT_EQ(test, ctx->hw->regs[MFRC522_REG_DIVLEN], div_mask);

	ret = mfrc522_test_set_irq_pending(ctx->dev, comm_mask, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = mfrc522_irq_wait(ctx->dev, comm_mask, 0,
			      timeout, &comm_status, &div_status);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, comm_status, comm_mask);
	KUNIT_EXPECT_EQ(test, div_status, 0);

	ret = mfrc522_irq_disable(ctx->dev);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

static struct kunit_case mfrc522_core_test_cases[] = {
	KUNIT_CASE(mfrc522_test_fifo_roundtrip),
	KUNIT_CASE(mfrc522_test_fifo_flush),
	KUNIT_CASE(mfrc522_test_fifo_invalid),
	KUNIT_CASE(mfrc522_test_irq_enable_wait),
	{}
};

static struct kunit_suite mfrc522_core_test_suite = {
	.name = "mfrc522-core",
	.init = mfrc522_core_test_init,
	.exit = mfrc522_core_test_exit,
	.test_cases = mfrc522_core_test_cases,
};

kunit_test_suite(mfrc522_core_test_suite);

MODULE_DESCRIPTION("MFRC522 core KUnit tests");
MODULE_LICENSE("GPL");
