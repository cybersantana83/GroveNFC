/*
 * nfc_emulator.h — NFC tag emulation API (Zephyr port).
 */
#ifndef NFC_EMULATOR_H_
#define NFC_EMULATOR_H_

#include <zephyr/device.h>
#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start emulating a tag type on the given slot.
 * @return true on success.
 */
bool nfc_emulator_start(const struct device *dev,
                        emu_type_t type, uint8_t slot);

/**
 * @brief Stop emulation and put the module back to idle.
 */
void nfc_emulator_stop(const struct device *dev);

/**
 * @brief Drive the emulation state machine (call ~10 ms tick from worker).
 * For Grove NFC hardware the module handles emulation autonomously, so this
 * is a no-op in the current implementation.
 */
void nfc_emulator_tick(const struct device *dev);

/**
 * @brief Return true if emulation is currently active.
 */
bool nfc_emulator_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* NFC_EMULATOR_H_ */
