// mfrc522.h
#ifndef __MFRC522_H__
#define __MFRC522_H__

#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/reset.h>
#include <linux/pm_runtime.h>
#include <linux/mutex.h>
#include "mfrc522_regs.h"

enum mfrc522_bus_type {
    MFRC522_BUS_SPI,
    MFRC522_BUS_I2C,
    MFRC522_BUS_UART,
};

struct mfrc522_hw_ops;

// Main driver private data
struct mfrc522_dev {
    struct device *dev;
    enum mfrc522_bus_type bus_type;
    const struct mfrc522_hw_ops *hw;
    void *bus_priv;          // Pointer to SPI/I2C/UART state
    struct mutex lock;       // Protects hw access and FIFO

    int irq;
    struct reset_control *reset_gpio;
    struct dentry *dbg_root; // debugfs root

    // Power management
    bool suspended;
    bool power_enabled;
    struct mutex pm_lock;
#ifdef CONFIG_PM
    struct dev_pm_domain *pm_domain;
#endif
};

struct mfrc522_hw_ops {
    int (*read_reg)(struct mfrc522_dev *mdev, u8 reg, u8 *val);
    int (*write_reg)(struct mfrc522_dev *mdev, u8 reg, u8 val);
    int (*read_fifo)(struct mfrc522_dev *mdev, u8 *buf, size_t len);
    int (*write_fifo)(struct mfrc522_dev *mdev, const u8 *buf, size_t len);
};

extern int mfrc522_register(struct mfrc522_dev *mdev);
extern void mfrc522_unregister(struct mfrc522_dev *mdev);

#endif // __MFRC522_H__
