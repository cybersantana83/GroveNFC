/*
 * nfc_worker.h — NFC polling thread API.
 */
#ifndef NFC_WORKER_H_
#define NFC_WORKER_H_

#include <zephyr/kernel.h>
#include "app_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the NFC worker (call once from main before enabling
 *        the thread).  Sets up message queues and grabs the device pointer.
 * @return 0 on success, -errno otherwise.
 */
int nfc_worker_init(void);

/**
 * @brief Send a command to the NFC worker thread.
 * Non-blocking; returns -EAGAIN if the queue is full.
 */
int nfc_worker_send_cmd(const struct nfc_cmd *cmd);

/**
 * @brief Convenience wrapper: stop RF and restart reader mode.
 */
int nfc_worker_recover(void);

/**
 * @brief Set 14B-only reader mode (for China ID card scanning).
 */
int nfc_worker_set_14b_only(bool only);

/**
 * @brief Read the current mode snapshot flags (set by UI, read by worker).
 */
bool nfc_worker_is_reader_page(void);
bool nfc_worker_is_ndef_page(void);
bool nfc_worker_is_piano_page(void);

/**
 * @brief Tell the worker which UI page is active so it can choose the right
 *        scan mode.
 */
void nfc_worker_set_page(app_page_t page, emu_type_t emu_type, uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* NFC_WORKER_H_ */
