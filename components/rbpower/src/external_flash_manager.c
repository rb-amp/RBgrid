/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file external_flash_manager.c
 * @brief Storage medium selection: the internal flash always holds the config; the statistics rings
 *        live on the internal `stats` partition, or on an external SPI NOR chip when one is fitted.
 *
 * Boot order (external_flash_init):
 *   1. internal `stats` partition + internal `config` LittleFS — mandatory; without them the hub
 *      keeps statistics in RAM only;
 *   2. external NOR probe on SPI2 (MISO 12, MOSI 13, CLK 14, CS 15). A chip is ADOPTED for the
 *      statistics when it answers a plausible JEDEC ID and its stats region is larger than the
 *      internal one (a W25Q128 holds about 49 points with history, the internal 4 MB partition 12).
 *      Adoption = erase the chip's stats region where it is not blank, then copy the internal stats
 *      region onto it at the same offsets: the slot layout (flash_slots.json, /stats_fmt in the
 *      internal config) stays valid unchanged, so the history recorded so far moves with it.
 *      The marker /stats_media (internal config) names the adopted chip by its unique ID.
 *   3. later boots: marker + the same chip → external at once, no copy;
 *      marker + no chip → statistics RAM-only (the history is on the chip; the internal copy is
 *      stale, writing there would split it), reported as an error;
 *      marker + a DIFFERENT chip → adopted again from the internal copy (the history recorded on
 *      the old chip since its adoption stays on that chip).
 * There is no authenticity check of the chip: any W25Q-class NOR of 8 MB or more is used.
 */

#include "sdkconfig.h"
#include "external_flash_config.h"
#include "littlefs_raw.h"
#include "rbstorage_media.h"
#include "rbextflash_structures.h"   /* RBPOWER_RECORD_FORMAT_VERSION */
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "FLASH_MGR";

/** @brief LittleFS path (internal config) of the adopted-chip marker: "external <uid hex16> <size>". */
#define STATS_MEDIA_MARKER "/stats_media"
/** @brief LittleFS path of the stats record-format marker (same file as in rbextflash_operations.c). */
#define STATS_FORMAT_MARKER "/stats_fmt"

/** @brief One entry of the fixed partition table of the storage medium. */
typedef struct {
    const char* name;    ///< Partition name ("config", "stats")
    uint32_t offset;     ///< Offset in the external-layout address space, bytes
    uint32_t size;       ///< Size, bytes
    bool mounted;        ///< Filesystem mounted on this partition
    bool available;      ///< Partition verified and usable
} flash_partition_info_t;

static flash_partition_info_t g_partitions[] = {
    { .name = "config", .offset = PARTITION_CONFIG_OFFSET, .size = PARTITION_CONFIG_SIZE },
    { .name = "stats",  .offset = PARTITION_STATS_OFFSET,  .size = PARTITION_STATS_SIZE  },
};
/** @brief Number of entries in the partition table. */
#define NUM_PARTITIONS (sizeof(g_partitions)/sizeof(g_partitions[0]))

static external_flash_state_t g_flash_state = {
    .pins = {
        .miso_pin = DEFAULT_FLASH_MISO_PIN,
        .mosi_pin = DEFAULT_FLASH_MOSI_PIN,
        .sclk_pin = DEFAULT_FLASH_SCLK_PIN,
        .cs_pin = DEFAULT_FLASH_CS_PIN
    },
};

static flash_partition_info_t *part(const char *name)
{
    for (size_t i = 0; i < NUM_PARTITIONS; i++) if (strcmp(g_partitions[i].name, name) == 0) return &g_partitions[i];
    return NULL;
}

/** Bootstrap of the config LittleFS: the backup directory and a default /config.json on a fresh partition. */
static void create_config_file_structure(void)
{
    littlefs_raw_mkdir("/backup");
    const char* config_path = "/config.json";
    if (!littlefs_raw_exists(config_path)) {
        const char* default_config =
            "{"
            "\"system\":{\"version\":\"1.0.0\",\"device_id\":\"ESP32\",\"initialized\":false,"
                       "\"home_name\":\"Default\",\"location\":\"Unknown\",\"home_enabled\":false},"
            "\"hardware\":{\"channels\":2},"
            "\"network\":{\"wifi_ssid\":\"\",\"ap_enabled\":false},"
            "\"telemetry\":{\"interval\":60,\"enabled\":false},"
            "\"tariffs\":{\"zones\":[],\"active\":false},"
            "\"sensors\":{\"count\":0},"
            "\"units\":{\"power\":\"kW\",\"energy\":\"kWh\"}"
            "}";
        if (littlefs_raw_write(config_path, default_config, strlen(default_config)) != ESP_OK)
            ESP_LOGE(TAG, "Failed to create %s", config_path);
    }
}

/* ================================================================================================
 * EXTERNAL CHIP ADOPTION
 * ================================================================================================ */

static bool all_ff(const uint8_t *b, size_t n)
{
    const uint32_t *w = (const uint32_t *)b;
    for (size_t i = 0; i < n / 4; i++) if (w[i] != 0xFFFFFFFFu) return false;
    return true;
}

/** Erase the chip's stats region [STATS_OFFSET, STATS_OFFSET+size) where it is not blank (a new chip
 *  costs one read pass, about 7 s for 16 MB at 20 MHz; a used one also its block erases). */
static esp_err_t ext_clean_region(esp_flash_t *h, uint32_t size, uint8_t *buf)
{
    uint32_t addr = PARTITION_STATS_OFFSET, end = PARTITION_STATS_OFFSET + size, erased = 0;
    while (addr < end) {
        uint32_t chunk = 0x10000u - (addr & 0xFFFFu);          /* up to the next 64 KB boundary */
        if (chunk > end - addr) chunk = end - addr;
        bool blank = true;
        for (uint32_t o = 0; o < chunk && blank; o += 4096) {
            esp_err_t e = esp_flash_read(h, buf, addr + o, 4096);
            if (e != ESP_OK) return e;
            blank = all_ff(buf, 4096);
        }
        if (!blank) {
            esp_err_t e = esp_flash_erase_region(h, addr, chunk);
            if (e != ESP_OK) return e;
            erased += chunk;
        }
        addr += chunk;
        vTaskDelay(1);
    }
    ESP_LOGI(TAG, "external stats region checked: %lu KB erased", (unsigned long)(erased / 1024));
    return ESP_OK;
}

/** Copy the internal stats region onto the chip at the same offsets (blank sectors skipped), verify
 *  each written sector. *any = at least one non-blank sector was copied. */
static esp_err_t ext_copy_internal(esp_flash_t *h, uint32_t ext_size, uint8_t *buf, uint8_t *chk, bool *any)
{
    *any = false;
    uint32_t isz = rbmedia_stats_region_size(), copied = 0;
    for (uint32_t off = 0; off < isz; off += 4096) {
        esp_err_t e = rbmedia_stats_read(PARTITION_STATS_OFFSET + off, buf, 4096);
        if (e != ESP_OK) return e;
        if (all_ff(buf, 4096)) continue;
        if (off + 4096 > ext_size) {   /* only with a smaller bench chip (RBGRID_EXTFLASH_TEST_MIN_KB) */
            ESP_LOGE(TAG, "recorded history reaches %lu KB, the chip region holds %lu KB — not adopted",
                     (unsigned long)((off + 4096) / 1024), (unsigned long)(ext_size / 1024));
            return ESP_ERR_INVALID_SIZE;
        }
        e = esp_flash_write(h, buf, PARTITION_STATS_OFFSET + off, 4096);
        if (e == ESP_OK) e = esp_flash_read(h, chk, PARTITION_STATS_OFFSET + off, 4096);
        if (e != ESP_OK) return e;
        if (memcmp(buf, chk, 4096) != 0) return ESP_ERR_INVALID_CRC;
        copied += 4096;
        *any = true;
        if ((off & 0xFFFFu) == 0) vTaskDelay(1);
    }
    ESP_LOGI(TAG, "internal stats copied to the external chip: %lu KB", (unsigned long)(copied / 1024));
    return ESP_OK;
}

static bool read_marker(uint64_t *uid)
{
    char m[64] = {0};
    size_t n = 0;
    if (!littlefs_raw_exists(STATS_MEDIA_MARKER) ||
        littlefs_raw_read(STATS_MEDIA_MARKER, m, sizeof(m) - 1, &n) != ESP_OK) return false;
    m[n < sizeof(m) ? n : sizeof(m) - 1] = 0;
    unsigned long long u = 0;
    if (sscanf(m, "external %llx", &u) != 1) return false;
    *uid = (uint64_t)u;
    return true;
}

static void ext_attach(esp_flash_t *h, uint32_t id, uint32_t size)
{
    uint32_t ssz = size - PARTITION_STATS_OFFSET;
    rbmedia_attach_external(h, ssz);
    g_flash_state.flash_handle = h;
    g_flash_state.status.flash_id = id;
    g_flash_state.status.flash_size = size;
    g_flash_state.partitions.stats_size = ssz;
    part("stats")->size = ssz;
    ESP_LOGI(TAG, "statistics on the EXTERNAL chip: ID 0x%06" PRIX32 ", %" PRIu32 " MB, stats region %" PRIu32 " KB",
             id, size >> 20, ssz >> 10);
}

/** Probe the external chip and adopt it for the statistics when it is larger than the internal region. */
static void ext_stats_select(void)
{
    uint64_t want_uid = 0;
    bool want_ext = read_marker(&want_uid);

    esp_flash_t *h = NULL;
    uint32_t id = 0, size = 0;
    bool chip = false;
    if (init_external_flash() == ESP_OK && (h = get_external_flash_handle()) != NULL) {
        id = 0;
        esp_flash_read_id(h, &id);
        id &= 0xFFFFFF;
        if (esp_flash_get_physical_size(h, &size) != ESP_OK) size = 0;
        chip = id != 0 && id != 0xFFFFFF && size > PARTITION_STATS_OFFSET;
    }
    uint32_t isz = rbmedia_stats_region_size();
    bool big = chip && (size - PARTITION_STATS_OFFSET) > isz;
#if CONFIG_RBGRID_EXTFLASH_TEST_MIN_KB > 0
    if (chip && !big && (size - PARTITION_STATS_OFFSET) >= CONFIG_RBGRID_EXTFLASH_TEST_MIN_KB * 1024u) {
        ESP_LOGW(TAG, "BENCH BUILD: accepting a %" PRIu32 " KB chip smaller than the internal region", size >> 10);
        big = true;
    }
#endif

    if (!big) {
        deinit_external_flash();
        if (chip) ESP_LOGW(TAG, "external NOR ID 0x%06" PRIX32 " (%" PRIu32 " KB) is not larger than the internal "
                                "stats region — statistics stay internal", id, size >> 10);
        if (want_ext) {
            /* The history is on the adopted chip: writing the stale internal copy would split it. */
            rbmedia_detach();
            g_flash_state.status.error_message = "external statistics flash missing";
            ESP_LOGE(TAG, "the statistics chip is missing (marker %s) — statistics RAM-only until it is back",
                     STATS_MEDIA_MARKER);
        }
        return;
    }

    uint64_t uid = 0;
    if (esp_flash_read_unique_chip_id(h, &uid) != ESP_OK) uid = 0;

    if (want_ext && uid == want_uid) {
        ext_attach(h, id, size);
        return;
    }
    if (want_ext)
        ESP_LOGW(TAG, "a DIFFERENT statistics chip (uid %016" PRIx64 ", adopted %016" PRIx64 ") — adopting it from "
                      "the internal copy; the history recorded since the first adoption stays on the old chip",
                 uid, want_uid);

    /* Adoption: clean the chip's region, copy the internal one, then write the marker. A failure leaves the
     * statistics on the internal flash, untouched. */
    ESP_LOGW(TAG, "adopting external NOR ID 0x%06" PRIX32 " (%" PRIu32 " MB) for the statistics — one-time copy",
             id, size >> 20);
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint8_t *chk = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    bool any = false;
    esp_err_t e = (buf && chk) ? ESP_OK : ESP_ERR_NO_MEM;
    if (e == ESP_OK) e = ext_clean_region(h, size - PARTITION_STATS_OFFSET, buf);
    if (e == ESP_OK) e = ext_copy_internal(h, size - PARTITION_STATS_OFFSET, buf, chk, &any);
    free(buf);
    free(chk);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "adoption failed (%s) — statistics stay on the internal flash", esp_err_to_name(e));
        deinit_external_flash();
        if (want_ext) rbmedia_detach();
        g_flash_state.status.error_message = "external statistics flash adoption failed";
        return;
    }
    /* A fresh hub (nothing copied, no format marker yet): the region is clean, stamp the format so the ring
     * layer does not blind-wipe 16 MB once more. */
    if (!any && !littlefs_raw_exists(STATS_FORMAT_MARKER)) {
        uint32_t v = RBPOWER_RECORD_FORMAT_VERSION;
        littlefs_raw_write(STATS_FORMAT_MARKER, &v, sizeof(v));
    }
    char m[64];
    int n = snprintf(m, sizeof(m), "external %016" PRIx64 " %" PRIu32, uid, size);
    e = littlefs_raw_write(STATS_MEDIA_MARKER, m, (size_t)n);
    if (e != ESP_OK) ESP_LOGE(TAG, "marker %s not written (%s) — the copy repeats next boot", STATS_MEDIA_MARKER,
                              esp_err_to_name(e));
    ext_attach(h, id, size);
}

/* ================================================================================================
 * PUBLIC
 * ================================================================================================ */

/* Select the storage medium (documented in external_flash_config.h). */
esp_err_t external_flash_init(void)
{
    if (rbmedia_init_internal() != ESP_OK) {
        g_flash_state.status.error_message = "internal stats partition missing (old partition table)";
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t lfs = littlefs_raw_mount_internal();
    if (lfs != ESP_OK) {
        ESP_LOGE(TAG, "internal littlefs mount failed: %s", esp_err_to_name(lfs));
        g_flash_state.status.error_message = "internal config littlefs mount failed";
        return lfs;
    }
    g_flash_state.flash_handle = NULL;
    g_flash_state.status.flash_id = 0;            /* 0 = internal media */
    esp_flash_get_size(esp_flash_default_chip, &g_flash_state.status.flash_size);
    g_flash_state.status.filesystem_mounted = true;
    g_flash_state.partitions.stats_size = rbmedia_stats_region_size();
    const esp_partition_t *cfg = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "config");
    g_flash_state.partitions.config_size = cfg ? cfg->size : 0;
    /* "config" MUST be marked mounted — every rbgrid_config_storage save/load path gates on
     * external_flash_is_config_mounted(). */
    part("stats")->size = g_flash_state.partitions.stats_size;
    part("stats")->available = true;
    part("config")->size = g_flash_state.partitions.config_size;
    part("config")->available = true;
    part("config")->mounted = true;
    create_config_file_structure();

    ext_stats_select();

    g_flash_state.status.initialized = true;
    ESP_LOGI(TAG, "storage: config INTERNAL %u KB, statistics %s %u KB",
             (unsigned)(g_flash_state.partitions.config_size / 1024),
             rbmedia_mode() == RBMEDIA_EXTERNAL ? "EXTERNAL" : rbmedia_mode() == RBMEDIA_INTERNAL ? "INTERNAL" : "NONE (RAM-only)",
             (unsigned)(g_flash_state.partitions.stats_size / 1024));
    return ESP_OK;
}

/* Deinitialize (documented in external_flash_config.h). */
esp_err_t external_flash_deinit(void)
{
    if (g_flash_state.status.filesystem_mounted) {
        littlefs_raw_unmount();
        g_flash_state.status.filesystem_mounted = false;
        part("config")->mounted = false;
    }
    if (g_flash_state.flash_handle) deinit_external_flash();
    g_flash_state.flash_handle = NULL;
    g_flash_state.status.initialized = false;
    return ESP_OK;
}

external_flash_state_t* external_flash_get_state(void) { return &g_flash_state; }

bool external_flash_is_available(void) { return g_flash_state.status.initialized; }

bool external_flash_is_partition_available(const char* partition_name)
{
    flash_partition_info_t *p = partition_name ? part(partition_name) : NULL;
    return p && p->available;
}

bool external_flash_get_partition_info(const char* partition_name, uint32_t* offset, uint32_t* size)
{
    flash_partition_info_t *p = partition_name ? part(partition_name) : NULL;
    if (!p) return false;
    if (offset) *offset = p->offset;
    if (size) *size = p->size;
    return p->available;
}

const char* external_flash_get_config_mount_point(void)
{
    return g_flash_state.status.filesystem_mounted ? "/config" : NULL;
}

bool external_flash_is_config_mounted(void)
{
    flash_partition_info_t *p = part("config");
    return p && p->mounted;
}
