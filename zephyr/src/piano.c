/*
 * piano.c — 8-note piano, plays tones via PWM and maps NFC card UIDs.
 *
 * Note frequencies (Hz): Do=523, Re=587, Mi=659, Fa=698,
 *                        Sol=784, La=880, Ti=988, Do8=1047
 *
 * Tone is generated on the PWM channel aliased `buzzer` in the DTS.
 * The duty cycle is fixed at 50 % for a square-wave tone.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "piano.h"
#include "storage.h"
#include "app_events.h"

LOG_MODULE_REGISTER(piano, CONFIG_LOG_DEFAULT_LEVEL);

#define BUZZER_NODE DT_ALIAS(pwm_tone)

#if DT_NODE_HAS_STATUS(BUZZER_NODE, okay)
static const struct pwm_dt_spec s_buzzer = PWM_DT_SPEC_GET(BUZZER_NODE);
#endif

/* ── Note table ─────────────────────────────────────────────────────────── */
#define NOTE_COUNT CONFIG_GROVENFC_PIANO_NOTES

static const uint32_t s_note_freq[8] = {
    523, 587, 659, 698, 784, 880, 988, 1047
};

static char s_uid_map[8][CARD_UID_LEN]; /* '\0' → unassigned */

static uint8_t s_bind_slot = 0;

/* ── Public API ─────────────────────────────────────────────────────────── */

void piano_init(void)
{
    for (uint8_t i = 0; i < NOTE_COUNT; ++i) {
        if (!storage_get_piano_uid(i, s_uid_map[i], CARD_UID_LEN)) {
            s_uid_map[i][0] = '\0';
        }
    }
    LOG_INF("Piano mappings loaded");
}

int piano_match_card(const char *uid)
{
    if (!uid || uid[0] == '\0') return -1;
    for (int i = 0; i < NOTE_COUNT; ++i) {
        if (s_uid_map[i][0] != '\0' &&
            strncmp(s_uid_map[i], uid, CARD_UID_LEN - 1) == 0) {
            return i;
        }
    }
    return -1;
}

void piano_play_note(uint8_t note)
{
    if (note >= NOTE_COUNT) return;

#if DT_NODE_HAS_STATUS(BUZZER_NODE, okay)
    if (!device_is_ready(s_buzzer.dev)) return;
    uint32_t period_ns = 1000000000u / s_note_freq[note];
    uint32_t pulse_ns  = period_ns / 2u;
    pwm_set_dt(&s_buzzer, period_ns, pulse_ns);
    k_msleep(200);
    pwm_set_dt(&s_buzzer, period_ns, 0); /* silence */
#else
    LOG_DBG("piano_play_note: note=%d (no buzzer HW)", note);
#endif
}

void piano_play_card(const char *uid)
{
    int note = piano_match_card(uid);
    if (note >= 0) {
        piano_play_note((uint8_t)note);
        return;
    }
    /* Not mapped yet — bind to current slot */
    strncpy(s_uid_map[s_bind_slot], uid, CARD_UID_LEN - 1);
    s_uid_map[s_bind_slot][CARD_UID_LEN - 1] = '\0';
    storage_set_piano_uid(s_bind_slot, s_uid_map[s_bind_slot]);
    piano_play_note(s_bind_slot);
}

void piano_advance_slot(void)
{
    s_bind_slot = (uint8_t)((s_bind_slot + 1) % NOTE_COUNT);
}

uint8_t piano_current_slot(void)
{
    return s_bind_slot;
}
