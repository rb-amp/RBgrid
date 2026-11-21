/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbstorage_media.c
 * @brief Storage media routing (internal `stats` partition or external NOR); see rbstorage_media.h.
 */
#include "rbstorage_media.h"
#include "partition_layout.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rbhome_types.h"   /* rbpower_diag_printf: FAIL line in the event log */

static const char *TAG = "rbmedia";

static rbmedia_mode_t         s_mode = RBMEDIA_NONE;
static const esp_partition_t *s_stats_part = NULL;   /* internal mode */
static esp_flash_t           *s_ext_handle = NULL;   /* external mode (optional) */
static uint32_t               s_ext_size = 0;        /* external stats region size, bytes */

/* Drop-out watch (external mode only). The chip is picked at boot; if it stops answering while the hub runs,
 * the IO shims stop touching it and stats_media reads "none" until the next start, which finds it again (or
 * not). Two triggers: an IO error whose follow-up probe fails, or 3 failed probes in a row (one per 10 s, from
 * the statistics task, which is also the task that writes the history). Mode and handle stay as they are:
 * a NULL esp_flash handle means the MAIN chip to esp_flash_*, so it must never reach them. */
#define RBMEDIA_PROBE_PERIOD_US  (10LL * 1000 * 1000)
#define RBMEDIA_PROBE_MISSES     3
static volatile bool s_ext_lost = false;
static uint64_t      s_ext_uid = 0;      /* unique ID at attach; 0 = none readable, the JEDEC ID is compared */
static uint32_t      s_ext_jedec = 0;
static uint8_t       s_probe_miss = 0;
static int64_t       s_next_probe_us = 0;

/** @brief TRUE when the attached chip answers with the identity it had at attach. */
static bool ext_answers(esp_flash_t *h)
{
    if (s_ext_uid) {
        uint64_t u = 0;
        return esp_flash_read_unique_chip_id(h, &u) == ESP_OK && u == s_ext_uid;
    }
    uint32_t id = 0;
    return esp_flash_read_id(h, &id) == ESP_OK && (id & 0xFFFFFFu) == s_ext_jedec;
}

/** @brief Stop using the external chip for the rest of this boot. */
static void ext_mark_lost(const char *why)
{
    if (s_ext_lost) return;
    s_ext_lost = true;
    ESP_LOGE(TAG, "statistics chip stopped responding (%s) — statistics RAM-only until the hub restarts with it", why);
    rbpower_diag_printf("FAIL storage: statistics chip stopped responding (%s); history not shown or saved until "
                        "the hub restarts with the chip", why);
}

/** @brief After a failed external IO: probe once; a chip that does not answer is lost at once. */
static void ext_io_failed(esp_flash_t *h, esp_err_t e)
{
    if (!ext_answers(h)) ext_mark_lost(esp_err_to_name(e));
}

/** @brief The external handle while the chip is usable, else NULL (the caller returns ESP_ERR_INVALID_STATE). */
static inline esp_flash_t *ext_usable(void)
{
    return s_ext_lost ? NULL : s_ext_handle;
}

esp_err_t rbmedia_init_internal(void)
{
    if (s_mode == RBMEDIA_INTERNAL) return ESP_OK;
    s_stats_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                            ESP_PARTITION_SUBTYPE_ANY, "stats");
    if (!s_stats_part) {
        ESP_LOGE(TAG, "internal `stats` partition not found — storage stays RAM-only "
                      "(old partition table? full reflash with the new layout required)");
        return ESP_ERR_NOT_FOUND;
    }
    s_mode = RBMEDIA_INTERNAL;
    ESP_LOGI(TAG, "media = INTERNAL: stats partition @0x%06lX, %lu KB",
             (unsigned long)s_stats_part->address, (unsigned long)(s_stats_part->size / 1024));
    return ESP_OK;
}

esp_err_t rbmedia_attach_external(esp_flash_t *handle, uint32_t region_size)
{
    if (!handle || !region_size) return ESP_ERR_INVALID_ARG;
    s_ext_handle = handle;
    s_ext_size = region_size;
    s_ext_lost = false;
    s_probe_miss = 0;
    if (esp_flash_read_unique_chip_id(handle, &s_ext_uid) != ESP_OK || s_ext_uid == UINT64_MAX) s_ext_uid = 0;
    s_ext_jedec = 0;
    if (esp_flash_read_id(handle, &s_ext_jedec) == ESP_OK) s_ext_jedec &= 0xFFFFFFu;
    s_next_probe_us = esp_timer_get_time() + RBMEDIA_PROBE_PERIOD_US;
    s_mode = RBMEDIA_EXTERNAL;
    ESP_LOGI(TAG, "media = EXTERNAL NOR, stats region %lu KB", (unsigned long)(region_size / 1024));
    return ESP_OK;
}

void rbmedia_detach(void)
{
    s_ext_handle = NULL;
    s_ext_size = 0;
    s_mode = RBMEDIA_NONE;
    ESP_LOGW(TAG, "media = NONE: statistics RAM-only");
}

rbmedia_mode_t rbmedia_mode(void) { return s_mode; }

const char *rbmedia_stats_media_str(void)
{
    if (s_mode == RBMEDIA_EXTERNAL) return s_ext_lost ? "none" : "external";
    return s_mode == RBMEDIA_INTERNAL ? "internal" : "none";
}

bool rbmedia_ext_lost(void) { return s_mode == RBMEDIA_EXTERNAL && s_ext_lost; }

void rbmedia_watch_tick(void)
{
    if (s_mode != RBMEDIA_EXTERNAL || s_ext_lost || !s_ext_handle) return;
    int64_t now = esp_timer_get_time();
    if (now < s_next_probe_us) return;
    s_next_probe_us = now + RBMEDIA_PROBE_PERIOD_US;
    if (ext_answers(s_ext_handle)) { s_probe_miss = 0; return; }
    if (++s_probe_miss >= RBMEDIA_PROBE_MISSES) ext_mark_lost("no answer to 3 identity reads");
    else ESP_LOGW(TAG, "statistics chip identity read failed (%u of %u)", s_probe_miss, RBMEDIA_PROBE_MISSES);
}

uint32_t rbmedia_stats_region_size(void)
{
    if (s_mode == RBMEDIA_INTERNAL) return (uint32_t)s_stats_part->size;
    if (s_mode == RBMEDIA_EXTERNAL) return s_ext_size;
    return 0;
}

/* abs (external-layout) → internal-partition-relative, with bounds check */
static inline esp_err_t xlat(uint32_t abs_off, size_t len, uint32_t *rel)
{
    if (abs_off < PARTITION_STATS_OFFSET) return ESP_ERR_INVALID_ARG;
    uint32_t r = abs_off - PARTITION_STATS_OFFSET;
    if (!s_stats_part || r + len > s_stats_part->size) return ESP_ERR_INVALID_SIZE;
    *rel = r;
    return ESP_OK;
}

esp_err_t rbmedia_stats_read(uint32_t abs_off, void *dst, size_t len)
{
    if (s_mode == RBMEDIA_EXTERNAL) {
        esp_flash_t *h = ext_usable();
        if (!h) return ESP_ERR_INVALID_STATE;
        esp_err_t x = esp_flash_read(h, dst, abs_off, len);
        if (x != ESP_OK) ext_io_failed(h, x);
        return x;
    }
    if (s_mode != RBMEDIA_INTERNAL) return ESP_ERR_INVALID_STATE;
    uint32_t rel; esp_err_t e = xlat(abs_off, len, &rel);
    return (e == ESP_OK) ? esp_partition_read(s_stats_part, rel, dst, len) : e;
}

esp_err_t rbmedia_stats_write(uint32_t abs_off, const void *src, size_t len)
{
    if (s_mode == RBMEDIA_EXTERNAL) {
        esp_flash_t *h = ext_usable();
        if (!h) return ESP_ERR_INVALID_STATE;
        esp_err_t x = esp_flash_write(h, src, abs_off, len);
        if (x != ESP_OK) ext_io_failed(h, x);
        return x;
    }
    if (s_mode != RBMEDIA_INTERNAL) return ESP_ERR_INVALID_STATE;
    uint32_t rel; esp_err_t e = xlat(abs_off, len, &rel);
    return (e == ESP_OK) ? esp_partition_write(s_stats_part, rel, src, len) : e;
}

esp_err_t rbmedia_stats_erase(uint32_t abs_off, size_t len)
{
    if (s_mode == RBMEDIA_EXTERNAL) {
        esp_flash_t *h = ext_usable();
        if (!h) return ESP_ERR_INVALID_STATE;
        esp_err_t x = esp_flash_erase_region(h, abs_off, len);
        if (x != ESP_OK) ext_io_failed(h, x);
        return x;
    }
    if (s_mode != RBMEDIA_INTERNAL) return ESP_ERR_INVALID_STATE;
    uint32_t rel; esp_err_t e = xlat(abs_off, len, &rel);
    return (e == ESP_OK) ? esp_partition_erase_range(s_stats_part, rel, len) : e;
}

esp_flash_t *rbmedia_stats_handle_token(void)
{
    if (s_mode == RBMEDIA_EXTERNAL) return ext_usable();
    if (s_mode == RBMEDIA_INTERNAL) return esp_flash_default_chip;  /* token only, never dereferenced */
    return NULL;
}
