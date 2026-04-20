/*
 * piano.h — 8-note piano backed by NFC card UID mapping.
 */
#ifndef PIANO_H_
#define PIANO_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize piano (loads mappings from storage). */
void piano_init(void);

/**
 * @brief Match a card UID to a note slot.
 * @return 0–7 note slot, or -1 if not mapped.
 */
int piano_match_card(const char *uid);

/**
 * @brief Play the tone for the given note slot and optionally
 *        bind the UID to that slot if it was not yet mapped.
 */
void piano_play_card(const char *uid);

/** Advance the current "bind slot" to the next note. */
void piano_advance_slot(void);

/** Return the current "bind slot" index (0–7). */
uint8_t piano_current_slot(void);

/** Directly play a note (0–7) for testing. */
void piano_play_note(uint8_t note);

#ifdef __cplusplus
}
#endif

#endif /* PIANO_H_ */
