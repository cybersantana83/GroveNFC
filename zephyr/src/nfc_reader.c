/*
 * nfc_reader.c — ISO14443A/B, ISO15693, FeliCa reader (Zephyr port).
 *
 * Direct translation of the Arduino GroveNFC.cpp reader path to Zephyr I2C
 * calls via the grove_nfc driver.  All timing is replaced with k_msleep().
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

#include "nfc_reader.h"
#include "grove_nfc.h"

LOG_MODULE_REGISTER(nfc_reader, CONFIG_LOG_DEFAULT_LEVEL);

/* ── Internal helpers ───────────────────────────────────────────────────── */

static void bytes_to_hex(const uint8_t *data, size_t len,
                         char *out, size_t out_sz, bool reverse)
{
    static const char *HEX = "0123456789ABCDEF";
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 3 < out_sz; ++i) {
        if (i) out[pos++] = ':';
        uint8_t b = reverse ? data[len - 1 - i] : data[i];
        out[pos++] = HEX[(b >> 4) & 0xF];
        out[pos++] = HEX[b & 0xF];
    }
    out[pos] = '\0';
}

static void bytes_to_hex_compact(const uint8_t *data, size_t len,
                                 char *out, size_t out_sz)
{
    static const char *HEX = "0123456789ABCDEF";
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < out_sz; ++i) {
        out[pos++] = HEX[(data[i] >> 4) & 0xF];
        out[pos++] = HEX[data[i] & 0xF];
    }
    out[pos] = '\0';
}

/* Enter reader mode (common setup before any protocol scan). */
static int select_reader_common(const struct device *dev)
{
    int rc;
    grove_nfc_set_rf(dev, false);
    k_msleep(2);
    rc = grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                                 GNFC_MODE_DEFAULT | GNFC_MODE_TAG_NONE);
    if (rc) return rc;
    k_msleep(5);
    rc = grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                                 GNFC_MODE_READER | GNFC_MODE_TAG_NONE);
    if (rc) return rc;
    grove_nfc_write_misc_reg(dev, GNFC_REG_THRU,        GNFC_THRU_OFF);
    grove_nfc_write_misc_reg(dev, GNFC_REG_TX_LAST_BIT, GNFC_TLB_0);
    return 0;
}

/* ── ISO14443A ──────────────────────────────────────────────────────────── */

static bool read_iso14a(const struct device *dev, struct card_info *card)
{
    if (select_reader_common(dev) != 0) return false;

    grove_nfc_write_sys_reg(dev, GNFC_REG_RF_CFG,
                            GNFC_RFCFG_RD_14A | GNFC_RFCFG_TAG_14A);
    grove_nfc_write_sys_reg(dev, GNFC_REG_FWI, 0x0009);
    grove_nfc_write_sys_reg(dev, GNFC_REG_TX_CRC_EN, GNFC_CRC_DISABLE);
    grove_nfc_write_sys_reg(dev, GNFC_REG_RX_CRC_EN, GNFC_CRC_DISABLE);
    grove_nfc_write_misc_reg(dev, GNFC_REG_TX_LAST_BIT, GNFC_TLB_7);
    grove_nfc_set_rf(dev, true);
    k_msleep(5);

    uint8_t rx[64] = {0};
    uint16_t rx_len = sizeof(rx);

    /* REQA */
    const uint8_t reqa[] = {0x52};
    if (grove_nfc_txrx(dev, reqa, 1, rx, &rx_len, 10) != 0) return false;

    grove_nfc_write_misc_reg(dev, GNFC_REG_TX_LAST_BIT, GNFC_TLB_0);

    /* Anti-collision CL1 */
    const uint8_t ac1[] = {0x93, 0x20};
    rx_len = sizeof(rx);
    if (grove_nfc_txrx(dev, ac1, 2, rx, &rx_len, 10) != 0 || rx_len < 5)
        return false;

    uint8_t uid_cl1[4] = {rx[0], rx[1], rx[2], rx[3]};
    uint8_t bcc1 = rx[4];

    grove_nfc_write_sys_reg(dev, GNFC_REG_TX_CRC_EN, GNFC_CRC_14A);
    grove_nfc_write_sys_reg(dev, GNFC_REG_RX_CRC_EN, GNFC_CRC_14A);

    /* SELECT CL1 */
    const uint8_t sel1[] = {0x93, 0x70,
                             uid_cl1[0], uid_cl1[1], uid_cl1[2], uid_cl1[3], bcc1};
    rx_len = sizeof(rx);
    if (grove_nfc_txrx(dev, sel1, 7, rx, &rx_len, 10) != 0) return false;
    uint8_t sak = rx[0];

    uint8_t uid_buf[7];
    size_t uid_len;
    bool cascade = (uid_cl1[0] == 0x88);

    if (!cascade) {
        memcpy(uid_buf, uid_cl1, 4);
        uid_len = 4;
    } else {
        grove_nfc_write_sys_reg(dev, GNFC_REG_TX_CRC_EN, GNFC_CRC_DISABLE);
        grove_nfc_write_sys_reg(dev, GNFC_REG_RX_CRC_EN, GNFC_CRC_DISABLE);
        const uint8_t ac2[] = {0x95, 0x20};
        rx_len = sizeof(rx);
        if (grove_nfc_txrx(dev, ac2, 2, rx, &rx_len, 10) != 0 || rx_len < 5)
            return false;
        uint8_t bcc2 = rx[4];
        grove_nfc_write_sys_reg(dev, GNFC_REG_TX_CRC_EN, GNFC_CRC_14A);
        grove_nfc_write_sys_reg(dev, GNFC_REG_RX_CRC_EN, GNFC_CRC_14A);
        const uint8_t sel2[] = {0x95, 0x70, rx[0], rx[1], rx[2], rx[3], bcc2};
        rx_len = sizeof(rx);
        if (grove_nfc_txrx(dev, sel2, 7, rx, &rx_len, 10) != 0) return false;
        sak = rx[0];
        uid_buf[0] = uid_cl1[1]; uid_buf[1] = uid_cl1[2]; uid_buf[2] = uid_cl1[3];
        uid_buf[3] = sel2[2];    uid_buf[4] = sel2[3];
        uid_buf[5] = sel2[4];    uid_buf[6] = sel2[5];
        uid_len = 7;
    }

    bytes_to_hex(uid_buf, uid_len, card->uid, sizeof(card->uid), false);
    card->valid = true;

    /* Identify subtype by SAK, then GET_VERSION for SAK=0x00 */
    char sak_str[8];
    snprintf(sak_str, sizeof(sak_str), "SAK:%02X", sak);

    static const struct { uint8_t sak; const char *proto; const char *detail_sfx; } sak_map[] = {
        {0x08, "MFC1K",   "MIFARE Classic 1K"},
        {0x18, "MFC4K",   "MIFARE Classic 4K"},
        {0x09, "MFCMini", "MIFARE Classic Mini"},
        {0x10, "MFPlus2K","MIFARE Plus 2K"},
        {0x11, "MFPlus4K","MIFARE Plus 4K"},
        {0x20, "DESFire", "DESFire/JCOP"},
        {0x28, "DESFire", "DESFire/JCOP CL2"},
    };
    for (size_t i = 0; i < ARRAY_SIZE(sak_map); ++i) {
        if (sak_map[i].sak == sak) {
            strncpy(card->protocol, sak_map[i].proto, sizeof(card->protocol) - 1);
            snprintf(card->detail, sizeof(card->detail), "%s %s",
                     sak_str, sak_map[i].detail_sfx);
            return true;
        }
    }

    if (sak == 0x00) {
        /* NTAG / Ultralight: try GET_VERSION (0x60) */
        const uint8_t get_ver[] = {0x60};
        rx_len = sizeof(rx);
        if (grove_nfc_txrx(dev, get_ver, 1, rx, &rx_len, 15) == 0 && rx_len >= 8) {
            uint8_t ic  = rx[2];
            uint8_t sz  = rx[6];
            if (ic == 0x04) {
                const char *p = "NTAG";
                if      (sz == 0x0F) p = "NTAG213";
                else if (sz == 0x11) p = "NTAG215";
                else if (sz == 0x13) p = "NTAG216";
                strncpy(card->protocol, p, sizeof(card->protocol) - 1);
            } else {
                const char *p = "MFUL";
                if      (sz == 0x0B) p = "MFUL11";
                else if (sz == 0x0E) p = "MFUL21";
                strncpy(card->protocol, p, sizeof(card->protocol) - 1);
            }
            snprintf(card->detail, sizeof(card->detail),
                     "%s ic=0x%02X sz=0x%02X", sak_str, ic, sz);
            return true;
        }
        /* Fallback: NTAG203 */
        strncpy(card->protocol, "NTAG203", sizeof(card->protocol) - 1);
        snprintf(card->detail, sizeof(card->detail), "%s NTAG203", sak_str);
        return true;
    }

    strncpy(card->protocol, "ISO14443A", sizeof(card->protocol) - 1);
    snprintf(card->detail, sizeof(card->detail), "%s %zuB UID",
             sak_str, uid_len);
    return true;
}

/* ── ISO14443B ──────────────────────────────────────────────────────────── */

static bool read_iso14b_once(const struct device *dev, struct card_info *card)
{
    grove_nfc_set_rf(dev, false);
    grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                            GNFC_MODE_DEFAULT | GNFC_MODE_TAG_NONE);
    k_msleep(10);
    grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                            GNFC_MODE_READER | GNFC_MODE_TAG_NONE);
    grove_nfc_write_sys_reg(dev, GNFC_REG_RF_CFG,
                            GNFC_RFCFG_RD_14B | GNFC_RFCFG_TAG_14B);
    grove_nfc_write_sys_reg(dev, GNFC_REG_FWI,        0x0000);
    grove_nfc_write_sys_reg(dev, GNFC_REG_TX_CRC_EN,  GNFC_CRC_14B);
    grove_nfc_write_sys_reg(dev, GNFC_REG_RX_CRC_EN,  GNFC_CRC_14B);
    grove_nfc_write_misc_reg(dev, GNFC_REG_TX_LAST_BIT, GNFC_TLB_0);
    grove_nfc_write_misc_reg(dev, GNFC_REG_EGT,        GNFC_EGT_6);
    grove_nfc_write_misc_reg(dev, GNFC_REG_THRU,       GNFC_THRU_OFF);
    grove_nfc_set_rf(dev, true);
    k_msleep(10);

    uint8_t rx[128] = {0};
    uint16_t rx_len;

    /* REQB with AFI=0x00 */
    const uint8_t reqb[] = {0x05, 0x00, 0x00};
    rx_len = sizeof(rx);
    if (grove_nfc_txrx(dev, reqb, 3, rx, &rx_len, 20) != 0) {
        grove_nfc_set_rf(dev, false);
        return false;
    }

    /* ATTRIB */
    const uint8_t attrib[] = {0x1D, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x08, 0x01, 0x08};
    rx_len = sizeof(rx);
    if (grove_nfc_txrx(dev, attrib, 9, rx, &rx_len, 20) != 0) {
        grove_nfc_set_rf(dev, false);
        return false;
    }

    uint8_t uid_buf[16] = {0};
    size_t uid_len = (rx_len < sizeof(uid_buf)) ? rx_len : sizeof(uid_buf);
    memcpy(uid_buf, rx, uid_len);

    /* GET UID APDU (China ID card) */
    const uint8_t get_uid[] = {0x00, 0x36, 0x00, 0x00, 0x08};
    rx_len = sizeof(rx);
    if (grove_nfc_txrx(dev, get_uid, 5, rx, &rx_len, 30) == 0 && rx_len >= 4) {
        uid_len = (rx_len < sizeof(uid_buf)) ? rx_len : sizeof(uid_buf);
        memcpy(uid_buf, rx, uid_len);
    }

    grove_nfc_set_rf(dev, false);

    if (uid_len == 0) return false;

    strncpy(card->protocol, "ISO14443B", sizeof(card->protocol) - 1);
    bytes_to_hex(uid_buf, uid_len, card->uid, sizeof(card->uid), false);
    strncpy(card->detail, "ChinaID UID", sizeof(card->detail) - 1);
    card->valid = true;
    return true;
}

/* ── ISO15693 ────────────────────────────────────────────────────────────── */

static bool read_iso15(const struct device *dev, struct card_info *card)
{
    if (select_reader_common(dev) != 0) return false;
    grove_nfc_write_sys_reg(dev, GNFC_REG_RF_CFG,
                            GNFC_RFCFG_RD_15 | GNFC_RFCFG_TAG_15);
    grove_nfc_write_sys_reg(dev, GNFC_REG_TX_CRC_EN, GNFC_CRC_15);
    grove_nfc_write_sys_reg(dev, GNFC_REG_RX_CRC_EN, GNFC_CRC_15);
    grove_nfc_write_misc_reg(dev, GNFC_REG_TX_LAST_BIT, GNFC_TLB_0);
    grove_nfc_set_rf(dev, true);
    k_msleep(5);

    uint8_t rx[64] = {0};
    uint16_t rx_len = sizeof(rx);
    const uint8_t inv[] = {0x26, 0x01, 0x00};
    if (grove_nfc_txrx(dev, inv, 3, rx, &rx_len, 15) != 0 || rx_len < 10) {
        /* One retry with longer wait */
        grove_nfc_set_rf(dev, true);
        k_msleep(4);
        rx_len = sizeof(rx);
        if (grove_nfc_txrx(dev, inv, 3, rx, &rx_len, 100) != 0 || rx_len < 10)
            return false;
    }

    strncpy(card->protocol, "ISO15693",  sizeof(card->protocol) - 1);
    bytes_to_hex(&rx[2], 8, card->uid, sizeof(card->uid), true);
    strncpy(card->detail, "Inventory", sizeof(card->detail) - 1);
    card->valid = true;
    return true;
}

/* ── FeliCa ──────────────────────────────────────────────────────────────── */

static bool read_felica(const struct device *dev, struct card_info *card)
{
    if (select_reader_common(dev) != 0) return false;
    grove_nfc_write_sys_reg(dev, GNFC_REG_RF_CFG,
                            GNFC_RFCFG_RD_FLC | GNFC_RFCFG_TAG_FLC);
    grove_nfc_write_misc_reg(dev, GNFC_REG_SLOT,       GNFC_SLOT_0);
    grove_nfc_write_sys_reg(dev, GNFC_REG_TX_CRC_EN,   GNFC_CRC_FLC);
    grove_nfc_write_sys_reg(dev, GNFC_REG_RX_CRC_EN,   GNFC_CRC_FLC);
    grove_nfc_set_rf(dev, true);
    k_msleep(5);

    uint8_t rx[64] = {0};
    uint16_t rx_len = sizeof(rx);
    const uint8_t poll[] = {0x06, 0x00, 0xFF, 0xFF, 0x00, 0x00};
    if (grove_nfc_txrx(dev, poll, 6, rx, &rx_len, 15) != 0 || rx_len < 10)
        return false;

    strncpy(card->protocol, "FeliCa", sizeof(card->protocol) - 1);
    bytes_to_hex_compact(&rx[2], 8, card->uid, sizeof(card->uid));
    snprintf(card->detail, sizeof(card->detail),
             "IDm:%s PMm:%02X%02X", card->uid, rx[10], rx[11]);
    card->valid = true;
    return true;
}

/* ── NDEF parser (Type-2 tag) ────────────────────────────────────────────── */

static bool type2_read_block(const struct device *dev,
                              uint8_t start_page, uint8_t *out16)
{
    grove_nfc_write_sys_reg(dev, GNFC_REG_TX_CRC_EN, GNFC_CRC_14A);
    grove_nfc_write_sys_reg(dev, GNFC_REG_RX_CRC_EN, GNFC_CRC_14A);
    const uint8_t cmd[] = {0x30, start_page};
    uint16_t out_len = 16;
    return grove_nfc_txrx(dev, cmd, 2, out16, &out_len, 12) == 0 && out_len >= 16;
}

static const char *uri_prefix_lut[] = {
    "", "http://www.", "https://www.", "http://", "https://",
    "tel:", "mailto:", "ftp://anonymous:anonymous@", "ftp://ftp.",
    "ftps://", "sftp://", "smb://", "nfs://", "ftp://", "dav://",
    "news:", "telnet://", "imap:", "rtsp://", "urn:", "pop:",
    "sip:", "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://",
    "tcpobex://", "irdaobex://", "file://", "urn:epc:id:",
    "urn:epc:tag:", "urn:epc:pat:", "urn:epc:raw:", "urn:epc:", "urn:nfc:",
};

static size_t parse_ndef(const uint8_t *msg, size_t len,
                         char *out, size_t out_sz)
{
    size_t written = 0;
    size_t i = 0;
    while (i + 2 < len && written + 2 < out_sz) {
        uint8_t hdr      = msg[i++];
        bool    sr       = (hdr & 0x10) != 0;
        bool    il       = (hdr & 0x08) != 0;
        uint8_t tnf      = hdr & 0x07;
        uint8_t type_len = msg[i++];
        uint32_t pl_len;
        if (sr) { pl_len = msg[i++]; }
        else {
            if (i + 4 > len) break;
            pl_len = ((uint32_t)msg[i] << 24) | ((uint32_t)msg[i+1] << 16) |
                     ((uint32_t)msg[i+2] << 8) | msg[i+3];
            i += 4;
        }
        uint8_t id_len = il ? msg[i++] : 0;
        if (i + type_len + id_len + pl_len > len) break;
        const uint8_t *type_ptr = &msg[i];  i += type_len;
        i += id_len;
        const uint8_t *payload  = &msg[i];  i += pl_len;

        if (written > 0 && written + 2 < out_sz) out[written++] = '\n';

        if (tnf == 0x01 && type_len == 1 && type_ptr[0] == 'T' && pl_len >= 2) {
            uint8_t lang_len = payload[0] & 0x3F;
            if (pl_len > (size_t)(1 + lang_len)) {
                size_t txt_len = pl_len - 1 - lang_len;
                for (size_t k = 0; k < txt_len && written + 1 < out_sz; ++k) {
                    char c = (char)payload[1 + lang_len + k];
                    out[written++] = (c >= 0x20 && c <= 0x7E) ? c : '.';
                }
            }
        } else if (tnf == 0x01 && type_len == 1 && type_ptr[0] == 'U' && pl_len >= 1) {
            uint8_t pfx = payload[0];
            if (pfx < ARRAY_SIZE(uri_prefix_lut)) {
                const char *p = uri_prefix_lut[pfx];
                while (*p && written + 1 < out_sz) out[written++] = *p++;
            }
            for (size_t k = 1; k < pl_len && written + 1 < out_sz; ++k) {
                char c = (char)payload[k];
                out[written++] = (c >= 0x20 && c <= 0x7E) ? c : '.';
            }
        }
        if (hdr & 0x40) break; /* ME: message end */
    }
    if (written < out_sz) out[written] = '\0';
    return written;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void nfc_reader_reset(const struct device *dev)
{
    grove_nfc_set_rf(dev, false);
    k_msleep(2);
    grove_nfc_write_sys_reg(dev, GNFC_REG_MODE,
                            GNFC_MODE_DEFAULT | GNFC_MODE_TAG_NONE);
    k_msleep(5);
    grove_nfc_write_sys_reg(dev, GNFC_REG_FWI,       0x0000);
    grove_nfc_write_sys_reg(dev, GNFC_REG_TX_CRC_EN, GNFC_CRC_DISABLE);
    grove_nfc_write_sys_reg(dev, GNFC_REG_RX_CRC_EN, GNFC_CRC_DISABLE);
    grove_nfc_write_misc_reg(dev, GNFC_REG_TX_LAST_BIT, GNFC_TLB_0);
    grove_nfc_write_misc_reg(dev, GNFC_REG_THRU,        GNFC_THRU_OFF);
    grove_nfc_write_misc_reg(dev, GNFC_REG_EGT,         GNFC_EGT_6);
    grove_nfc_write_misc_reg(dev, GNFC_REG_SLOT,        GNFC_SLOT_0);
}

bool nfc_reader_recover(const struct device *dev)
{
    if (!grove_nfc_ping(dev)) return false;
    nfc_reader_reset(dev);
    return grove_nfc_ping(dev);
}

bool nfc_reader_read_any(const struct device *dev, struct card_info *card)
{
    memset(card, 0, sizeof(*card));
    if (read_iso14b_once(dev, card)) return true;
    if (read_iso14a(dev, card))      return true;
    if (read_iso15(dev, card))       return true;
    if (read_felica(dev, card))      return true;
    strncpy(card->protocol, "None",     sizeof(card->protocol) - 1);
    strncpy(card->detail,   "No card",  sizeof(card->detail) - 1);
    return false;
}

bool nfc_reader_read_14b(const struct device *dev, struct card_info *card)
{
    memset(card, 0, sizeof(*card));
    if (read_iso14b_once(dev, card)) return true;
    /* One retry after brief recovery */
    nfc_reader_recover(dev);
    k_msleep(20);
    return read_iso14b_once(dev, card);
}

bool nfc_reader_read_ndef(const struct device *dev,
                          char *text_buf, size_t text_len,
                          char *detail_buf, size_t detail_len)
{
    if (select_reader_common(dev) != 0) {
        strncpy(detail_buf, "Mode switch failed", detail_len - 1);
        return false;
    }
    grove_nfc_write_sys_reg(dev, GNFC_REG_RF_CFG,
                            GNFC_RFCFG_RD_14A | GNFC_RFCFG_TAG_14A);
    grove_nfc_write_sys_reg(dev, GNFC_REG_FWI,       0x0009);
    grove_nfc_write_sys_reg(dev, GNFC_REG_TX_CRC_EN, GNFC_CRC_DISABLE);
    grove_nfc_write_sys_reg(dev, GNFC_REG_RX_CRC_EN, GNFC_CRC_DISABLE);
    grove_nfc_write_misc_reg(dev, GNFC_REG_TX_LAST_BIT, GNFC_TLB_7);
    grove_nfc_set_rf(dev, true);
    k_msleep(5);

    /* Quick 4/7-byte UID acquisition (reuses selectISO14A logic) */
    uint8_t rx[64] = {0};
    uint16_t rx_len = sizeof(rx);
    const uint8_t reqa[] = {0x52};
    if (grove_nfc_txrx(dev, reqa, 1, rx, &rx_len, 10) != 0) {
        grove_nfc_set_rf(dev, false);
        strncpy(detail_buf, "No ISO14443A tag", detail_len - 1);
        return false;
    }
    grove_nfc_write_misc_reg(dev, GNFC_REG_TX_LAST_BIT, GNFC_TLB_0);
    const uint8_t ac1[] = {0x93, 0x20};
    rx_len = sizeof(rx);
    if (grove_nfc_txrx(dev, ac1, 2, rx, &rx_len, 10) != 0 || rx_len < 5) {
        grove_nfc_set_rf(dev, false);
        strncpy(detail_buf, "Anti-collision failed", detail_len - 1);
        return false;
    }
    grove_nfc_write_sys_reg(dev, GNFC_REG_TX_CRC_EN, GNFC_CRC_14A);
    grove_nfc_write_sys_reg(dev, GNFC_REG_RX_CRC_EN, GNFC_CRC_14A);
    const uint8_t sel1[] = {0x93, 0x70, rx[0], rx[1], rx[2], rx[3], rx[4]};
    rx_len = sizeof(rx);
    grove_nfc_txrx(dev, sel1, 7, rx, &rx_len, 10);

    /* Read pages 4..51 (4 pages per block = 16 bytes each) */
    uint8_t data[192] = {0};
    size_t  data_len  = 0;
    for (uint8_t page = 4; page < 52 && data_len + 16 <= sizeof(data); page += 4) {
        uint8_t block[16] = {0};
        if (!type2_read_block(dev, page, block)) break;
        memcpy(data + data_len, block, 16);
        data_len += 16;
    }
    grove_nfc_set_rf(dev, false);

    if (data_len < 8) {
        strncpy(detail_buf, "Read too short", detail_len - 1);
        return false;
    }

    /* Locate NDEF TLV (type=0x03) */
    size_t tlv = 0;
    while (tlv + 2 < data_len && data[tlv] != 0x03) {
        if (data[tlv] == 0x00) { ++tlv; continue; }
        if (data[tlv] == 0xFE) break;
        tlv += 2 + data[tlv + 1];
    }
    if (tlv + 2 >= data_len || data[tlv] != 0x03) {
        strncpy(detail_buf, "No NDEF TLV", detail_len - 1);
        return false;
    }

    size_t ndef_len, ndef_off;
    if (data[tlv + 1] == 0xFF) {
        if (tlv + 4 >= data_len) {
            strncpy(detail_buf, "NDEF length invalid", detail_len - 1);
            return false;
        }
        ndef_len = ((size_t)data[tlv + 2] << 8) | data[tlv + 3];
        ndef_off = tlv + 4;
    } else {
        ndef_len = data[tlv + 1];
        ndef_off = tlv + 2;
    }

    if (ndef_len == 0 || ndef_off + ndef_len > data_len) {
        strncpy(detail_buf, "NDEF payload invalid", detail_len - 1);
        return false;
    }

    size_t n = parse_ndef(data + ndef_off, ndef_len, text_buf, text_len);
    if (n == 0) {
        strncpy(detail_buf, "NDEF empty", detail_len - 1);
        return false;
    }
    strncpy(detail_buf, "Type2 NDEF", detail_len - 1);
    return true;
}

bool nfc_reader_self_check(const struct device *dev,
                           char *report, size_t report_len,
                           uint16_t *hw_ver, uint16_t *fw_ver)
{
    char line[48];
    report[0] = '\0';
    bool ok = true;

    bool ping_ok = grove_nfc_ping(dev);
    strncat(report, ping_ok ? "I2C: OK\n" : "I2C: FAIL\n",
            report_len - strlen(report) - 1);
    ok = ok && ping_ok;
    if (!ping_ok) { *hw_ver = 0; *fw_ver = 0; return false; }

    uint16_t hw = 0, fw = 0;
    grove_nfc_read_sys_reg(dev, GNFC_REG_HW_VER, &hw);
    grove_nfc_read_sys_reg(dev, GNFC_REG_FW_VER, &fw);
    *hw_ver = hw; *fw_ver = fw;
    snprintf(line, sizeof(line), "HW: 0x%04X  FW: 0x%04X\n", hw, fw);
    strncat(report, line, report_len - strlen(report) - 1);

    /* Mode register R/W verify */
    uint16_t mode_val = GNFC_MODE_DEFAULT | GNFC_MODE_TAG_NONE;
    grove_nfc_write_sys_reg(dev, GNFC_REG_MODE, mode_val);
    k_msleep(2);
    uint16_t mode_back = 0;
    grove_nfc_read_sys_reg(dev, GNFC_REG_MODE, &mode_back);
    bool mode_ok = (mode_back == mode_val);
    strncat(report, mode_ok ? "Mode R/W: OK\n" : "Mode R/W: FAIL\n",
            report_len - strlen(report) - 1);
    ok = ok && mode_ok;

    /* RF switch */
    grove_nfc_set_rf(dev, true);
    k_msleep(2);
    grove_nfc_set_rf(dev, false);
    strncat(report, "RF switch: OK", report_len - strlen(report) - 1);
    return ok;
}
