/*
 * nfc_emulator.c — NFC tag emulation (Zephyr port).
 *
 * Grove NFC module emulation is configured by:
 *   1. Writing the tag image to EEPROM (I2cEpromReg_Addr = 0x1000).
 *   2. Writing SetTagAddr + SetMode registers.
 *   3. The module then responds autonomously to RF; no CPU intervention needed
 *      beyond keeping the I2C bus alive.
 *
 * BCC (XOR) computation for NTAG UID bytes mirrors the Arduino source.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdint.h>

#include "nfc_emulator.h"
#include "grove_nfc.h"

LOG_MODULE_REGISTER(nfc_emulator, CONFIG_LOG_DEFAULT_LEVEL);

/* ── EEPROM tag image header constants ─────────────────────────────────── */

static const uint8_t kNtag213Hdr[] = {
    0x04, 0x31, 0x1D, 0xA0, 0x01, 0x17, 0x45, 0x03,
    0x50, 0x48, 0x00, 0x00, 0xE1, 0x10, 0x12, 0x00,
    0x01, 0x03, 0xA0, 0x0C, 0x34, 0x03, 0x00, 0xFE,
};
static const uint8_t kNtag215Hdr[] = {
    0x04, 0x31, 0x1D, 0xA0, 0x01, 0x17, 0x45, 0x03,
    0x50, 0x48, 0x00, 0x00, 0xE1, 0x10, 0x3E, 0x00,
    0x03, 0x00, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t kNtag216Hdr[] = {
    0x04, 0x31, 0x1D, 0xA0, 0x01, 0x17, 0x45, 0x03,
    0x50, 0x48, 0x00, 0x00, 0xE1, 0x10, 0x6D, 0x00,
    0x03, 0x00, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t kMf1kHdr[] = {
    0x64, 0x5E, 0xE5, 0x6A, 0xB5, 0x08, 0x04, 0x00,
    0x04, 0x48, 0xE5, 0x87, 0x7D, 0x91, 0xD9, 0x90,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07,
    0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
static const uint8_t kMf1kSector[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07,
    0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
static const uint8_t kISO15Hdr[] = {
    0xC1, 0xC6, 0x02, 0xB9, 0x50, 0x00, 0x07, 0xE0,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Tag EEPROM base addresses */
#define TAG_ADDR_NTAG213 0x0000u
#define TAG_ADDR_NTAG215 0x1000u
#define TAG_ADDR_NTAG216 0x2000u
#define TAG_ADDR_MF1K    0x3000u
#define TAG_ADDR_ISO15   0x7000u
#define TAG_ADDR_14B     0x0000u

static bool s_active = false;

/* ── Helpers ────────────────────────────────────────────────────────────── */

static uint8_t apply_slot_nibble(uint8_t val, uint8_t slot)
{
    return (uint8_t)((val & 0xF0u) | (((val & 0x0Fu) + (slot & 0x0Fu)) & 0x0Fu));
}

static void update_ntag_bcc(uint8_t *hdr, size_t len)
{
    if (len < 9) return;
    hdr[3] = 0x88u ^ hdr[0] ^ hdr[1] ^ hdr[2]; /* BCC0 */
    hdr[8] = hdr[4] ^ hdr[5] ^ hdr[6] ^ hdr[7]; /* BCC1 */
}

static bool write_ntag_image(const struct device *dev,
                              const uint8_t *hdr, size_t hdr_len,
                              uint16_t tag_addr, uint16_t mode_tag)
{
    uint8_t buf[24];
    memcpy(buf, hdr, hdr_len);

    grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                            GNFC_MODE_DEFAULT | GNFC_MODE_TAG_NONE);
    k_msleep(2);
    grove_nfc_write_sys_reg(dev, GNFC_REG_TAG_ADDR, tag_addr);
    k_msleep(2);
    grove_nfc_write_data(dev, GNFC_REG_EEPROM, buf, (uint16_t)hdr_len);
    k_msleep(2);
    grove_nfc_write_misc_reg(dev, GNFC_REG_EP_WRITE, GNFC_EP_WRITE);
    k_msleep(GNFC_EEPROM_WRITE_MS_SHORT);

    grove_nfc_set_rf(dev, false);
    grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                            GNFC_MODE_DEFAULT | GNFC_MODE_TAG_NONE);
    k_msleep(5);
    grove_nfc_write_sys_reg(dev, GNFC_REG_TAG_ADDR, tag_addr);
    k_msleep(5);
    return grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                                   GNFC_MODE_DEFAULT | mode_tag) == 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

bool nfc_emulator_start(const struct device *dev,
                        emu_type_t type, uint8_t slot)
{
    s_active = false;

    switch (type) {
    case EMU_N213: {
        uint8_t hdr[sizeof(kNtag213Hdr)];
        memcpy(hdr, kNtag213Hdr, sizeof(hdr));
        hdr[7] = apply_slot_nibble(hdr[7], slot);
        update_ntag_bcc(hdr, sizeof(hdr));
        s_active = write_ntag_image(dev, hdr, sizeof(hdr),
                                    TAG_ADDR_NTAG213, GNFC_MODE_TAG | GNFC_MODE_TAG_NTAG213);
        break;
    }
    case EMU_N215: {
        uint8_t hdr[sizeof(kNtag215Hdr)];
        memcpy(hdr, kNtag215Hdr, sizeof(hdr));
        hdr[7] = apply_slot_nibble(hdr[7], slot);
        update_ntag_bcc(hdr, sizeof(hdr));
        s_active = write_ntag_image(dev, hdr, sizeof(hdr),
                                    TAG_ADDR_NTAG215, GNFC_MODE_TAG | GNFC_MODE_TAG_NTAG215);
        break;
    }
    case EMU_N216: {
        uint8_t hdr[sizeof(kNtag216Hdr)];
        memcpy(hdr, kNtag216Hdr, sizeof(hdr));
        hdr[7] = apply_slot_nibble(hdr[7], slot);
        update_ntag_bcc(hdr, sizeof(hdr));
        s_active = write_ntag_image(dev, hdr, sizeof(hdr),
                                    TAG_ADDR_NTAG216, GNFC_MODE_TAG | GNFC_MODE_TAG_NTAG216);
        break;
    }
    case EMU_MF1K: {
        /* Write sector 0 header */
        uint8_t hdr[sizeof(kMf1kHdr)];
        memcpy(hdr, kMf1kHdr, sizeof(hdr));
        hdr[3] = apply_slot_nibble(hdr[3], slot);
        hdr[4] = hdr[0] ^ hdr[1] ^ hdr[2] ^ hdr[3]; /* BCC */

        grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                                GNFC_MODE_DEFAULT | GNFC_MODE_TAG_NONE);
        k_msleep(2);
        grove_nfc_write_sys_reg(dev, GNFC_REG_TAG_ADDR, TAG_ADDR_MF1K);
        k_msleep(2);
        grove_nfc_write_data(dev, GNFC_REG_EEPROM, hdr, sizeof(hdr));
        k_msleep(2);
        /* Write sectors 1-15 */
        for (uint16_t sec = 1; sec < 16; ++sec) {
            grove_nfc_write_data(dev, GNFC_REG_EEPROM + (sec << 6),
                                 kMf1kSector, sizeof(kMf1kSector));
            k_msleep(2);
        }
        grove_nfc_write_misc_reg(dev, GNFC_REG_EP_WRITE, GNFC_EP_WRITE);
        k_msleep(GNFC_EEPROM_WRITE_MS_MF1K);

        grove_nfc_set_rf(dev, false);
        grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                                GNFC_MODE_DEFAULT | GNFC_MODE_TAG_NONE);
        k_msleep(5);
        grove_nfc_write_sys_reg(dev, GNFC_REG_TAG_ADDR, TAG_ADDR_MF1K);
        k_msleep(5);
        s_active = grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                       GNFC_MODE_DEFAULT | GNFC_MODE_TAG_MF1_1K) == 0;
        break;
    }
    case EMU_ISO14B: {
        /* China-II (ISO14443B) tag emulation */
        grove_nfc_set_rf(dev, false);
        grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                                GNFC_MODE_DEFAULT | GNFC_MODE_TAG_NONE);
        k_msleep(5);
        grove_nfc_write_sys_reg(dev, GNFC_REG_TAG_ADDR, TAG_ADDR_14B);
        k_msleep(5);
        s_active = grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                       GNFC_MODE_DEFAULT | GNFC_MODE_TAG_CHINA_II) == 0;
        break;
    }
    case EMU_ISO15: {
        uint8_t hdr[sizeof(kISO15Hdr)];
        memcpy(hdr, kISO15Hdr, sizeof(hdr));
        hdr[0] = apply_slot_nibble(hdr[0], slot);

        grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                                GNFC_MODE_DEFAULT | GNFC_MODE_TAG_NONE);
        k_msleep(2);
        grove_nfc_write_sys_reg(dev, GNFC_REG_TAG_ADDR, TAG_ADDR_ISO15);
        k_msleep(2);
        grove_nfc_write_data(dev, GNFC_REG_EEPROM, hdr, sizeof(hdr));
        k_msleep(2);
        grove_nfc_write_misc_reg(dev, GNFC_REG_EP_WRITE, GNFC_EP_WRITE);
        k_msleep(GNFC_EEPROM_WRITE_MS_SHORT);

        grove_nfc_set_rf(dev, false);
        grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                                GNFC_MODE_DEFAULT | GNFC_MODE_TAG_NONE);
        k_msleep(5);
        grove_nfc_write_sys_reg(dev, GNFC_REG_TAG_ADDR, TAG_ADDR_ISO15);
        k_msleep(5);
        s_active = grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                       GNFC_MODE_DEFAULT | GNFC_MODE_TAG_ISLI) == 0;
        break;
    }
    case EMU_FELICA:
        /* FeliCa emulation not supported by the Grove NFC module firmware. */
        LOG_WRN("FeliCa emulation not supported on Grove NFC hardware");
        s_active = false;
        break;
    default:
        s_active = false;
        break;
    }

    if (s_active) {
        LOG_INF("Emulation started: type=%d slot=%d", type, slot);
    }
    return s_active;
}

void nfc_emulator_stop(const struct device *dev)
{
    if (!s_active) return;
    grove_nfc_set_rf(dev, false);
    grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                            GNFC_MODE_DEFAULT | GNFC_MODE_TAG_NONE);
    s_active = false;
    LOG_INF("Emulation stopped");
}

void nfc_emulator_tick(const struct device *dev)
{
    /*
     * The Grove NFC module handles tag emulation in hardware once configured;
     * no CPU-side state machine tick is required.
     * A future version could poll the status register here for "tag selected"
     * notifications if the firmware supports them.
     */
    ARG_UNUSED(dev);
}

bool nfc_emulator_is_active(void)
{
    return s_active;
}
