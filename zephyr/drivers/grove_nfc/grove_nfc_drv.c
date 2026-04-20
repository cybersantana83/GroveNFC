/*
 * grove_nfc_drv.c — Zephyr driver for the Seeed Grove NFC module (I2C 0x48)
 *
 * The module exposes a simple register-mapped I2C interface:
 *   System regs  (0x0000-0x001E): 2-byte address, 2-byte little-endian value
 *   Misc regs    (0x0020-0x0029): 2-byte address, 1-byte value
 *   Data reg     (0x0100):        2-byte address, N bytes payload
 *   EEPROM reg   (0x1000):        2-byte address, N bytes payload
 *
 * Translated from the Arduino GroveNFC.cpp I2C routines.
 */

#define DT_DRV_COMPAT seeed_grove_nfc

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include "grove_nfc.h"

LOG_MODULE_REGISTER(grove_nfc, CONFIG_I2C_LOG_LEVEL);

struct grove_nfc_data {
    bool initialised;
};

struct grove_nfc_config {
    struct i2c_dt_spec i2c;
};

/* ── Helpers ────────────────────────────────────────────────────────────── */

/*
 * Build the 2-byte big-endian register address into a small stack buffer.
 * addr_buf must point to at least 2 bytes.
 */
static inline void encode_addr(uint8_t *addr_buf, uint16_t reg)
{
    addr_buf[0] = (uint8_t)(reg >> 8);
    addr_buf[1] = (uint8_t)(reg & 0xFF);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int grove_nfc_write_sys_reg(const struct device *dev, uint16_t reg, uint16_t val)
{
    const struct grove_nfc_config *cfg = dev->config;
    /* Wire: [addr_hi, addr_lo, val_lo, val_hi]  (value is little-endian) */
    uint8_t buf[4] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFF),
        (uint8_t)(val & 0xFF),
        (uint8_t)(val >> 8),
    };
    return i2c_write_dt(&cfg->i2c, buf, sizeof(buf));
}

int grove_nfc_read_sys_reg(const struct device *dev, uint16_t reg, uint16_t *val)
{
    const struct grove_nfc_config *cfg = dev->config;
    uint8_t addr[2];
    uint8_t resp[2];
    int rc;

    encode_addr(addr, reg);
    rc = i2c_write_read_dt(&cfg->i2c, addr, sizeof(addr), resp, sizeof(resp));
    if (rc == 0) {
        /* Response is little-endian: [lo, hi] */
        *val = (uint16_t)resp[0] | ((uint16_t)resp[1] << 8);
    }
    return rc;
}

int grove_nfc_write_misc_reg(const struct device *dev, uint16_t reg, uint8_t val)
{
    const struct grove_nfc_config *cfg = dev->config;
    uint8_t buf[3] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFF),
        val,
    };
    return i2c_write_dt(&cfg->i2c, buf, sizeof(buf));
}

int grove_nfc_write_data(const struct device *dev, uint16_t reg,
                         const uint8_t *data, uint16_t len)
{
    const struct grove_nfc_config *cfg = dev->config;
    /* I2C write: [addr_hi, addr_lo, data...] */
    uint8_t addr[2];
    encode_addr(addr, reg);

    struct i2c_msg msgs[2] = {
        { .buf = addr,          .len = 2,   .flags = I2C_MSG_WRITE },
        { .buf = (uint8_t *)data, .len = len, .flags = I2C_MSG_WRITE | I2C_MSG_STOP },
    };
    return i2c_transfer_dt(&cfg->i2c, msgs, ARRAY_SIZE(msgs));
}

int grove_nfc_read_data(const struct device *dev, uint16_t reg,
                        uint8_t *data, uint16_t len)
{
    const struct grove_nfc_config *cfg = dev->config;
    uint8_t addr[2];
    encode_addr(addr, reg);
    return i2c_write_read_dt(&cfg->i2c, addr, sizeof(addr), data, len);
}

int grove_nfc_txrx(const struct device *dev,
                   const uint8_t *cmd, uint8_t cmd_len,
                   uint8_t *out, uint16_t *out_len,
                   uint16_t wait_ms)
{
    int rc;
    uint16_t status = 0;
    uint16_t rx_len = 0;

    /* 1. Write command bytes to the data register */
    rc = grove_nfc_write_data(dev, GNFC_REG_DATA, cmd, cmd_len);
    if (rc != 0) {
        LOG_WRN("txrx: write cmd failed %d", rc);
        return rc;
    }

    /* 2. Poll NFC status register until RECV_DONE (or error / timeout) */
    int64_t deadline = k_uptime_get() + wait_ms;
    do {
        k_msleep(2);
        rc = grove_nfc_read_sys_reg(dev, GNFC_REG_NFC_STATUS, &status);
        if (rc != 0) {
            LOG_WRN("txrx: status read failed %d", rc);
            grove_nfc_set_rf(dev, false);
            return rc;
        }
        if (status & (GNFC_STATUS_RECV_DONE | GNFC_STATUS_RECV_TIMEOUT |
                      GNFC_STATUS_RECV_CRCERR | GNFC_STATUS_RECV_BITERR)) {
            break;
        }
    } while (k_uptime_get() < deadline);

    if (!(status & GNFC_STATUS_RECV_DONE)) {
        grove_nfc_set_rf(dev, false);
        k_msleep(2);
        return -ETIMEDOUT;
    }

    /* 3. Read RX length, then data */
    rc = grove_nfc_read_sys_reg(dev, GNFC_REG_RX_LEN, &rx_len);
    if (rc != 0 || rx_len == 0 || rx_len > *out_len) {
        grove_nfc_set_rf(dev, false);
        k_msleep(2);
        return (rc != 0) ? rc : -ENOBUFS;
    }

    rc = grove_nfc_read_data(dev, GNFC_REG_DATA, out, rx_len);
    if (rc != 0) {
        grove_nfc_set_rf(dev, false);
        k_msleep(2);
        return rc;
    }

    *out_len = rx_len;
    return 0;
}

int grove_nfc_set_rf(const struct device *dev, bool on)
{
    return grove_nfc_write_misc_reg(dev, GNFC_REG_RF_ON,
                                    on ? GNFC_RF_ON : GNFC_RF_OFF);
}

bool grove_nfc_ping(const struct device *dev)
{
    const struct grove_nfc_config *cfg = dev->config;
    return i2c_reg_read_byte_dt(&cfg->i2c, 0x00, &(uint8_t){0}) == 0;
}

/* ── Driver init ────────────────────────────────────────────────────────── */

static int grove_nfc_init(const struct device *dev)
{
    const struct grove_nfc_config *cfg = dev->config;

    if (!i2c_is_ready_dt(&cfg->i2c)) {
        LOG_ERR("I2C bus %s not ready", cfg->i2c.bus->name);
        return -ENODEV;
    }

    LOG_INF("Grove NFC driver initialised on I2C %s addr=0x%02x",
            cfg->i2c.bus->name, cfg->i2c.addr);
    return 0;
}

/* ── Device instance macro ─────────────────────────────────────────────── */

#define GROVE_NFC_INIT(n)                                                   \
    static struct grove_nfc_data grove_nfc_data_##n;                        \
    static const struct grove_nfc_config grove_nfc_cfg_##n = {             \
        .i2c = I2C_DT_SPEC_INST_GET(n),                                    \
    };                                                                      \
    DEVICE_DT_INST_DEFINE(n, grove_nfc_init, NULL,                         \
                          &grove_nfc_data_##n, &grove_nfc_cfg_##n,         \
                          POST_KERNEL, CONFIG_I2C_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(GROVE_NFC_INIT)
