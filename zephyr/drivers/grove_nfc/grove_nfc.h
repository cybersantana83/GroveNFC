/*
 * grove_nfc.h — Public API for the Grove NFC I2C driver (Zephyr port)
 *
 * Register map mirrors GroveNFC.h from the original Arduino project.
 * All multi-byte values on the bus are little-endian (lo byte first).
 */
#ifndef GROVE_NFC_H_
#define GROVE_NFC_H_

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── I2C addresses ─────────────────────────────────────────────────────── */
#define GROVE_NFC_I2C_ADDR       0x48u   /* Grove NFC module                */
#define GROVE_NFC_UNIT_I2C_ADDR  0x50u   /* M5Stack NFC Unit (ST25R3916)    */

/* ── System registers (16-bit addr, 16-bit little-endian value) ────────── */
#define GNFC_REG_HW_VER       0x0000u
#define GNFC_REG_FW_VER       0x0002u
#define GNFC_REG_MODE         0x0004u
#define GNFC_REG_TAG_ADDR     0x0006u
#define GNFC_REG_RF_CFG       0x0008u
#define GNFC_REG_FWI          0x000Au
#define GNFC_REG_TX_CRC_EN    0x000Cu
#define GNFC_REG_RX_CRC_EN    0x000Eu
#define GNFC_REG_NFC_STATUS   0x0010u
#define GNFC_REG_RX_LEN       0x0012u

/* ── Misc registers (16-bit addr, 8-bit value) ─────────────────────────── */
#define GNFC_REG_RF_ON        0x0020u
#define GNFC_REG_TX_LAST_BIT  0x0021u
#define GNFC_REG_THRU         0x0022u
#define GNFC_REG_EGT          0x0023u
#define GNFC_REG_SLOT         0x0024u
#define GNFC_REG_EP_WRITE     0x0029u

/* ── Data / EEPROM registers ────────────────────────────────────────────── */
#define GNFC_REG_DATA         0x0100u
#define GNFC_REG_EEPROM       0x1000u

/* ── Mode register bit fields ───────────────────────────────────────────── */
#define GNFC_MODE_DEFAULT      0x0000u
#define GNFC_MODE_READER       0x0100u
#define GNFC_MODE_TAG          0x0200u
#define GNFC_MODE_TAG_NONE     0x0000u
#define GNFC_MODE_TAG_NTAG213  0x0001u
#define GNFC_MODE_TAG_NTAG215  0x0002u
#define GNFC_MODE_TAG_NTAG216  0x0003u
#define GNFC_MODE_TAG_MF1_1K   0x0004u
#define GNFC_MODE_TAG_ISLI     0x000Cu
#define GNFC_MODE_TAG_CHINA_II 0x0020u

/* ── RF config ──────────────────────────────────────────────────────────── */
#define GNFC_RFCFG_RD_14A  0x0100u
#define GNFC_RFCFG_RD_14B  0x0500u
#define GNFC_RFCFG_RD_FLC  0x0900u
#define GNFC_RFCFG_RD_15   0x0B00u
#define GNFC_RFCFG_TAG_14A 0x0001u
#define GNFC_RFCFG_TAG_14B 0x0005u
#define GNFC_RFCFG_TAG_FLC 0x0009u
#define GNFC_RFCFG_TAG_15  0x000Bu

/* ── CRC enables ────────────────────────────────────────────────────────── */
#define GNFC_CRC_DISABLE   0x0000u
#define GNFC_CRC_14A       0x0001u
#define GNFC_CRC_14B       0x0002u
#define GNFC_CRC_FLC       0x0004u
#define GNFC_CRC_15        0x0008u

/* ── NFC status bits ────────────────────────────────────────────────────── */
#define GNFC_STATUS_RECV_DONE    0x0001u
#define GNFC_STATUS_RECV_BITERR  0x1000u
#define GNFC_STATUS_RECV_CRCERR  0x2000u
#define GNFC_STATUS_RECV_TIMEOUT 0x4000u

/* ── Misc register values ───────────────────────────────────────────────── */
#define GNFC_RF_OFF   0x00u
#define GNFC_RF_ON    0x01u
#define GNFC_THRU_OFF 0x00u
#define GNFC_EGT_6    0x06u
#define GNFC_SLOT_0   0x00u
#define GNFC_EP_WRITE 0x01u
#define GNFC_TLB_0    0x00u
#define GNFC_TLB_7    0x07u

/* ── EEPROM write settle time (ms) ─────────────────────────────────────── */
#define GNFC_EEPROM_WRITE_MS_SHORT 250u
#define GNFC_EEPROM_WRITE_MS_MF1K  500u

/**
 * @brief Write a 16-bit system register (little-endian on the wire).
 * Wire format: [addr_hi, addr_lo, val_lo, val_hi]
 */
int grove_nfc_write_sys_reg(const struct device *dev, uint16_t reg, uint16_t val);

/**
 * @brief Read a 16-bit system register (little-endian on the wire).
 * Wire format: write [addr_hi, addr_lo], read [val_lo, val_hi]
 */
int grove_nfc_read_sys_reg(const struct device *dev, uint16_t reg, uint16_t *val);

/**
 * @brief Write an 8-bit misc register.
 * Wire format: [addr_hi, addr_lo, val]
 */
int grove_nfc_write_misc_reg(const struct device *dev, uint16_t reg, uint8_t val);

/**
 * @brief Write arbitrary data starting at a register address.
 * Wire format: [addr_hi, addr_lo, data[0], data[1], ...]
 */
int grove_nfc_write_data(const struct device *dev, uint16_t reg,
                         const uint8_t *data, uint16_t len);

/**
 * @brief Read arbitrary data starting at a register address.
 * Wire format: write [addr_hi, addr_lo], read data[0..len-1]
 */
int grove_nfc_read_data(const struct device *dev, uint16_t reg,
                        uint8_t *data, uint16_t len);

/**
 * @brief RF transceive: write cmd to data reg, poll status, read response.
 * @param wait_ms  Maximum time to wait for RECV_DONE (milliseconds)
 * @param out_len  In: buffer size. Out: number of bytes received.
 * @return 0 on success (RECV_DONE set and data read), -errno otherwise.
 */
int grove_nfc_txrx(const struct device *dev,
                   const uint8_t *cmd, uint8_t cmd_len,
                   uint8_t *out, uint16_t *out_len,
                   uint16_t wait_ms);

/**
 * @brief Turn RF field on or off.
 */
int grove_nfc_set_rf(const struct device *dev, bool on);

/**
 * @brief Probe that the device responds on I2C (ping).
 */
bool grove_nfc_ping(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* GROVE_NFC_H_ */
