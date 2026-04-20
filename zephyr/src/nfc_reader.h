/*
 * nfc_reader.h — ISO14443A/B, ISO15693, FeliCa reader API (Zephyr port).
 */
#ifndef NFC_READER_H_
#define NFC_READER_H_

#include <zephyr/device.h>
#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Reset the module to a known state (call on boot / recover). */
void nfc_reader_reset(const struct device *dev);

/** Recover from a communication error. Returns true if the device responds. */
bool nfc_reader_recover(const struct device *dev);

/**
 * @brief Try all supported protocols in order (14B→14A→15→FeliCa).
 * @return true if a card was detected.
 */
bool nfc_reader_read_any(const struct device *dev, struct card_info *card);

/**
 * @brief Scan ISO14443B only (China ID card mode).
 */
bool nfc_reader_read_14b(const struct device *dev, struct card_info *card);

/**
 * @brief Read NDEF from a Type-2 tag (ISO14443A).
 * Returns true if NDEF was found and parsed.
 */
bool nfc_reader_read_ndef(const struct device *dev,
                          char *text_buf, size_t text_len,
                          char *detail_buf, size_t detail_len);

/**
 * @brief Run self-check diagnostics.
 * @return true if all checks pass.
 */
bool nfc_reader_self_check(const struct device *dev,
                           char *report, size_t report_len,
                           uint16_t *hw_ver, uint16_t *fw_ver);

#ifdef __cplusplus
}
#endif

#endif /* NFC_READER_H_ */
