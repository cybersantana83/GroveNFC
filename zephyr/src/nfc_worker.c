/*
 * nfc_worker.c — NFC polling thread (pinned to CPU core 0).
 *
 * Architecture:
 *   • One Zephyr thread runs the NFC state machine on Core 0.
 *   • The UI thread (Core 1) communicates via k_msgq:
 *       nfc_cmd_q    (UI  → worker): commands (start emu, diagnose, etc.)
 *       nfc_result_q (worker → UI): card detections, NDEF, health events.
 *   • Shared mode-snapshot variables are written by UI and read by the
 *     worker; they use atomic_t / a mutex because the worker only reads them
 *     and the UI only writes them between command sends.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include "app_events.h"
#include "nfc_worker.h"
#include "nfc_reader.h"
#include "nfc_emulator.h"
#include "grove_nfc.h"

LOG_MODULE_REGISTER(nfc_worker, CONFIG_LOG_DEFAULT_LEVEL);

/* ── Message queues ─────────────────────────────────────────────────────── */
K_MSGQ_DEFINE(nfc_cmd_q,    sizeof(struct nfc_cmd),    8, 4);
K_MSGQ_DEFINE(nfc_result_q, sizeof(struct nfc_result), 8, 4);

/* ── Thread parameters ──────────────────────────────────────────────────── */
#define NFC_THREAD_STACK  CONFIG_GROVENFC_NFC_STACK_SIZE
#define NFC_THREAD_PRIO   5
#define NFC_POLL_MS       CONFIG_GROVENFC_NFC_POLL_INTERVAL_MS

static K_THREAD_STACK_DEFINE(nfc_stack, NFC_THREAD_STACK);
static struct k_thread nfc_thread_data;

/* ── Device reference (set during init) ────────────────────────────────── */
static const struct device *s_nfc_dev;

/* ── UI page snapshot (written by UI thread, read by worker) ────────────── */
static atomic_t s_current_page = ATOMIC_INIT(PAGE_READER);
static atomic_t s_emu_type     = ATOMIC_INIT(EMU_N213);
static atomic_t s_emu_slot     = ATOMIC_INIT(0);
static atomic_t s_14b_only     = ATOMIC_INIT(0);

/* ── Helpers ────────────────────────────────────────────────────────────── */

static void post_result(struct nfc_result *res)
{
    if (k_msgq_put(&nfc_result_q, res, K_NO_WAIT) != 0) {
        LOG_WRN("nfc_result_q full — dropped result");
    }
}

static void post_card(const struct card_info *card)
{
    struct nfc_result res = {
        .type = NFC_RES_CARD,
        .card = *card,
        .ok   = card->valid,
    };
    post_result(&res);
}

static void post_no_card(void)
{
    struct nfc_result res = { .type = NFC_RES_NO_CARD };
    post_result(&res);
}

/* ── Command handlers ───────────────────────────────────────────────────── */

static void handle_diagnose(void)
{
    struct nfc_result res = { .type = NFC_RES_DIAGNOSE };
    res.ok = nfc_reader_self_check(s_nfc_dev, res.diag_report,
                                   sizeof(res.diag_report),
                                   &res.hw_ver, &res.fw_ver);
    post_result(&res);
}

static void handle_scan_ndef(void)
{
    struct nfc_result res = { .type = NFC_RES_NDEF };
    res.ok = nfc_reader_read_ndef(s_nfc_dev, res.ndef_text,
                                  sizeof(res.ndef_text),
                                  res.ndef_detail,
                                  sizeof(res.ndef_detail));
    post_result(&res);
}

static void handle_start_emu(uint8_t emu_type, uint8_t slot)
{
    struct nfc_result res = { .type = NFC_RES_CMD_DONE };
    res.ok = nfc_emulator_start(s_nfc_dev, (emu_type_t)emu_type, slot);
    post_result(&res);
}

static void handle_stop_rf(void)
{
    nfc_emulator_stop(s_nfc_dev);
    grove_nfc_set_rf(s_nfc_dev, false);

    struct nfc_result res = { .type = NFC_RES_CMD_DONE, .ok = true };
    post_result(&res);
}

static void handle_recover(void)
{
    bool ok = nfc_reader_recover(s_nfc_dev);
    struct nfc_result res = {
        .type = NFC_RES_HEALTH,
        .ok   = ok,
        .health_reconnected = ok,
        .health_lost        = !ok,
    };
    post_result(&res);
}

/* ── Main worker loop ───────────────────────────────────────────────────── */

static void nfc_worker_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    LOG_INF("NFC worker started (Core 0 pinned)");

    /* Allow hardware to settle after boot */
    k_msleep(300);

    bool emu_active = false;
    uint32_t health_check_ms = 0;

    while (true) {
        /* ── 1. Drain command queue ────────────────────────────────────── */
        struct nfc_cmd cmd;
        while (k_msgq_get(&nfc_cmd_q, &cmd, K_NO_WAIT) == 0) {
            switch (cmd.type) {
            case NFC_CMD_START_EMU:
                emu_active = false; /* reset before starting */
                handle_start_emu(cmd.emu_type, cmd.emu_slot);
                emu_active = true;
                break;
            case NFC_CMD_STOP_RF:
                handle_stop_rf();
                emu_active = false;
                break;
            case NFC_CMD_DIAGNOSE:
                handle_diagnose();
                break;
            case NFC_CMD_SCAN_NDEF:
                handle_scan_ndef();
                break;
            case NFC_CMD_RECOVER:
                handle_recover();
                emu_active = false;
                break;
            case NFC_CMD_SET_14B_ONLY:
                atomic_set(&s_14b_only, cmd.flag ? 1 : 0);
                break;
            default:
                break;
            }
        }

        /* ── 2. Tick emulation state machine ──────────────────────────── */
        if (emu_active) {
            nfc_emulator_tick(s_nfc_dev);
            k_msleep(10);
            continue;
        }

        /* ── 3. Periodic health check ─────────────────────────────────── */
        uint32_t now = k_uptime_get_32();
        if (now - health_check_ms > 3000) {
            health_check_ms = now;
            if (!grove_nfc_ping(s_nfc_dev)) {
                struct nfc_result res = {
                    .type = NFC_RES_HEALTH,
                    .ok   = false,
                    .health_lost = true,
                };
                post_result(&res);
            }
        }

        /* ── 4. Reader / piano / NDEF polling ─────────────────────────── */
        app_page_t page = (app_page_t)atomic_get(&s_current_page);
        struct card_info card = {0};
        bool got;

        if (page == PAGE_NDEF) {
            /* NDEF scanning is done on demand via NFC_CMD_SCAN_NDEF */
            k_msleep(NFC_POLL_MS);
            continue;
        }

        if (page == PAGE_READER || page == PAGE_PIANO) {
            bool b_only = (bool)atomic_get(&s_14b_only);
            if (b_only) {
                got = nfc_reader_read_14b(s_nfc_dev, &card);
            } else {
                got = nfc_reader_read_any(s_nfc_dev, &card);
            }

            if (got) {
                post_card(&card);
            } else {
                post_no_card();
            }
        }

        k_msleep(NFC_POLL_MS);
    }
}

/* ── Init & public API ──────────────────────────────────────────────────── */

int nfc_worker_init(void)
{
    s_nfc_dev = DEVICE_DT_GET(DT_ALIAS(grove_nfc));
    if (!device_is_ready(s_nfc_dev)) {
        LOG_ERR("Grove NFC device not ready");
        return -ENODEV;
    }

    /* Boot-time module reset */
    nfc_reader_reset(s_nfc_dev);

    k_tid_t tid = k_thread_create(&nfc_thread_data, nfc_stack,
                                  K_THREAD_STACK_SIZEOF(nfc_stack),
                                  nfc_worker_thread, NULL, NULL, NULL,
                                  NFC_THREAD_PRIO, 0, K_MSEC(100));
    k_thread_name_set(tid, "nfc_worker");

    /* Pin NFC thread to Core 0 */
#ifdef CONFIG_SCHED_CPU_MASK
    k_thread_cpu_mask_clear(tid);
    k_thread_cpu_mask_enable(tid, 0);
    k_thread_start(tid);
#endif

    LOG_INF("NFC worker thread created");
    return 0;
}

int nfc_worker_send_cmd(const struct nfc_cmd *cmd)
{
    return k_msgq_put(&nfc_cmd_q, cmd, K_NO_WAIT);
}

int nfc_worker_recover(void)
{
    struct nfc_cmd cmd = { .type = NFC_CMD_RECOVER };
    return nfc_worker_send_cmd(&cmd);
}

int nfc_worker_set_14b_only(bool only)
{
    struct nfc_cmd cmd = { .type = NFC_CMD_SET_14B_ONLY, .flag = only };
    return nfc_worker_send_cmd(&cmd);
}

void nfc_worker_set_page(app_page_t page, emu_type_t emu_type, uint8_t slot)
{
    atomic_set(&s_current_page, (atomic_val_t)page);
    atomic_set(&s_emu_type,     (atomic_val_t)emu_type);
    atomic_set(&s_emu_slot,     (atomic_val_t)slot);
}

bool nfc_worker_is_reader_page(void)
{
    return (app_page_t)atomic_get(&s_current_page) == PAGE_READER;
}

bool nfc_worker_is_ndef_page(void)
{
    return (app_page_t)atomic_get(&s_current_page) == PAGE_NDEF;
}

bool nfc_worker_is_piano_page(void)
{
    return (app_page_t)atomic_get(&s_current_page) == PAGE_PIANO;
}
