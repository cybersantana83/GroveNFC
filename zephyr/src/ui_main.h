/*
 * ui_main.h — LVGL-based 5-page UI thread API.
 */
#ifndef UI_MAIN_H_
#define UI_MAIN_H_

#include <zephyr/device.h>
#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create and start the UI thread (pinned to Core 1).
 *
 * Must be called after display is ready and nfc_worker_init().
 */
void ui_main_start(void);

/**
 * @brief Push a notification banner onto the UI (from any thread).
 * String is copied — safe to pass stack memory.
 */
void ui_main_notify(const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* UI_MAIN_H_ */
