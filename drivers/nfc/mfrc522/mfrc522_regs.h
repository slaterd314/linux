// mfrc522_regs.h
#ifndef __MFRC522_REGS_H__
#define __MFRC522_REGS_H__

// Core register helpers
#define MFRC522_REG_MASK              0x3F
#define MFRC522_FIFO_SIZE             64U
#define MFRC522_FIFO_FLUSH            0x80
#define MFRC522_FIFO_LEVEL_MASK       0x7F
#define MFRC522_IRQ_STATUS_MASK       0x7F

// MFRC522 Register Map (see datasheet Table 6.1 / Table 31)
// Page 0: Command and Status
#define MFRC522_REG_COMMAND           0x01
#define MFRC522_REG_COMMIEN           0x02
#define MFRC522_REG_DIVLEN            0x03
#define MFRC522_REG_COMMIRQ           0x04
#define MFRC522_REG_DIVIRQ            0x05
#define MFRC522_REG_ERROR             0x06
#define MFRC522_REG_STATUS1           0x07
#define MFRC522_REG_STATUS2           0x08
#define MFRC522_REG_FIFO_DATA         0x09
#define MFRC522_REG_FIFO_LEVEL        0x0A
#define MFRC522_REG_WATER_LEVEL       0x0B
#define MFRC522_REG_CONTROL           0x0C
#define MFRC522_REG_BIT_FRAMING       0x0D
#define MFRC522_REG_COLL              0x0E

// Page 1: Command and Status
#define MFRC522_REG_MODE              0x11
#define MFRC522_REG_TX_MODE           0x12
#define MFRC522_REG_RX_MODE           0x13
#define MFRC522_REG_TX_CONTROL        0x14
#define MFRC522_REG_TX_ASK            0x15
#define MFRC522_REG_TX_SEL            0x16
#define MFRC522_REG_RX_SEL            0x17
#define MFRC522_REG_RX_THRESHOLD      0x18
#define MFRC522_REG_DEMOD             0x19
#define MFRC522_REG_MFTX              0x1C
#define MFRC522_REG_SERIALSPEED       0x1F

// Page 2,3: Configuration, Test etc
#define MFRC522_REG_CRC_RESULT_H      0x21
#define MFRC522_REG_CRC_RESULT_L      0x22
#define MFRC522_REG_MOD_WIDTH         0x24
#define MFRC522_REG_RF_CFG            0x26
#define MFRC522_REG_GS_N              0x27
#define MFRC522_REG_CW_GS_P           0x28
#define MFRC522_REG_MOD_GS_P          0x29
#define MFRC522_REG_T_MODE            0x2A
#define MFRC522_REG_T_PRESCALER       0x2B
#define MFRC522_REG_T_RELOAD_H        0x2C
#define MFRC522_REG_T_RELOAD_L        0x2D
#define MFRC522_REG_VERSION           0x37
#define MFRC522_REG_AUTO_TEST         0x36

// MFRC522 Commands
#define MFRC522_CMD_IDLE              0x00
#define MFRC522_CMD_MEM               0x01
#define MFRC522_CMD_GENRANDOMID       0x02
#define MFRC522_CMD_CALCCRC           0x03
#define MFRC522_CMD_TRANSMIT          0x04
#define MFRC522_CMD_RECEIVE           0x08
#define MFRC522_CMD_TRANSCEIVE        0x0C
#define MFRC522_CMD_MFAUTHENT         0x0E
#define MFRC522_CMD_SOFTRESET         0x0F

#endif // __MFRC522_REGS_H__
