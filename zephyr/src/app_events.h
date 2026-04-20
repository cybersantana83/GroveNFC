/*
 * app_events.h — Shared event types between NFC worker and UI threads.
 *
 * Uses Zephyr k_msgq for lock-free single-producer/consumer queues.
 * Structs are kept small (≤64 bytes) so the message queues don't waste RAM.
 */
#ifndef APP_EVENTS_H_
#define APP_EVENTS_H_

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Card info ─────────────────────────────────────────────────────────── */
#define CARD_PROTO_LEN  16
#define CARD_UID_LEN    32
#define CARD_DETAIL_LEN 48

struct card_info {
    char  protocol[CARD_PROTO_LEN];
    char  uid[CARD_UID_LEN];
    char  detail[CARD_DETAIL_LEN];
    bool  valid;
};

/* ── Commands: UI → NFC worker ─────────────────────────────────────────── */
enum nfc_cmd_type {
    NFC_CMD_NONE = 0,
    NFC_CMD_START_EMU,
    NFC_CMD_STOP_RF,
    NFC_CMD_DIAGNOSE,
    NFC_CMD_SCAN_NDEF,
    NFC_CMD_RECOVER,
    NFC_CMD_SET_14B_ONLY,
};

struct nfc_cmd {
    enum nfc_cmd_type type;
    uint8_t  emu_type;   /* EmuType enum value (for START_EMU) */
    uint8_t  emu_slot;   /* slot index */
    bool     flag;       /* generic flag (e.g. 14B-only mode) */
};

/* ── Results: NFC worker → UI ──────────────────────────────────────────── */
enum nfc_result_type {
    NFC_RES_CARD = 0,
    NFC_RES_NO_CARD,
    NFC_RES_NDEF,
    NFC_RES_DIAGNOSE,
    NFC_RES_HEALTH,
    NFC_RES_CMD_DONE,
};

#define NFC_DIAG_LEN  128
#define NFC_NDEF_LEN  96

struct nfc_result {
    enum nfc_result_type type;
    struct card_info card;          /* valid for CARD */
    char   ndef_text[NFC_NDEF_LEN]; /* valid for NDEF */
    char   ndef_detail[48];
    char   diag_report[NFC_DIAG_LEN]; /* valid for DIAGNOSE */
    bool   ok;
    uint16_t hw_ver;
    uint16_t fw_ver;
    bool   health_lost;
    bool   health_reconnected;
};

/* ── Emulation type enum (matches Arduino EmuType) ──────────────────────── */
typedef enum {
    EMU_MF1K   = 0,
    EMU_N213   = 1,
    EMU_N215   = 2,
    EMU_N216   = 3,
    EMU_ISO14B = 4,
    EMU_FELICA = 5,
    EMU_ISO15  = 6,
    EMU_COUNT,
} emu_type_t;

/* ── Menu page enum ──────────────────────────────────────────────────────── */
typedef enum {
    PAGE_READER  = 0,
    PAGE_NDEF    = 1,
    PAGE_EMU     = 2,
    PAGE_PIANO   = 3,
    PAGE_DIAG    = 4,
    PAGE_COUNT,
} app_page_t;

/* ── Queue accessors (defined in nfc_worker.c) ──────────────────────────── */
extern struct k_msgq nfc_cmd_q;    /* app → nfc worker */
extern struct k_msgq nfc_result_q; /* nfc worker → app */

#ifdef __cplusplus
}
#endif

#endif /* APP_EVENTS_H_ */
