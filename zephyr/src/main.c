/*
 * main.c — GroveNFC Zephyr application entry point.
 *
 * Boot sequence:
 *   1. Initialise storage (NVS)
 *   2. Initialise piano (load card→note mappings)
 *   3. Power on display backlight via PWM
 *   4. Start NFC worker thread (Core 0)
 *   5. Start LVGL UI thread (Core 1)
 *   6. Return — the main thread exits and Zephyr manages the rest.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

#include "nfc_worker.h"
#include "ui_main.h"
#include "storage.h"
#include "piano.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

/* ── Backlight (PWM alias `backlight` in DTS) ───────────────────────────── */
#define BL_NODE DT_ALIAS(pwm_bl)

#if DT_NODE_HAS_STATUS(BL_NODE, okay)
static const struct pwm_dt_spec s_bl = PWM_DT_SPEC_GET(BL_NODE);

static void backlight_set(uint8_t pct)
{
    if (!device_is_ready(s_bl.dev)) return;
    uint32_t period_ns = 1000000u; /* 1 kHz */
    uint32_t pulse_ns  = (uint32_t)(period_ns / 100u) * pct;
    pwm_set_dt(&s_bl, period_ns, pulse_ns);
}
#else
static void backlight_set(uint8_t pct) { ARG_UNUSED(pct); }
#endif

/* ── Display init ───────────────────────────────────────────────────────── */
static void display_init(void)
{
    const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display)) {
        LOG_ERR("Display not ready");
        return;
    }
    display_blanking_off(display);
    backlight_set(80);
    LOG_INF("Display ready");
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(void)
{
    LOG_INF("GroveNFC Zephyr starting");

    /* 1. Storage */
    if (storage_init() != 0) {
        LOG_WRN("Storage init failed — running without persistence");
    }

    /* 2. Piano */
    piano_init();

    /* 3. Display + backlight */
    display_init();

    /* 4. NFC worker (Core 0) */
    if (nfc_worker_init() != 0) {
        LOG_ERR("NFC worker init failed");
    }

    /* 5. UI thread (Core 1) */
    ui_main_start();

    LOG_INF("Boot complete");
    return 0;
}
