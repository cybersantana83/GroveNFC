/*
 * storage.h — Zephyr NVS-based persistent storage API.
 */
#ifndef STORAGE_H_
#define STORAGE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize NVS partition. Call once at boot. */
int storage_init(void);

/**
 * @brief Load/save piano note → card UID mapping.
 * @param slot  0–7 (CONFIG_GROVENFC_PIANO_NOTES - 1)
 * @param uid   Card UID hex string (CARD_UID_LEN bytes)
 */
int  storage_set_piano_uid(uint8_t slot, const char *uid);
bool storage_get_piano_uid(uint8_t slot, char *uid, size_t uid_sz);

/** Last used emulator type (0 = EMU_MF1K … 6 = EMU_FELICA). */
int  storage_set_emu_type(uint8_t type);
bool storage_get_emu_type(uint8_t *type);

/** Last used emulator slot. */
int  storage_set_emu_slot(uint8_t slot);
bool storage_get_emu_slot(uint8_t *slot);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H_ */
