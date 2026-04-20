/*
 * ui_main.c — LVGL 5-page UI thread, pinned to CPU Core 1.
 *
 * Pages (tabs):
 *   0 — Reader   : shows live protocol + UID of last card
 *   1 — NDEF     : scan-on-demand NDEF text viewer
 *   2 — Emulator : choose type & slot, start/stop emulation
 *   3 — Piano    : 8-note piano mapped to NFC cards
 *   4 — Diagnose : self-test report
 *
 * Navigation:
 *   Short press btn_a → advance action on current page (scan, next item)
 *   Long  press btn_a → cycle to next page
 *
 * The UI reads nfc_result_q to update display and writes nfc_cmd_q for
 * commands.  All LVGL calls happen from this single UI thread.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

#include "ui_main.h"
#include "app_events.h"
#include "nfc_worker.h"
#include "storage.h"
#include "piano.h"

LOG_MODULE_REGISTER(ui_main, CONFIG_LOG_DEFAULT_LEVEL);

/* ── Hardware references ────────────────────────────────────────────────── */
#define BTN_NODE DT_ALIAS(sw0)

#if DT_NODE_HAS_STATUS(BTN_NODE, okay)
static const struct gpio_dt_spec s_btn = GPIO_DT_SPEC_GET(BTN_NODE, gpios);
#endif

/* ── Button debounce / long-press ───────────────────────────────────────── */
#define BTN_DEBOUNCE_MS  20
#define BTN_LONG_MS     600

static int64_t  s_press_ms  = 0;
static bool     s_was_down  = false;

typedef enum { BTN_NONE = 0, BTN_SHORT, BTN_LONG } btn_event_t;

static btn_event_t btn_poll(void)
{
#if DT_NODE_HAS_STATUS(BTN_NODE, okay)
    int raw = gpio_pin_get_dt(&s_btn);
    bool down = (raw > 0);
    int64_t now = k_uptime_get();
    btn_event_t ev = BTN_NONE;

    if (down && !s_was_down) {
        s_press_ms = now;
    } else if (!down && s_was_down) {
        int64_t held = now - s_press_ms;
        if (held >= BTN_DEBOUNCE_MS && held < BTN_LONG_MS) ev = BTN_SHORT;
    } else if (down && s_was_down) {
        if ((now - s_press_ms) >= BTN_LONG_MS && s_press_ms != 0) {
            ev = BTN_LONG;
            s_press_ms = 0; /* suppress repeat */
        }
    }
    s_was_down = down;
    return ev;
#else
    return BTN_NONE;
#endif
}

/* ── Thread parameters ──────────────────────────────────────────────────── */
#define UI_STACK_SIZE  CONFIG_GROVENFC_UI_STACK_SIZE
#define UI_THREAD_PRIO 8

static K_THREAD_STACK_DEFINE(ui_stack, UI_STACK_SIZE);
static struct k_thread ui_thread_data;

/* ── Page state ─────────────────────────────────────────────────────────── */
static app_page_t s_page = PAGE_READER;

/* Per-page LVGL objects */
static lv_obj_t *s_tabview;

/* Reader page */
static lv_obj_t *s_rd_proto_label;
static lv_obj_t *s_rd_uid_label;
static lv_obj_t *s_rd_detail_label;
static lv_obj_t *s_rd_14b_label;
static bool      s_14b_mode = false;

/* NDEF page */
static lv_obj_t *s_nd_status_label;
static lv_obj_t *s_nd_text_label;

/* Emulator page */
static lv_obj_t *s_emu_type_label;
static lv_obj_t *s_emu_slot_label;
static lv_obj_t *s_emu_status_label;
static uint8_t   s_emu_type_idx = 0;
static uint8_t   s_emu_slot_idx = 0;
static bool      s_emu_active   = false;

/* Piano page */
static lv_obj_t *s_piano_note_label;
static lv_obj_t *s_piano_uid_label;

/* Diagnose page */
static lv_obj_t *s_diag_label;

/* Notification banner */
static lv_obj_t *s_notif_label;
static int64_t   s_notif_expire = 0;

/* ── Notification (thread-safe via mutex-protected k_msgq) ─────────────── */
static K_MSGQ_DEFINE(s_notif_q, 48, 4, 1);

void ui_main_notify(const char *msg)
{
    char buf[48];
    strncpy(buf, msg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    k_msgq_put(&s_notif_q, buf, K_NO_WAIT);
}

/* ── Emulator type name table ───────────────────────────────────────────── */
static const char *const s_emu_names[] = {
    [EMU_MF1K]   = "MF Classic 1K",
    [EMU_N213]   = "NTAG213",
    [EMU_N215]   = "NTAG215",
    [EMU_N216]   = "NTAG216",
    [EMU_ISO14B] = "ISO14443B",
    [EMU_ISO15]  = "ISO15693",
    [EMU_FELICA] = "FeliCa(N/A)",
};

/* ── Piano note labels ──────────────────────────────────────────────────── */
static const char *const s_note_names[] = {
    "Do", "Re", "Mi", "Fa", "Sol", "La", "Ti", "Do8",
};

/* ── UI builder helpers ─────────────────────────────────────────────────── */

static lv_obj_t *make_label(lv_obj_t *parent, const char *init_text,
                             lv_align_t align, int16_t x, int16_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, init_text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_align(l, align, x, y);
    return l;
}

/* ── Build all pages ────────────────────────────────────────────────────── */

static void build_ui(void)
{
    s_tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 20);
    lv_obj_set_size(s_tabview, LV_PCT(100), LV_PCT(100));

    lv_obj_t *tab_rd  = lv_tabview_add_tab(s_tabview, "Read");
    lv_obj_t *tab_nd  = lv_tabview_add_tab(s_tabview, "NDEF");
    lv_obj_t *tab_emu = lv_tabview_add_tab(s_tabview, "Emu");
    lv_obj_t *tab_pi  = lv_tabview_add_tab(s_tabview, "Piano");
    lv_obj_t *tab_dg  = lv_tabview_add_tab(s_tabview, "Diag");

    /* ── Reader ── */
    s_rd_proto_label  = make_label(tab_rd, "Protocol: --",
                                   LV_ALIGN_TOP_LEFT, 0, 0);
    s_rd_uid_label    = make_label(tab_rd, "UID: --",
                                   LV_ALIGN_TOP_LEFT, 0, 18);
    s_rd_detail_label = make_label(tab_rd, "Detail: --",
                                   LV_ALIGN_TOP_LEFT, 0, 36);
    s_rd_14b_label    = make_label(tab_rd, "[Normal]",
                                   LV_ALIGN_TOP_LEFT, 0, 54);

    /* ── NDEF ── */
    s_nd_status_label = make_label(tab_nd, "Press btn to scan",
                                   LV_ALIGN_TOP_LEFT, 0, 0);
    s_nd_text_label   = make_label(tab_nd, "",
                                   LV_ALIGN_TOP_LEFT, 0, 20);

    /* ── Emulator ── */
    char emu_buf[24];
    snprintf(emu_buf, sizeof(emu_buf), "Type: %s", s_emu_names[s_emu_type_idx]);
    s_emu_type_label   = make_label(tab_emu, emu_buf,
                                    LV_ALIGN_TOP_LEFT, 0, 0);
    char slot_buf[16];
    snprintf(slot_buf, sizeof(slot_buf), "Slot: %d", s_emu_slot_idx);
    s_emu_slot_label   = make_label(tab_emu, slot_buf,
                                    LV_ALIGN_TOP_LEFT, 0, 20);
    s_emu_status_label = make_label(tab_emu, "Stopped",
                                    LV_ALIGN_TOP_LEFT, 0, 40);

    /* ── Piano ── */
    s_piano_note_label = make_label(tab_pi, "Note: --",
                                    LV_ALIGN_TOP_LEFT, 0, 0);
    s_piano_uid_label  = make_label(tab_pi, "Card: --",
                                    LV_ALIGN_TOP_LEFT, 0, 20);

    /* ── Diagnose ── */
    s_diag_label = make_label(tab_dg, "Press btn to run",
                               LV_ALIGN_TOP_LEFT, 0, 0);

    /* ── Notification overlay ── */
    s_notif_label = lv_label_create(lv_layer_top());
    lv_label_set_text(s_notif_label, "");
    lv_obj_set_style_bg_color(s_notif_label, lv_color_hex(0x444444),
                               LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_notif_label, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_notif_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(s_notif_label, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_flag(s_notif_label, LV_OBJ_FLAG_HIDDEN);
}

/* ── Page update helpers ────────────────────────────────────────────────── */

static void update_reader(const struct nfc_result *res)
{
    char buf[64];
    if (res->type == NFC_RES_CARD && res->ok) {
        snprintf(buf, sizeof(buf), "Proto: %s", res->card.protocol);
        lv_label_set_text(s_rd_proto_label, buf);
        snprintf(buf, sizeof(buf), "UID: %s", res->card.uid);
        lv_label_set_text(s_rd_uid_label, buf);
        snprintf(buf, sizeof(buf), "%s", res->card.detail);
        lv_label_set_text(s_rd_detail_label, buf);
    } else if (res->type == NFC_RES_NO_CARD) {
        lv_label_set_text(s_rd_proto_label, "Proto: --");
        lv_label_set_text(s_rd_uid_label,   "UID: --");
        lv_label_set_text(s_rd_detail_label,"");
    }
}

static void update_ndef(const struct nfc_result *res)
{
    if (res->type != NFC_RES_NDEF) return;
    if (res->ok) {
        lv_label_set_text(s_nd_status_label, res->ndef_detail);
        lv_label_set_text(s_nd_text_label,   res->ndef_text);
    } else {
        lv_label_set_text(s_nd_status_label, res->ndef_detail[0]
                          ? res->ndef_detail : "No NDEF found");
        lv_label_set_text(s_nd_text_label, "");
    }
}

static void update_emu_labels(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "Type: %s", s_emu_names[s_emu_type_idx]);
    lv_label_set_text(s_emu_type_label, buf);
    snprintf(buf, sizeof(buf), "Slot: %d", s_emu_slot_idx);
    lv_label_set_text(s_emu_slot_label, buf);
    lv_label_set_text(s_emu_status_label, s_emu_active ? "Active" : "Stopped");
}

static void update_piano(const struct nfc_result *res)
{
    if (res->type != NFC_RES_CARD || !res->ok) return;
    int note = piano_match_card(res->card.uid);
    char buf[24];
    if (note >= 0) {
        snprintf(buf, sizeof(buf), "Note: %s", s_note_names[note]);
    } else {
        snprintf(buf, sizeof(buf), "Note: ? (new)");
    }
    lv_label_set_text(s_piano_note_label, buf);
    snprintf(buf, sizeof(buf), "Card: %.20s", res->card.uid);
    lv_label_set_text(s_piano_uid_label, buf);
}

static void update_diag(const struct nfc_result *res)
{
    if (res->type != NFC_RES_DIAGNOSE) return;
    char buf[144];
    snprintf(buf, sizeof(buf), "%s\nHW=%04X FW=%04X",
             res->diag_report, res->hw_ver, res->fw_ver);
    lv_label_set_text(s_diag_label, buf);
}

/* ── Button action per page ─────────────────────────────────────────────── */

static void on_short_press(void)
{
    struct nfc_cmd cmd = {0};
    switch (s_page) {
    case PAGE_READER:
        /* Toggle 14B-only mode */
        s_14b_mode = !s_14b_mode;
        nfc_worker_set_14b_only(s_14b_mode);
        lv_label_set_text(s_rd_14b_label,
                          s_14b_mode ? "[14B only]" : "[Normal]");
        break;
    case PAGE_NDEF:
        lv_label_set_text(s_nd_status_label, "Scanning...");
        cmd.type = NFC_CMD_SCAN_NDEF;
        nfc_worker_send_cmd(&cmd);
        break;
    case PAGE_EMU:
        if (s_emu_active) {
            /* Stop emulation */
            cmd.type = NFC_CMD_STOP_RF;
            nfc_worker_send_cmd(&cmd);
            s_emu_active = false;
        } else {
            /* Cycle type */
            s_emu_type_idx = (s_emu_type_idx + 1) % EMU_COUNT;
        }
        update_emu_labels();
        break;
    case PAGE_PIANO:
        /* Cycle note slot assignment */
        piano_advance_slot();
        {
            char buf[24];
            snprintf(buf, sizeof(buf), "Note: %s", s_note_names[piano_current_slot()]);
            lv_label_set_text(s_piano_note_label, buf);
        }
        break;
    case PAGE_DIAG:
        lv_label_set_text(s_diag_label, "Running...");
        cmd.type = NFC_CMD_DIAGNOSE;
        nfc_worker_send_cmd(&cmd);
        break;
    default:
        break;
    }
}

static void on_long_press(void)
{
    /* Emulator: long-press to start/cycle slot; other pages: go to next page */
    if (s_page == PAGE_EMU && !s_emu_active) {
        /* Start emulation */
        struct nfc_cmd cmd = {
            .type     = NFC_CMD_START_EMU,
            .emu_type = s_emu_type_idx,
            .emu_slot = s_emu_slot_idx,
        };
        s_emu_slot_idx = (s_emu_slot_idx + 1) % CONFIG_GROVENFC_PIANO_NOTES;
        nfc_worker_send_cmd(&cmd);
        s_emu_active = true;
        update_emu_labels();
        return;
    }

    /* Cycle to next page */
    s_page = (app_page_t)((s_page + 1) % PAGE_COUNT);
    lv_tabview_set_act(s_tabview, (uint16_t)s_page, LV_ANIM_ON);
    nfc_worker_set_page(s_page, (emu_type_t)s_emu_type_idx, s_emu_slot_idx);
}

/* ── Main UI thread ─────────────────────────────────────────────────────── */

static void ui_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    LOG_INF("UI thread started");

#if DT_NODE_HAS_STATUS(BTN_NODE, okay)
    if (device_is_ready(s_btn.port)) {
        gpio_pin_configure_dt(&s_btn, GPIO_INPUT);
    }
#endif

    build_ui();

    while (true) {
        /* ── Handle button ──────────────────────────────────────────── */
        btn_event_t bev = btn_poll();
        if (bev == BTN_SHORT) on_short_press();
        else if (bev == BTN_LONG) on_long_press();

        /* ── Drain NFC result queue ─────────────────────────────────── */
        struct nfc_result res;
        while (k_msgq_get(&nfc_result_q, &res, K_NO_WAIT) == 0) {
            switch (res.type) {
            case NFC_RES_CARD:
            case NFC_RES_NO_CARD:
                if (s_page == PAGE_READER) update_reader(&res);
                if (s_page == PAGE_PIANO && res.type == NFC_RES_CARD) {
                    update_piano(&res);
                    piano_play_card(res.card.uid);
                }
                break;
            case NFC_RES_NDEF:
                if (s_page == PAGE_NDEF) update_ndef(&res);
                break;
            case NFC_RES_DIAGNOSE:
                if (s_page == PAGE_DIAG) update_diag(&res);
                break;
            case NFC_RES_CMD_DONE:
                if (!res.ok && s_emu_active) {
                    s_emu_active = false;
                    lv_label_set_text(s_emu_status_label, "Failed");
                    ui_main_notify("Emulation failed");
                } else if (res.ok && s_emu_active) {
                    lv_label_set_text(s_emu_status_label, "Active");
                }
                break;
            case NFC_RES_HEALTH:
                if (!res.ok) {
                    ui_main_notify("NFC comm error");
                } else if (res.health_reconnected) {
                    ui_main_notify("NFC recovered");
                }
                break;
            default:
                break;
            }
        }

        /* ── Drain notification queue ───────────────────────────────── */
        char notif[48];
        if (k_msgq_get(&s_notif_q, notif, K_NO_WAIT) == 0) {
            lv_label_set_text(s_notif_label, notif);
            lv_obj_clear_flag(s_notif_label, LV_OBJ_FLAG_HIDDEN);
            s_notif_expire = k_uptime_get() + 2000;
        }
        if (s_notif_expire && k_uptime_get() > s_notif_expire) {
            lv_obj_add_flag(s_notif_label, LV_OBJ_FLAG_HIDDEN);
            s_notif_expire = 0;
        }

        /* ── LVGL tick ──────────────────────────────────────────────── */
        lv_task_handler();
        k_msleep(10);
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void ui_main_start(void)
{
    k_tid_t tid = k_thread_create(&ui_thread_data, ui_stack,
                                  K_THREAD_STACK_SIZEOF(ui_stack),
                                  ui_thread_entry, NULL, NULL, NULL,
                                  UI_THREAD_PRIO, 0, K_MSEC(200));
    k_thread_name_set(tid, "ui_main");

#ifdef CONFIG_SCHED_CPU_MASK
    k_thread_cpu_mask_clear(tid);
    k_thread_cpu_mask_enable(tid, 1);
    k_thread_start(tid);
#endif

    LOG_INF("UI thread created");
}
