/*
 * storage.c — Zephyr NVS-based persistent storage.
 *
 * NVS ID layout:
 *   1–8  : piano slot 0–7 UID strings (CARD_UID_LEN bytes each)
 *   9    : last emu_type (1 byte)
 *   10   : last emu_slot (1 byte)
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "storage.h"
#include "app_events.h"

LOG_MODULE_REGISTER(storage, CONFIG_LOG_DEFAULT_LEVEL);

#define NVS_PARTITION        storage_partition
#define NVS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(NVS_PARTITION)

#define NVS_ID_PIANO_BASE 1u   /* IDs 1..8 */
#define NVS_ID_EMU_TYPE   9u
#define NVS_ID_EMU_SLOT   10u

static struct nvs_fs s_fs;
static bool          s_ready = false;

int storage_init(void)
{
    struct flash_pages_info info;
    const struct device *flash_dev = NVS_PARTITION_DEVICE;

    if (!device_is_ready(flash_dev)) {
        LOG_ERR("Flash device not ready");
        return -ENODEV;
    }

    s_fs.flash_device = flash_dev;
    s_fs.offset       = NVS_PARTITION_OFFSET;

    int rc = flash_get_page_info_by_offs(flash_dev, s_fs.offset, &info);
    if (rc) {
        LOG_ERR("flash_get_page_info_by_offs: %d", rc);
        return rc;
    }
    s_fs.sector_size  = info.size;
    s_fs.sector_count = 3u;

    rc = nvs_mount(&s_fs);
    if (rc) {
        LOG_ERR("nvs_mount: %d", rc);
        return rc;
    }
    s_ready = true;
    LOG_INF("NVS ready (sector_size=%u)", s_fs.sector_size);
    return 0;
}

int storage_set_piano_uid(uint8_t slot, const char *uid)
{
    if (!s_ready || slot >= CONFIG_GROVENFC_PIANO_NOTES) return -EINVAL;
    uint16_t id = (uint16_t)(NVS_ID_PIANO_BASE + slot);
    return nvs_write(&s_fs, id, uid, CARD_UID_LEN);
}

bool storage_get_piano_uid(uint8_t slot, char *uid, size_t uid_sz)
{
    if (!s_ready || slot >= CONFIG_GROVENFC_PIANO_NOTES || uid_sz < CARD_UID_LEN)
        return false;
    uint16_t id = (uint16_t)(NVS_ID_PIANO_BASE + slot);
    ssize_t rc = nvs_read(&s_fs, id, uid, CARD_UID_LEN);
    if (rc < 0) {
        uid[0] = '\0';
        return false;
    }
    uid[CARD_UID_LEN - 1] = '\0';
    return uid[0] != '\0';
}

int storage_set_emu_type(uint8_t type)
{
    if (!s_ready) return -EINVAL;
    return nvs_write(&s_fs, NVS_ID_EMU_TYPE, &type, 1);
}

bool storage_get_emu_type(uint8_t *type)
{
    if (!s_ready) return false;
    ssize_t rc = nvs_read(&s_fs, NVS_ID_EMU_TYPE, type, 1);
    return rc == 1;
}

int storage_set_emu_slot(uint8_t slot)
{
    if (!s_ready) return -EINVAL;
    return nvs_write(&s_fs, NVS_ID_EMU_SLOT, &slot, 1);
}

bool storage_get_emu_slot(uint8_t *slot)
{
    if (!s_ready) return false;
    ssize_t rc = nvs_read(&s_fs, NVS_ID_EMU_SLOT, slot, 1);
    return rc == 1;
}
