/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbllm.c
 * @brief LLM protocol v1: tool registry → manifest, call envelope, LLM tokens + pairing, call log, worker task.
 *
 * Contract: documents/api/LLM_PROTOCOL_V1.md (hello_world). The hub computes, the model reads: every tool returns
 * finished aggregates with units. Stage 1 = read tools only; the config tools (propose/apply/rollback) come with
 * stage 2 and need the config-version store.
 *
 * Threads: rbllm_auth / pairing / admin run in the transport's task (httpd); calls run on the worker task. Shared
 * state (tokens, pairings, log) is guarded by s_mx.
 */
#include "rbllm.h"
#include "rbgrid_core.h"
#include "rbgrid_core_priv.h"
#include "rbgrid_tz.h"
#include "rbhome_types.h"
#include "rbhome_tariff.h"
#include "rbpower.h"
#include "rbjson.h"
#include "rbdynprice.h"
#include "rbactivation.h"
#include "rbpeak.h"
#include "rbgrid_config_storage.h"   /* rbgrid_storage_save_tariffs */
#include "rbextflash_operations.h"   /* rbpower_flash_unit_has_history */
#include "littlefs_raw.h"
#include "rbamp.h"                   /* rbamp_basic_fw_name */
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "rbmqtt.h"                  /* broker state for get_config integrations */
#include "rbgrid_config_api.h"     /* rbgrid_system_reboot */
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

static const char *TAG = "RBLLM";

#define RBLLM_FILE          "/llm.json"
#define RBLLM_MAX_TOKENS    8
#define RBLLM_MAX_PAIRS     2
#define RBLLM_PAIR_TTL_S    600
#define RBLLM_LOG_N         200
#define RBLLM_LAST_USED_PERSIST_S 3600   /* last-used time reaches flash at most hourly per token */

/* ============================================================================================ state */

typedef struct {
    char     id[9];        /* 8 hex */
    char     name[33];     /* client name (data, never shown in a tool description) */
    uint8_t  scopes;
    char     hash[65];     /* sha256 hex of the token */
    uint32_t created;      /* epoch s (0 = clock unset at pairing) */
    uint32_t last_used;    /* epoch s, RAM value */
    uint32_t last_saved;   /* last_used as persisted */
} rbllm_token_t;

typedef struct {
    char     pair_id[17];
    char     client[33];
    uint8_t  scopes;
    char     code[7];
    int64_t  exp_us;
    uint8_t  fails;
} rbllm_pair_t;

typedef struct {
    uint32_t ts;
    char     client[33];   /* the whole client name (≤ 32 B, as the token's) */
    char     tool[28];
    char     code[20];     /* "ok" or the error code */
    uint16_t ms;
    uint16_t bytes;
    bool     cfg;
} rbllm_log_t;

/* PSRAM (.ext_ram.bss): ~1.6 KB kept out of internal RAM; touched only from tasks, under s_mx */
static EXT_RAM_BSS_ATTR struct {
    bool          loaded;
    bool          enabled;
    bool          read_only;
    rbllm_token_t tok[RBLLM_MAX_TOKENS];
    rbllm_pair_t  pair[RBLLM_MAX_PAIRS];
    uint32_t      pair_fail_n;      /* failed codes in the current 10-min window */
    int64_t       pair_fail_win_us;
    int64_t       pair_block_until_us;
    rbllm_log_t  *log;              /* ring in PSRAM */
    uint16_t      log_head, log_n;
} S;

static SemaphoreHandle_t s_mx;
static StaticSemaphore_t s_mx_buf;
static char *(*s_events_reader)(uint32_t, uint32_t, int) = NULL;

void rbllm_register_events_reader(char *(*fn)(uint32_t, uint32_t, int)) { s_events_reader = fn; }

/* I2C module actions (rbi2c_action in rbgrid_app): the module-setup tools go through the same validated path as the
 * hub page's Add-a-module flow (bus/address checks, UID-confirmed address change). */
static char *(*s_i2c_action)(const struct cJSON *, int *);
void rbllm_register_i2c_action(char *(*fn)(const struct cJSON *, int *)) { s_i2c_action = fn; }
bool rbwebserver_ui_lang_code(char out[3]);   /* rbwebserver_ui.c */
bool rbgrid_ap_user_password(char *out, size_t n);   /* rbwebserver_wifi.c: true = the user set the AP password */
bool rbwebserver_ui_kit_version(char *out, size_t n);   /* rbwebserver_ui.c */
static int (*s_modfw_status)(char *, size_t);
void rbllm_register_modfw_status(int (*fn)(char *out, size_t cap)) { s_modfw_status = fn; }
/* Run one op; returns the parsed reply (caller deletes) and *st; NULL when unavailable. */
static cJSON *i2c_do(const char *op, int bus, int module, int addr, int *st)
{
    *st = 500;
    if (!s_i2c_action) return NULL;
    cJSON *q = cJSON_CreateObject();
    if (!q) return NULL;
    cJSON_AddStringToObject(q, "op", op);
    if (bus >= 0) cJSON_AddNumberToObject(q, "bus", bus);
    if (module >= 0) cJSON_AddNumberToObject(q, "module", module);
    if (addr >= 0) cJSON_AddNumberToObject(q, "addr", addr);
    char *out = s_i2c_action(q, st);
    cJSON_Delete(q);
    cJSON *r = out ? cJSON_Parse(out) : NULL;
    free(out);
    return r;
}

static void lk(void)  { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); }
static void ulk(void) { if (s_mx) xSemaphoreGive(s_mx); }
static void ev_boot(void);
static void ev_watch(void);
static void ev_add(uint8_t type, uint8_t sev, uint16_t chan, const char *info);
static void ensure_task(void);
static void task_timer_cb(void *arg);
static esp_timer_handle_t s_task_timer;
static bool zone_window(const rbpower_tariff_zone_t *zo, char out[12]);

static uint32_t now_s(void) { return (uint32_t)time(NULL); }

static void hex_of(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = H[b[i] >> 4]; out[2 * i + 1] = H[b[i] & 15]; }
    out[2 * n] = '\0';
}
static void sha256_hex(const char *s, char out[65])
{
    uint8_t h[32];
    mbedtls_sha256((const unsigned char *)s, strlen(s), h, 0);
    hex_of(h, 32, out);
}
static void rand_hex(char *out, size_t nbytes)
{
    uint8_t b[24];
    if (nbytes > sizeof(b)) nbytes = sizeof(b);
    esp_fill_random(b, nbytes);
    hex_of(b, nbytes, out);
}

static uint8_t scopes_from_json(const cJSON *a)
{
    uint8_t s = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, a) {
        const char *v = cJSON_GetStringValue(e);
        if (!v) continue;
        if (!strcmp(v, "read")) s |= RBLLM_SCOPE_READ;
        else if (!strcmp(v, "config")) s |= RBLLM_SCOPE_CONFIG;
        /* "admin" is not granted by pairing: the token list is the hub page's, not a client's */
    }
    return s;
}
static void scopes_to_json(cJSON *parent, const char *key, uint8_t s)
{
    cJSON *a = cJSON_AddArrayToObject(parent, key);
    if (s & RBLLM_SCOPE_READ)   cJSON_AddItemToArray(a, cJSON_CreateString("read"));
    if (s & RBLLM_SCOPE_CONFIG) cJSON_AddItemToArray(a, cJSON_CreateString("config"));
    if (s & RBLLM_SCOPE_ADMIN)  cJSON_AddItemToArray(a, cJSON_CreateString("admin"));
}

/* Persist enabled/read_only + the tokens (hashes only). Called with s_mx held. */
static esp_err_t save_locked(void)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(r, "v", 1);
    cJSON_AddBoolToObject(r, "enabled", S.enabled);
    cJSON_AddBoolToObject(r, "read_only", S.read_only);
    cJSON *a = cJSON_AddArrayToObject(r, "tokens");
    for (int i = 0; i < RBLLM_MAX_TOKENS; i++) {
        rbllm_token_t *t = &S.tok[i];
        if (!t->id[0]) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", t->id);
        cJSON_AddStringToObject(o, "name", t->name);
        scopes_to_json(o, "scopes", t->scopes);
        cJSON_AddStringToObject(o, "h", t->hash);
        cJSON_AddNumberToObject(o, "created", t->created);
        cJSON_AddNumberToObject(o, "last", t->last_used);
        t->last_saved = t->last_used;
        cJSON_AddItemToArray(a, o);
    }
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!s) return ESP_ERR_NO_MEM;
    esp_err_t e = littlefs_raw_write(RBLLM_FILE, s, strlen(s));
    free(s);
    if (e != ESP_OK) rbpower_diag_printf("FAIL llm: %s not saved (%s)", RBLLM_FILE, esp_err_to_name(e));
    return e;
}

void rbllm_init(void)
{
    if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
    lk();
    if (!S.log) S.log = heap_caps_calloc(RBLLM_LOG_N, sizeof(rbllm_log_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (S.loaded) { ulk(); return; }
    S.loaded = true;
    size_t sz = 0;
    if (littlefs_raw_is_mounted() && littlefs_raw_size(RBLLM_FILE, &sz) == ESP_OK && sz > 0 && sz < 8192) {
        char *b = malloc(sz + 1);
        size_t got = 0;
        if (b && littlefs_raw_read(RBLLM_FILE, b, sz, &got) == ESP_OK) {
            b[got] = '\0';
            cJSON *r = cJSON_Parse(b);
            if (r) {
                S.enabled = cJSON_IsTrue(cJSON_GetObjectItem(r, "enabled"));
                S.read_only = cJSON_IsTrue(cJSON_GetObjectItem(r, "read_only"));
                int k = 0;
                const cJSON *o;
                cJSON_ArrayForEach(o, cJSON_GetObjectItem(r, "tokens")) {
                    if (k >= RBLLM_MAX_TOKENS) break;
                    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(o, "id"));
                    const char *h = cJSON_GetStringValue(cJSON_GetObjectItem(o, "h"));
                    if (!id || !h || strlen(h) != 64) continue;
                    rbllm_token_t *t = &S.tok[k++];
                    strlcpy(t->id, id, sizeof(t->id));
                    strlcpy(t->hash, h, sizeof(t->hash));
                    const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(o, "name"));
                    strlcpy(t->name, nm ? nm : "", sizeof(t->name));
                    t->scopes = scopes_from_json(cJSON_GetObjectItem(o, "scopes"));
                    t->created = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "created"));
                    t->last_used = t->last_saved = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "last"));
                }
                cJSON_Delete(r);
            } else {
                rbpower_diag_printf("FAIL llm: %s unreadable — LLM API stays off, tokens dropped", RBLLM_FILE);
            }
        }
        free(b);
    }
    ulk();
    ESP_LOGI(TAG, "LLM API %s", S.enabled ? "on" : "off");
    ev_boot();
    /* The worker (it also runs the event watcher) starts 30 s after the hub, or at the first call if one comes
     * sooner: its 12 KB stack is internal RAM, and in the first seconds Wi-Fi briefly needs that heap
     * (WROVER 22.11: with the stack taken at boot the rbAmp poll task could not be created). */
    if (!s_task_timer) {
        const esp_timer_create_args_t ta = { .callback = task_timer_cb, .name = "rbllm_start" };
        if (esp_timer_create(&ta, &s_task_timer) == ESP_OK) esp_timer_start_periodic(s_task_timer, 30LL * 1000 * 1000);
        else ensure_task();
    }
}

bool rbllm_enabled(void) { return S.enabled; }

int rbllm_pair_pending(void)
{
    if (!S.enabled || !s_mx) return 0;
    int n = 0;
    lk();
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < RBLLM_MAX_PAIRS; i++) if (S.pair[i].pair_id[0] && now <= S.pair[i].exp_us) n++;
    ulk();
    return n;
}

bool rbllm_auth(const char *token, rbllm_caller_t *out)
{
    if (!token || strncmp(token, "rbl_", 4) != 0 || strlen(token) > 80) return false;
    char h[65];
    sha256_hex(token, h);
    bool ok = false;
    lk();
    for (int i = 0; i < RBLLM_MAX_TOKENS; i++) {
        rbllm_token_t *t = &S.tok[i];
        if (!t->id[0]) continue;
        /* constant-time compare over the whole hash */
        uint8_t d = 0;
        for (int j = 0; j < 64; j++) d |= (uint8_t)(t->hash[j] ^ h[j]);
        if (d) continue;
        ok = true;
        if (out) {
            out->scopes = t->scopes;
            strlcpy(out->client, t->name, sizeof(out->client));
            strlcpy(out->token_id, t->id, sizeof(out->token_id));
        }
        uint32_t n = now_s();
        if (n > RBGRID_TS_SANE_MIN) {
            t->last_used = n;
            if (n - t->last_saved > RBLLM_LAST_USED_PERSIST_S) (void)save_locked();
        }
        break;
    }
    ulk();
    return ok;
}

void rbllm_hub_id(char out[13])
{
    uint8_t m[6] = {0};
    esp_efuse_mac_get_default(m);
    hex_of(m, 6, out);
}

/* ============================================================================================ time */

/* days since 1970-01-01 of a civil date (Howard Hinnant) */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static int64_t civil_seconds(const struct tm *t)
{
    return days_from_civil(t->tm_year + 1900, (unsigned)t->tm_mon + 1, (unsigned)t->tm_mday) * 86400 +
           t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

/* ISO 8601 with the hub's offset: 2026-11-19T10:15:00+01:00 */
static void iso_time(uint32_t ts, char out[48])
{
    time_t t = (time_t)ts;
    struct tm lt;
    rbgrid_localtime_r(&t, &lt);
    int off = (int)(civil_seconds(&lt) - (int64_t)ts);   /* seconds east of UTC */
    char sg = off < 0 ? '-' : '+';
    if (off < 0) off = -off;
    snprintf(out, 48, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", (lt.tm_year + 1900) % 10000, (lt.tm_mon + 1) % 100,
             lt.tm_mday % 100, lt.tm_hour % 100, lt.tm_min % 100, lt.tm_sec % 100, sg, (off / 3600) % 100, (off % 3600) / 60);
}
static void add_time(cJSON *o, const char *k, uint32_t ts) { char b[48]; iso_time(ts, b); cJSON_AddStringToObject(o, k, b); }

/* Parse "YYYY-MM-DD[THH:MM[:SS]][Z|±HH:MM]". No offset = hub local time. */
static bool parse_iso(const char *s, uint32_t *out)
{
    if (!s) return false;
    int y, mo, d, h = 0, mi = 0, se = 0, n = 0;
    if (sscanf(s, "%4d-%2d-%2d%n", &y, &mo, &d, &n) != 3) return false;
    const char *p = s + n;
    if (*p == 'T' || *p == ' ') {
        int n2 = 0;
        if (sscanf(p + 1, "%2d:%2d%n", &h, &mi, &n2) != 2) return false;
        p += 1 + n2;
        if (*p == ':') { int n3 = 0; if (sscanf(p + 1, "%2d%n", &se, &n3) != 1) return false; p += 1 + n3; }
        if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
    }
    if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 24 || mi > 59 || se > 60) return false;
    struct tm t = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = se, .tm_isdst = -1 };
    if (*p == '\0') { *out = (uint32_t)rbgrid_mktime(&t); return true; }
    int oh = 0, om = 0, sign = 0;
    if (*p == 'Z' && p[1] == '\0') sign = 1;
    else if ((*p == '+' || *p == '-') && sscanf(p + 1, "%2d:%2d", &oh, &om) == 2) sign = *p == '+' ? 1 : -1;
    else return false;
    int64_t v = civil_seconds(&t) - sign * (oh * 3600 + om * 60);
    if (v < 0) return false;
    *out = (uint32_t)v;
    return true;
}

/* Local calendar starts */
static uint32_t local_floor(uint32_t ts, char unit)
{
    time_t t = (time_t)ts;
    struct tm lt;
    rbgrid_localtime_r(&t, &lt);
    lt.tm_sec = 0; lt.tm_min = 0;
    if (unit == 'h') { lt.tm_isdst = -1; return (uint32_t)rbgrid_mktime(&lt); }
    lt.tm_hour = 0;
    if (unit == 'w') lt.tm_mday -= (lt.tm_wday + 6) % 7;   /* weeks start on Monday */
    if (unit == 'm' || unit == 'y') lt.tm_mday = 1;
    if (unit == 'y') lt.tm_mon = 0;
    lt.tm_isdst = -1;
    return (uint32_t)rbgrid_mktime(&lt);
}
static uint32_t local_add(uint32_t ts, char unit, int n)
{
    time_t t = (time_t)ts;
    struct tm lt;
    rbgrid_localtime_r(&t, &lt);
    if (unit == 'h') return ts + (uint32_t)(n * 3600);
    if (unit == 'd') lt.tm_mday += n;
    else if (unit == 'w') lt.tm_mday += 7 * n;
    else if (unit == 'm') lt.tm_mon += n;
    else if (unit == 'y') lt.tm_year += n;
    lt.tm_isdst = -1;
    return (uint32_t)rbgrid_mktime(&lt);
}

/* The same local wall time one calendar month earlier; a day past that month's end clamps to its last day
 * (31 Mar -> 28/29 Feb), so a month baseline never runs into the month it is compared with. */
static uint32_t local_month_back(uint32_t ts)
{
    time_t t = (time_t)ts;
    struct tm lt;
    rbgrid_localtime_r(&t, &lt);
    lt.tm_mon -= 1;
    if (lt.tm_mon < 0) { lt.tm_mon += 12; lt.tm_year -= 1; }
    static const int DM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int y = lt.tm_year + 1900;
    int dm = DM[lt.tm_mon] + (lt.tm_mon == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
    if (lt.tm_mday > dm) lt.tm_mday = dm;
    lt.tm_isdst = -1;
    return (uint32_t)rbgrid_mktime(&lt);
}

/* ============================================================================================ call context */

typedef struct {
    int64_t  deadline_us;
    uint8_t  scopes;
    const char *client;
    bool internal;         /* the hub page (RBLLM_CLIENT_WEB, admin token): not subject to read-only mode */
    const char *token_id;  /* the caller's token id ("" for the page) — forget_me */
    /* meta */
    uint32_t as_of;
    const char *resolution;
    bool truncated, gaps, estimated;
    cJSON *notes;          /* array or NULL */
    /* error */
    bool open_hour;        /* the answer includes the open hour's closed 15-min periods */
    int  retry_ms;         /* error.retry_after_ms (0 = none) */
    bool open_cost_exact;  /* the open hour's main-supply cost came from the hub's day figure, not an estimate */
    const char *err_code;
    char err_msg[160];
    char err_hint[160];
} rbllm_cx_t;

static void note(rbllm_cx_t *cx, const char *s)
{
    if (!cx->notes) cx->notes = cJSON_CreateArray();
    if (!cx->notes || cJSON_GetArraySize(cx->notes) >= 6) return;
    const cJSON *e;
    cJSON_ArrayForEach(e, cx->notes) if (!strcmp(cJSON_GetStringValue(e), s)) return;
    cJSON_AddItemToArray(cx->notes, cJSON_CreateString(s));
}
static cJSON *fail(rbllm_cx_t *cx, const char *code, const char *msg, const char *hint)
{
    cx->err_code = code;
    strlcpy(cx->err_msg, msg ? msg : "", sizeof(cx->err_msg));
    strlcpy(cx->err_hint, hint ? hint : "", sizeof(cx->err_hint));
    return NULL;
}
static bool late(const rbllm_cx_t *cx) { return esp_timer_get_time() > cx->deadline_us; }

/* ============================================================================================ point helpers */
static void ct_short(const char *desc, char out[24]);
static int ver_sync(const char *source, const char *summary);
/* ---- tariff zone helpers: zones are named by their TYPE (zone_id), never by their slot in the plan ---- */
static const char *const ZONE_N[RBPOWER_TARIFF_ZONE_COUNT] = { "night", "morning_peak", "day", "evening_peak", "late_evening", "weekend", "holiday", "critical_peak" };
static int zone_idx(const char *s)
{
    for (int i = 0; s && i < RBPOWER_TARIFF_ZONE_COUNT; i++) if (!strcmp(s, ZONE_N[i])) return i;
    return -1;
}
static const char *zone_nm(const rbpower_tariff_zone_t *zo)
{
    int z = (int)zo->zone_id;
    return z >= 0 && z < RBPOWER_TARIFF_ZONE_COUNT ? ZONE_N[z] : "unknown";
}
static int plan_nzones(const rbpower_tariff_plan_t *tp)
{
    return tp ? (tp->active_zones_count < RBPOWER_TARIFF_ZONE_COUNT ? tp->active_zones_count : RBPOWER_TARIFF_ZONE_COUNT) : 0;
}
static const rbpower_tariff_zone_t *plan_zone(const rbpower_tariff_plan_t *tp, int zi)
{
    for (int z = 0; z < plan_nzones(tp); z++) if (tp->zones[z].is_enabled && (int)tp->zones[z].zone_id == zi) return &tp->zones[z];
    return NULL;
}
static int zone_tr_count(const rbpower_tariff_zone_t *zo)
{
    int n = 0;
    for (int j = 0; zo && j < 4 && j < zo->conditions_count; j++)
        if (zo->conditions[j].is_active && zo->conditions[j].condition_type == RBPOWER_TARIFF_CONDITION_TIME_RANGE) n++;
    return n;
}
static const char *const DAY_N[7] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
static void days_str(int mask, char out[32])   /* Mon-origin mask -> "mon,tue,..."; 0 or 0x7F -> "" (every day) */
{
    out[0] = '\0';
    if (!mask || (mask & 0x7F) == 0x7F) return;
    size_t o = 0;
    for (int d = 0; d < 7; d++) if (mask & (1 << d)) o += snprintf(out + o, 32 - o, "%s%s", o ? "," : "", DAY_N[d]);
}
static void zone_days(const rbpower_tariff_zone_t *zo, char out[32])
{
    int mask = 0;
    for (int j = 0; zo && j < 4 && j < zo->conditions_count; j++) {
        if (!zo->conditions[j].is_active) continue;
        if (zo->conditions[j].condition_type == RBPOWER_TARIFF_CONDITION_WEEKDAY) mask |= zo->conditions[j].params.weekday.weekday_mask;
        else if (zo->conditions[j].condition_type == RBPOWER_TARIFF_CONDITION_WEEKEND) mask |= 0x60;
    }
    days_str(mask, out);
}
/* "all" / "" -> 0 (every day); "mon-fri", "sat,sun", "mon,wed-fri", "weekdays", "weekend" -> mask; -1 invalid */
static int parse_days(const char *s)
{
    if (!s) return -1;
    if (!s[0] || !strcasecmp(s, "all")) return 0;
    if (!strcasecmp(s, "weekdays")) return 0x1F;
    if (!strcasecmp(s, "weekend")) return 0x60;
    char b[48]; strlcpy(b, s, sizeof(b));
    int mask = 0;
    for (char *sv = NULL, *t = strtok_r(b, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
        while (*t == ' ') t++;
        char *dash = strchr(t, '-');
        if (dash) *dash = '\0';
        int a = -1, e = -1;
        for (int d = 0; d < 7; d++) { if (!strncasecmp(t, DAY_N[d], 3) && strlen(t) == 3) a = d; if (dash && !strncasecmp(dash + 1, DAY_N[d], 3) && strlen(dash + 1) == 3) e = d; }
        if (a < 0 || (dash && e < 0)) return -1;
        if (!dash) e = a;
        for (int d = a;; d = (d + 1) % 7) { mask |= 1 << d; if (d == e) break; }
    }
    return mask;
}
/* ---- the hub's own command channel (MQTT/ESPHome path) with the admin token: one write path for every interface ---- */
static cJSON *core_cmd(const char *cmd, const char *section, const cJSON *data)
{
    cJSON *q = cJSON_CreateObject();
    cJSON_AddStringToObject(q, "cmd", cmd);
    cJSON_AddStringToObject(q, "section", section);
    if (data) cJSON_AddItemToObject(q, "data", cJSON_Duplicate(data, true));
    char adm[48];
    if (rbgrid_token_load(RBGRID_TOKEN_FILE, adm)) cJSON_AddStringToObject(q, "token", adm);
    char *in = cJSON_PrintUnformatted(q);
    cJSON_Delete(q);
    memset(adm, 0, sizeof(adm));
    char *out = in ? rbgrid_command_dispatch(in, (int)strlen(in)) : NULL;
    free(in);
    cJSON *r = out ? cJSON_Parse(out) : NULL;
    free(out);
    return r;
}
/* a cmd_ack: data.result "ok" -> true; message into m */
static bool core_ok(const cJSON *ack, char *m, size_t n)
{
    const cJSON *d = cJSON_GetObjectItem(ack, "data");
    const char *res = cJSON_GetStringValue(cJSON_GetObjectItem(d, "result")), *msg = cJSON_GetStringValue(cJSON_GetObjectItem(d, "message"));
    if (m) snprintf(m, n, "%s", msg ? msg : (ack ? "" : "no answer"));
    return res && !strcmp(res, "ok");
}
static void mqtt_saved_uri(char *out, size_t n);
static void mask_uri(const char *u, char *out, size_t n)   /* mqtts://user:pass@host -> mqtts://(stars)@host */
{
    const char *at = u ? strchr(u, '@') : NULL, *sc = u ? strstr(u, "://") : NULL;
    if (at && sc && at > sc) snprintf(out, n, "%.*s***%s", (int)(sc + 3 - u), u, at);
    else snprintf(out, n, "%s", u ? u : "");
}

#define RBLLM_MAX_POINTS 24

static const char *role_name(uint8_t r)
{
    static const char *const N[] = { "main_supply", "alternative_supply", "circuit_breaker", "device_load", "zone_load",
                                     "renewable_source", "storage_system", "ev_charging", "heating_cooling", "smart_appliance" };
    return r < sizeof(N) / sizeof(N[0]) ? N[r] : "unknown";
}
/* §8.1 role class: main / generation / bidirectional / load */
static const char *role_class(uint8_t r)
{
    switch (r) {
    case RBPOWER_ROLE_MAIN_SUPPLY: return "main";
    case RBPOWER_ROLE_ALTERNATIVE_SUPPLY:
    case RBPOWER_ROLE_RENEWABLE_SOURCE: return "generation";
    case RBPOWER_ROLE_STORAGE_SYSTEM: return "bidirectional";
    default: return "load";
    }
}

static int snapshot(rbgrid_point_snap_t *p, int max)
{
    rbpower_home_lock();
    int n = rbgrid_core_points_snapshot(p, max);
    rbpower_home_unlock();
    return n;
}

static void utf8_clean(const char *in, char *out, size_t n);
static void add_name(cJSON *o, const char *name) { char b[48]; utf8_clean(name, b, sizeof(b)); cJSON_AddStringToObject(o, "name", b); }
static void uid_hex(uint16_t u, char out[5]) { snprintf(out, 5, "%04X", (unsigned)u); }

/* `channels` arg → selection mask over the snapshot. NULL/absent = all. Unknown id → not_found. */
static bool select_points(rbllm_cx_t *cx, const cJSON *ch, const rbgrid_point_snap_t *p, int n, bool sel[])
{
    for (int i = 0; i < n; i++) sel[i] = (ch == NULL);
    if (!ch) return true;
    const cJSON *e;
    cJSON_ArrayForEach(e, ch) {
        const char *v = cJSON_GetStringValue(e);
        bool hit = false;
        for (int i = 0; v && i < n; i++) {
            char h[5]; uid_hex(p[i].unit_id, h);
            if (!strcasecmp(v, h) || (p[i].point[0] && !strcmp(v, p[i].point))) { sel[i] = true; hit = true; }
        }
        if (!hit) {
            char m[96]; snprintf(m, sizeof(m), "unknown channel '%.40s'", v ? v : "?");
            fail(cx, "not_found", m, "Call list_channels for the channel ids.");
            return false;
        }
    }
    return true;
}

/* ============================================================================================ range engine */

/* Sum of one point's records over buckets. Sources: DAY records for whole closed local days, HOUR records for the
 * partial days at the edges and for today (day records exist only for closed days). Ranges older than the hour
 * depth at the edges are clipped with a note. */
typedef struct {
    uint32_t from;     /* bucket start */
    double   e_in, e_out, cost, rev;   /* Wh, Wh, currency, currency */
    uint16_t recs;
} bucket_t;

typedef struct {
    bucket_t *b;
    int       nb;
    char      unit;     /* bucket unit h d w m y, or 't' = one total bucket */
    uint32_t  newest_end;
    uint32_t  hour_secs;   /* seconds covered by HOUR records */
    uint32_t  day_secs;
    rbllm_cx_t *cx;
    bool      timed_out;
    uint32_t  newest_hour_end;   /* end of the newest HOUR record seen (open-hour de-duplication) */
    bool      no_open;           /* leave the open hour out (exact-cost path) */
    double    open_cost, open_rev;   /* the estimate added for the open hour */
    uint32_t  want_secs;         /* the span asked for (clipped to now) */
} sum_ctx_t;
#define PARTIAL_NOTE "Part of the period has no stored data (hub off or restarting, or the point added later): the figures cover only the recorded part, not the whole period."

static int bucket_index(const sum_ctx_t *c, uint32_t ts)
{
    if (c->unit == 't') return 0;
    uint32_t b0 = local_floor(ts, c->unit == 'd' ? 'd' : c->unit);
    /* buckets ascend; binary search */
    int lo = 0, hi = c->nb - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (c->b[mid].from == b0) return mid;
        if (c->b[mid].from < b0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* Record timestamps are the period END (rbstats_v4_types.h: "ts = Period END"); a record belongs to the period that
 * ends at ts, i.e. starts at the local hour/day floor of ts - 1. A partial (open-hour) record carries the hour's end
 * too, with dur = the covered time. */
static uint32_t rec_end(const rbpower_period_statistics_t *s) { return (uint32_t)(s->period_start_us / 1000000LL); }
static uint32_t rec_start(const rbpower_period_statistics_t *s, bool is_hour) { return local_floor(rec_end(s) - 1, is_hour ? 'h' : 'd'); }
static uint32_t clamp_now(uint32_t t) { uint32_t n = now_s(); return t > n ? n : t; }

static void sum_emit(const rbpower_period_statistics_t *s, void *v, bool is_hour)
{
    sum_ctx_t *c = (sum_ctx_t *)v;
    if (c->timed_out) return;
    uint32_t ts = rec_start(s, is_hour);
    uint32_t dur = s->v4_dur_s ? s->v4_dur_s : (is_hour ? 3600u : 86400u);
    int k = bucket_index(c, ts);
    if (k < 0) return;
    bucket_t *b = &c->b[k];
    if (s->exact.valid) {
        b->e_in += s->exact.ec_mwh / 1000.0; b->e_out += s->exact.es_mwh / 1000.0;
        b->cost += s->exact.c_cents / 100.0;  b->rev += s->exact.r_cents / 100.0;
    } else {
        b->e_in += s->energy_period.consumed_wh; b->e_out += s->energy_period.supplied_wh;
        b->cost += s->result_base.cost_consumed;  b->rev += s->result_base.revenue_supplied;
    }
    b->recs++;
    uint32_t end = clamp_now(rec_end(s));
    if (end > c->newest_end) c->newest_end = end;
    if (is_hour && rec_end(s) > c->newest_hour_end) c->newest_hour_end = rec_end(s);
    if (is_hour) c->hour_secs += dur; else c->day_secs += dur;
    if (late(c->cx)) c->timed_out = true;
}
static void sum_emit_h(const rbpower_period_statistics_t *s, void *v) { sum_emit(s, v, true); }
static void sum_emit_d(const rbpower_period_statistics_t *s, void *v) { sum_emit(s, v, false); }

#define HOUR_DEPTH_S (60u * 86400u)
#define DAY_DEPTH_S  (400u * 86400u)

/* Fill c (buckets already laid out) for point u over [from, to). */
static void hour_seg(sum_ctx_t *c, uint16_t u, uint32_t a, uint32_t b, uint32_t hmin)
{
    if (a >= b || c->timed_out) return;
    if (a < hmin) {
        c->cx->gaps = true;
        note(c->cx, "Part of the range is older than the hourly history (60 days); it was left out.");
        if (b <= hmin) return;
        a = hmin;
    }
    rbgrid_foreach_valid_record_ex(u, RBPOWER_STAT_PERIOD_1HOUR, RBPOWER_PERIOD_HOUR, a + 1, local_add(local_floor(b - 1, 'h'), 'h', 1), true,
                                   c->cx->deadline_us, sum_emit_h, c);
}
static void sum_range(sum_ctx_t *c, uint16_t u, uint32_t from, uint32_t to)
{
    uint32_t now = now_s();
    if (to > now) to = now;
    if (from >= to) return;
    c->want_secs += to - from;
    if (now - from > DAY_DEPTH_S) {
        c->cx->gaps = true;
        note(c->cx, "Part of the range is older than the daily history (400 days); it was left out.");
    }
    uint32_t hmin = now > HOUR_DEPTH_S ? now - HOUR_DEPTH_S : 0;
    uint32_t today0 = local_floor(now, 'd');
    uint32_t d0 = local_floor(from, 'd'); if (d0 < from) d0 = local_add(d0, 'd', 1);   /* first whole day */
    uint32_t d1 = local_floor(to, 'd');   if (d1 > today0) d1 = today0;                 /* end of the whole closed days */
    bool whole = c->unit != 'h' && d0 < d1;   /* hourly rows never take a day record */
    if (whole) {
        rbgrid_foreach_valid_record_ex(u, RBPOWER_STAT_PERIOD_DAILY, RBPOWER_PERIOD_DAY, d0 + 1, d1, true,
                                       c->cx->deadline_us, sum_emit_d, c);
        hour_seg(c, u, from, d0, hmin);   /* partial first day */
        hour_seg(c, u, d1, to, hmin);     /* partial last day / today */
    } else {
        hour_seg(c, u, from, to, hmin);
    }
    /* the open hour (RAM): its closed 15-min periods, unless a record already covers it */
    uint32_t oh = 0, cov = 0; double oi = 0, oo = 0;
    rbpower_home_lock();
    bool have = rbpower_stats_open_hour(u, &oh, &cov, &oi, &oo);
    rbpower_home_unlock();
    if (!c->no_open && have && cov && oh < to && oh + 3600u > from && c->newest_hour_end < oh + 3600u) {
        int k = bucket_index(c, oh);
        if (k >= 0) {
            bucket_t *b = &c->b[k];
            rbgrid_tariff_now_t tn; rbgrid_core_tariff_now(&tn);
            b->e_in += oi; b->e_out += oo;
            b->cost += oi / 1000.0 * tn.buy_rate_all_in;
            b->rev += oo / 1000.0 * tn.sell_rate;
            c->open_cost += oi / 1000.0 * tn.buy_rate_all_in;
            c->open_rev += oo / 1000.0 * tn.sell_rate;
            b->recs++;
            c->hour_secs += cov;
            if (oh + cov > c->newest_end) c->newest_end = clamp_now(oh + cov);
            c->cx->open_hour = true;
        }
    }
    if (late(c->cx)) c->timed_out = true;
}

/* Records cover less of the span than asked: a day or hour the hub was off (a day record carries its covered seconds).
 * Slack: the newest 15-min period not booked yet, plus a minute per hour for record jitter. */
static bool sum_partial(const sum_ctx_t *c)
{
    uint32_t got = c->hour_secs + c->day_secs, tol = 1200u + c->want_secs / 3600u * 60u;
    return got && c->want_secs > got + tol;
}

static double r4(double v) { return round(v * 10000.0) / 10000.0; }
static double r2(double v) { return round(v * 100.0) / 100.0; }

/* Parse `period` {from,to} | {preset}. Clips `to` to now. */
static bool parse_period(rbllm_cx_t *cx, const cJSON *p, uint32_t *from, uint32_t *to)
{
    uint32_t now = now_s();
    if (now < RBGRID_TS_SANE_MIN) { fail(cx, "clock_unsynced", "the hub clock is not set yet (just restarted, or no internet for NTP)", "Retry in a few seconds; if it persists, check the hub's internet access."); cx->retry_ms = 5000; return false; }
    if (!cJSON_IsObject(p)) { fail(cx, "invalid_args", "period: object {from,to} or {preset} required", "Pass period:{preset:\"yesterday\"}."); return false; }
    const char *pre = cJSON_GetStringValue(cJSON_GetObjectItem(p, "preset"));
    if (pre && (cJSON_GetObjectItem(p, "from") || cJSON_GetObjectItem(p, "to"))) {
        fail(cx, "invalid_args", "period takes either preset or from and to, not both", "Pass period:{preset:\"today\"} or period:{from:\"2026-11-01\",to:\"2026-11-02\"}.");
        return false;
    }
    if (pre) {
        uint32_t d = local_floor(now, 'd');
        if (!strcmp(pre, "today"))             { *from = d; *to = now; }
        else if (!strcmp(pre, "yesterday"))    { *from = local_add(d, 'd', -1); *to = d; }
        else if (!strcmp(pre, "last_7_days"))  { *from = local_add(d, 'd', -7); *to = d; }
        else if (!strcmp(pre, "last_30_days")) { *from = local_add(d, 'd', -30); *to = d; }
        else if (!strcmp(pre, "this_week"))    { *from = local_floor(now, 'w'); *to = now; }
        else if (!strcmp(pre, "last_week"))    { *to = local_floor(now, 'w'); *from = local_add(*to, 'w', -1); }
        else if (!strcmp(pre, "this_month"))   { *from = local_floor(now, 'm'); *to = now; }
        else if (!strcmp(pre, "last_month"))   { *to = local_floor(now, 'm'); *from = local_add(*to, 'm', -1); }
        else if (!strcmp(pre, "this_year"))    { *from = local_floor(now, 'y'); *to = now; }
        else if (!strcmp(pre, "last_year"))    { *to = local_floor(now, 'y'); *from = local_add(*to, 'y', -1); }
        else { fail(cx, "invalid_args", "period.preset: unknown value", "Use today, yesterday, last_7_days, last_30_days, this_week, last_week, this_month, last_month, this_year or last_year."); return false; }
        return true;
    }
    if (!parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(p, "from")), from) ||
        !parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(p, "to")), to)) {
        fail(cx, "invalid_args", "period.from / period.to: ISO 8601 date or date-time required", "Example: {\"from\":\"2026-11-01\",\"to\":\"2026-11-08\"} (to is exclusive).");
        return false;
    }
    if (*to > now) *to = now;
    if (*from >= *to) {
        fail(cx, "invalid_args", "period: from must be before to (and before now)",
             *to >= now ? "from is in the future: use a start before now, or period:{preset:\"today\"}."
                        : "Swap from and to: to is the exclusive end, e.g. {\"from\":\"2026-11-01\",\"to\":\"2026-11-08\"}.");
        return false;
    }
    if (now - *from > DAY_DEPTH_S + 86400u) {
        char h[112]; char t0[48]; iso_time(now - DAY_DEPTH_S, t0);
        snprintf(h, sizeof(h), "The earliest available is about %s.", t0);
        fail(cx, "out_of_history", "the range starts beyond the stored daily history (400 days)", h);
        return false;
    }
    return true;
}

static void add_period(cJSON *o, uint32_t from, uint32_t to)
{
    cJSON *p = cJSON_AddObjectToObject(o, "period");
    add_time(p, "from", from);
    add_time(p, "to", to);
}

/* ============================================================================================ tools */

/* Where a point's stored history starts: the start of its oldest day record, or of its oldest hour record while no day
 * has closed yet. 0 = nothing stored. Records only grow at the new end, so a short cache spares the flash walk; the
 * old end moves only when the ring wraps (400 days) or the point is deleted. */
#define HIST_CACHE_US (10LL * 60 * 1000000)
typedef struct { uint16_t u; uint32_t from; int64_t at; } hist_c_t;
static hist_c_t s_hist[RBLLM_MAX_POINTS];
static portMUX_TYPE s_hist_mux = portMUX_INITIALIZER_UNLOCKED;
static void hist_min(const rbpower_period_statistics_t *s, void *v)
{
    uint32_t *m = v, ts = (uint32_t)(s->period_start_us / 1000000LL);
    if (ts < *m) *m = ts;
}
static uint32_t history_from(uint16_t u, int64_t deadline_us)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_hist_mux);
    for (int i = 0; i < RBLLM_MAX_POINTS; i++)
        if (s_hist[i].u == u && s_hist[i].at && now - s_hist[i].at < HIST_CACHE_US) { uint32_t f = s_hist[i].from; portEXIT_CRITICAL(&s_hist_mux); return f; }
    portEXIT_CRITICAL(&s_hist_mux);
    uint32_t m = UINT32_MAX, from = 0;   /* record ts = the period's end */
    rbgrid_foreach_valid_record_ex(u, RBPOWER_STAT_PERIOD_DAILY, RBPOWER_PERIOD_DAY, 0, UINT32_MAX, false, deadline_us, hist_min, &m);
    if (m != UINT32_MAX) from = local_floor(m - 1, 'd');
    else {
        rbgrid_foreach_valid_record_ex(u, RBPOWER_STAT_PERIOD_1HOUR, RBPOWER_PERIOD_HOUR, 0, UINT32_MAX, false, deadline_us, hist_min, &m);
        if (m != UINT32_MAX) from = local_floor(m - 1, 'h');
    }
    if (esp_timer_get_time() > deadline_us) return from;   /* walk cut short: answer, but don't cache */
    portENTER_CRITICAL(&s_hist_mux);
    int k = 0;
    for (int i = 0; i < RBLLM_MAX_POINTS; i++) { if (s_hist[i].u == u) { k = i; break; } if (s_hist[i].at < s_hist[k].at) k = i; }
    s_hist[k] = (hist_c_t){ u, from, now };
    portEXIT_CRITICAL(&s_hist_mux);
    return from;
}
static void add_history_from(cJSON *o, const char *k, uint32_t from)
{
    if (from) add_time(o, k, from); else cJSON_AddNullToObject(o, k);
}

/* The hub's name as the user set it (hub page / propose_config hub.name), else the device name: one name everywhere
 * (get_hub_info, the manifest, the AI / LLM page's connect block). */
static void hub_display_name(char *b, size_t n)
{
    const rbpower_extended_config_t *ec = rbgrid_core_ext_cfg();
    rbgrid_json_context_t *jc = rbgrid_core_json_ctx();
    utf8_clean(ec && ec->home_name && ec->home_name[0] ? ec->home_name : (jc && jc->device_info.device_name[0] ? jc->device_info.device_name : "RBgrid"), b, n);
}
static cJSON *t_get_hub_info(const cJSON *a, rbllm_cx_t *cx)
{
    (void)a;
    cJSON *r = cJSON_CreateObject();
    const esp_app_desc_t *ad = esp_app_get_description();
    char id[13]; rbllm_hub_id(id);
    cJSON_AddStringToObject(r, "hub_id", id);
    { char b[48]; hub_display_name(b, sizeof(b)); cJSON_AddStringToObject(r, "name", b); }
    cJSON_AddStringToObject(r, "fw", ad ? ad->version : "");
    cJSON_AddStringToObject(r, "proto", RBLLM_PROTO);
#if CONFIG_IDF_TARGET_ESP32S3
    cJSON_AddStringToObject(r, "board", "esp32s3");
#else
    cJSON_AddStringToObject(r, "board", "esp32");
#endif
    uint32_t n = now_s();
    bool tv = rbtime_is_time_valid();
    cJSON_AddBoolToObject(r, "clock_synced", tv);
    if (tv) add_time(r, "hub_time", n);
    rbtime_config_t tc; memset(&tc, 0, sizeof(tc));
    rbtime_get_config(&tc);
    if (tc.timezone[0]) cJSON_AddStringToObject(r, "timezone", tc.timezone);
    cJSON_AddStringToObject(r, "currency", rbpower_tariff_currency());
    cJSON_AddNumberToObject(r, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    { char eh[17] = ""; esp_app_get_elf_sha256(eh, sizeof(eh)); cJSON_AddStringToObject(r, "build", eh); }   /* differs per image even when fw does not */
    cJSON *cap = cJSON_AddArrayToObject(r, "capabilities");   /* the same list as the manifest */
    cJSON_AddItemToArray(cap, cJSON_CreateString("read"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("events"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("playbooks"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("config_read"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("config"));
    cJSON_AddBoolToObject(r, "read_only_mode", S.read_only);
    {   /* how far back the hub can answer: depth limits, and where the oldest point's records start */
        cJSON *h = cJSON_AddObjectToObject(r, "history");
        cJSON_AddNumberToObject(h, "hourly_days", HOUR_DEPTH_S / 86400u);
        cJSON_AddNumberToObject(h, "daily_days", DAY_DEPTH_S / 86400u);
        rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
        uint32_t first = 0;
        if (p) {
            int np = snapshot(p, RBLLM_MAX_POINTS);
            for (int i = 0; i < np; i++) { uint32_t f = history_from(p[i].unit_id, cx->deadline_us); if (f && (!first || f < first)) first = f; }
            free(p);
        }
        add_history_from(h, "from", first);
    }
    {   /* the web app's language pack on the hub (installed from the hub page; null = the app's built-in English) */
        char lc[3];
        if (rbwebserver_ui_lang_code(lc)) cJSON_AddStringToObject(r, "ui_language", lc); else cJSON_AddNullToObject(r, "ui_language");
    }
    {   /* the web app kit installed on the hub (its page addresses follow this version); null = none installed */
        char kv[40];
        if (rbwebserver_ui_kit_version(kv, sizeof(kv))) cJSON_AddStringToObject(r, "ui_kit", kv); else cJSON_AddNullToObject(r, "ui_kit");
    }
    {   /* kept on the hub's own page on purpose: they change the assistant's own access or link, or need a file */
        cJSON *hp = cJSON_AddArrayToObject(r, "hub_page_only");
        const char *const HP[] = { "wifi", "admin_password", "assistant_access", "firmware_update", "factory_reset", "backup_restore", "language_pack", "mqtt_ca", "read_only" };
        for (size_t q = 0; q < sizeof(HP) / sizeof(HP[0]); q++) cJSON_AddItemToArray(hp, cJSON_CreateString(HP[q]));
    }
    cx->as_of = n;
    return r;
}

static cJSON *t_list_channels(const cJSON *a, rbllm_cx_t *cx)
{
    (void)a;
    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    if (!p) return fail(cx, "internal", "out of memory", NULL);
    int n = snapshot(p, RBLLM_MAX_POINTS);
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "channels");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        char h[5]; uid_hex(p[i].unit_id, h);
        cJSON_AddStringToObject(o, "id", h);
        add_name(o, p[i].name);
        cJSON_AddStringToObject(o, "point", p[i].point);
        cJSON_AddStringToObject(o, "role", role_class(p[i].role));
        cJSON_AddStringToObject(o, "role_detail", role_name(p[i].role));
        if (p[i].inside) { char ih[5]; uid_hex(p[i].inside, ih); cJSON_AddStringToObject(o, "inside", ih); }
        cJSON_AddBoolToObject(o, "present", p[i].present);
        cJSON_AddBoolToObject(o, "estimated_energy", p[i].estimated);
        cJSON_AddBoolToObject(o, "has_history", rbpower_flash_unit_has_history(p[i].unit_id));
        add_history_from(o, "history_from", history_from(p[i].unit_id, cx->deadline_us));
        cJSON_AddItemToArray(arr, o);
    }
    free(p);
    note(cx, "Energy direction is reported, not set: a reversed clamp is fixed at the clamp, not in software.");
    note(cx, "history_from is where a channel's stored records start (the first day may be partial); before it the hub answers null: no data, not zero.");
    cx->as_of = now_s();
    return r;
}

static cJSON *t_read_live(const cJSON *a, rbllm_cx_t *cx)
{
    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    if (!p) return fail(cx, "internal", "out of memory", NULL);
    int n = snapshot(p, RBLLM_MAX_POINTS);
    bool sel[RBLLM_MAX_POINTS];
    if (!select_points(cx, cJSON_GetObjectItem(a, "channels"), p, n, sel)) { free(p); return NULL; }
    rbgrid_system_totals_t st;
    rbpower_home_lock();
    rbgrid_core_system_totals(&st, false);
    rbpower_home_unlock();
    cJSON *r = cJSON_CreateObject();
    cJSON *sys = cJSON_AddObjectToObject(r, "grid");
    int stale_n = 0, fresh_n = 0;
    bool any_fresh = false;
    for (int i = 0; i < n; i++) if (p[i].present && p[i].age_ms != UINT32_MAX && p[i].age_ms <= 30000u) any_fresh = true;
    if (any_fresh) cJSON_AddNumberToObject(sys, "power_w", r2(st.p_w)); else cJSON_AddNullToObject(sys, "power_w");
    cJSON_AddStringToObject(sys, "sign", "positive = import from the grid");
    cJSON *arr = cJSON_AddArrayToObject(r, "channels");
    for (int i = 0; i < n; i++) {
        if (!sel[i]) continue;
        cJSON *o = cJSON_CreateObject();
        char h[5]; uid_hex(p[i].unit_id, h);
        cJSON_AddStringToObject(o, "id", h);
        add_name(o, p[i].name);
        cJSON_AddBoolToObject(o, "present", p[i].present);
        bool stale = !p[i].present || p[i].age_ms == UINT32_MAX || p[i].age_ms > 30000u;   /* no live data: null, never 0 */
        if (stale) { stale_n++; cJSON_AddNullToObject(o, "power_w"); cJSON_AddNullToObject(o, "current_a"); cJSON_AddNullToObject(o, "voltage_v"); cJSON_AddNullToObject(o, "pf");
                     if (p[i].age_ms != UINT32_MAX) cJSON_AddNumberToObject(o, "age_ms", p[i].age_ms); else cJSON_AddNullToObject(o, "age_ms");
                     cJSON_AddItemToArray(arr, o); continue; }
        fresh_n++;
        cJSON_AddNumberToObject(o, "power_w", r2(p[i].p_w));
        cJSON_AddNumberToObject(o, "current_a", round(p[i].i_a * 1000.0) / 1000.0);
        if (p[i].v_v > 1.0f) cJSON_AddNumberToObject(o, "voltage_v", r2(p[i].v_v));
        else cJSON_AddNullToObject(o, "voltage_v");
        if (p[i].pf > 0.0f && p[i].pf <= 1.0f && p[i].v_v > 1.0f && fabsf(p[i].p_w) > 1.0f) cJSON_AddNumberToObject(o, "pf", round(p[i].pf * 1000.0) / 1000.0);
        else cJSON_AddNullToObject(o, "pf");
        if (p[i].age_ms != UINT32_MAX) cJSON_AddNumberToObject(o, "age_ms", p[i].age_ms); else cJSON_AddNullToObject(o, "age_ms");
        cJSON_AddItemToArray(arr, o);
    }
    free(p);
    if (stale_n || !any_fresh) note(cx, "null = no live data (the channel's source is missing or silent for over 30 s) - not zero.");
    (void)fresh_n;
    cx->as_of = now_s();
    cx->resolution = "live";
    return r;
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_EXT: return "external";
    default: return "other";
    }
}

static cJSON *t_get_health(const cJSON *a, rbllm_cx_t *cx)
{
    (void)a;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddStringToObject(r, "last_reset", reset_name(esp_reset_reason()));
    cJSON_AddBoolToObject(r, "clock_synced", rbtime_is_time_valid());
    {   /* where the hub's time comes from, and when it was last set */
        static const char *const SRC[] = { "none", "internet", "hub", "manual", "rtc", "gps" };
        rbtime_statistics_t ts; memset(&ts, 0, sizeof(ts));
        rbtime_config_t tc; memset(&tc, 0, sizeof(tc)); rbtime_get_config(&tc);
        cJSON *ck = cJSON_AddObjectToObject(r, "clock");
        bool st_ok = rbtime_get_statistics(&ts) == RBPOWER_OK;
        int si = st_ok ? (int)ts.active_source : 0;
        cJSON_AddStringToObject(ck, "source", si >= 0 && si < 6 ? SRC[si] : "none");
        uint64_t up = (uint64_t)esp_timer_get_time();
        if (st_ok && ts.last_sync_timestamp_us && ts.last_sync_timestamp_us <= up && rbtime_is_time_valid()) add_time(ck, "last_set", now_s() - (uint32_t)((up - ts.last_sync_timestamp_us) / 1000000ULL));
        else cJSON_AddNullToObject(ck, "last_set");
        cJSON_AddStringToObject(ck, "time_server", tc.sntp_server);
        cJSON_AddStringToObject(ck, "timezone", tc.timezone);
    }
    cJSON *hp = cJSON_AddObjectToObject(r, "heap");
    cJSON_AddNumberToObject(hp, "internal_free_b", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(hp, "internal_min_b", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(hp, "psram_free_b", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    extern TaskHandle_t rbllm_worker_handle(void);
    TaskHandle_t wh = rbllm_worker_handle();
    if (wh) cJSON_AddNumberToObject(hp, "llm_stack_free_b", (double)uxTaskGetStackHighWaterMark(wh) * sizeof(StackType_t));
    int ist = 0;
    cJSON *i2cst = i2c_do("state", -1, -1, -1, &ist);
    if (i2cst) {
        cJSON *bs = cJSON_AddArrayToObject(r, "i2c_buses");
        const cJSON *b;
        cJSON_ArrayForEach(b, cJSON_GetObjectItem(i2cst, "buses")) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "bus", cJSON_GetNumberValue(cJSON_GetObjectItem(b, "bus")));
            cJSON_AddBoolToObject(o, "running", cJSON_IsTrue(cJSON_GetObjectItem(b, "up")));
            const cJSON *sda = cJSON_GetObjectItem(b, "sda"), *scl = cJSON_GetObjectItem(b, "scl");
            if (cJSON_IsNumber(sda) && sda->valueint >= 0) cJSON_AddNumberToObject(o, "sda", sda->valueint);
            if (cJSON_IsNumber(scl) && scl->valueint >= 0) cJSON_AddNumberToObject(o, "scl", scl->valueint);
            cJSON_AddItemToArray(bs, o);
        }
        const cJSON *mx = cJSON_GetObjectItem(i2cst, "max_modules");
        if (cJSON_IsNumber(mx)) cJSON_AddNumberToObject(r, "max_modules", mx->valueint);
        const cJSON *pk = cJSON_GetObjectItem(i2cst, "pins_ok");
        if (cJSON_IsArray(pk)) cJSON_AddItemToObject(r, "pins_ok", cJSON_Duplicate(pk, true));   /* free GPIOs set_bus_pins accepts */
        cJSON_AddBoolToObject(r, "restart_pending", cJSON_IsTrue(cJSON_GetObjectItem(i2cst, "reboot_pending")));
    }
    if (s_modfw_status) {   /* a module firmware update started on the hub page: progress, as the page shows it */
        char fb[320]; fb[0] = '\0';
        s_modfw_status(fb, sizeof(fb));
        cJSON *mf = fb[0] ? cJSON_Parse(fb) : NULL;
        if (mf) cJSON_AddItemToObject(r, "module_firmware", mf); else cJSON_AddNullToObject(r, "module_firmware");
    }
    cJSON *mods = cJSON_AddArrayToObject(r, "modules");
    int on = 0, tot = 0;
    for (uint8_t m = 0; m < RBPOWER_MODULE_DESC_MAX; m++) {
        rbpower_module_desc_t d;
        if (!rbpower_get_module_desc(m, &d)) continue;
        tot++; if (d.present) on++;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "module", m);
        char ad[8]; snprintf(ad, sizeof(ad), "0x%02X", d.addr);
        cJSON_AddStringToObject(o, "address", ad);
        {   /* the bus the module sits on (from the I2C state) */
            const cJSON *im;
            cJSON_ArrayForEach(im, cJSON_GetObjectItem(i2cst, "modules"))
                if ((int)cJSON_GetNumberValue(cJSON_GetObjectItem(im, "module")) == m) {
                    cJSON_AddNumberToObject(o, "bus", cJSON_GetNumberValue(cJSON_GetObjectItem(im, "bus"))); break;
                }
        }
        cJSON_AddStringToObject(o, "model", d.model);
        cJSON_AddStringToObject(o, "link", "i2c");
        cJSON_AddNumberToObject(o, "voltage_inputs", d.n_v);
        cJSON_AddNumberToObject(o, "current_inputs", d.n_i);
        char fb[12];
        const char *fn = d.line == 0 ? rbamp_basic_fw_name((uint8_t)d.fw) : NULL;
        if (!fn) { snprintf(fb, sizeof(fb), d.line == 0 ? "0x%02lX" : "0x%08lX", (unsigned long)d.fw); fn = fb; }
        cJSON_AddStringToObject(o, "fw", fn);
        cJSON_AddStringToObject(o, "status", d.present ? "online" : "missing");
        cJSON_AddItemToArray(mods, o);
    }
    cJSON_Delete(i2cst);
    cJSON_AddNumberToObject(r, "modules_online", on);
    cJSON_AddNumberToObject(r, "modules_known", tot);
    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    if (p) {
        int n = snapshot(p, RBLLM_MAX_POINTS), pr = 0;
        for (int i = 0; i < n; i++) pr += p[i].present;
        cJSON_AddNumberToObject(r, "channels_present", pr);
        cJSON_AddNumberToObject(r, "channels_configured", n);
        free(p);
    }
    if (tot > on) note(cx, "A module is missing: check its cable and power; its channels read no energy meanwhile.");
    cx->as_of = now_s();
    return r;
}

/* Copy @p in to @p out keeping only well-formed UTF-8 (a log line cut at its byte limit can end inside a character;
 * a client name is the user's text). Invalid bytes become '?'. */
static void utf8_clean(const char *in, char *out, size_t n)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)(in ? in : "");
    while (*p && o + 1 < n) {
        int len = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = len > 0 && !(len == 2 && *p < 0xC2);
        for (int k = 1; ok && k < len; k++) ok = (p[k] & 0xC0) == 0x80;
        if (!ok) { out[o++] = '?'; p++; continue; }
        if (o + len >= n) break;
        for (int k = 0; k < len; k++) out[o++] = (char)p[k];
        p += len;
    }
    out[o] = '\0';
}

/* ============================================================================================ typed event ring (§8.5)
 * One ring of EV_N records in /llm_events.bin; seq grows across reboots and is the get_events cursor. Written whole on
 * each event (events are rare: a restart, a config version, a module lost/back, the clock, prices). A record made before
 * the clock synced keeps its uptime and gets its time when the clock comes. */
#define EV_FILE   "/llm_events.bin"
#define EV_N      64
#define EV_MAGIC  0x31564552u   /* "REV1" */
enum { EV_REBOOT = 1, EV_CONFIG, EV_MODULE_LOST, EV_MODULE_BACK, EV_CLOCK, EV_PRICE };
enum { SEV_INFO = 0, SEV_WARNING, SEV_CRITICAL };
typedef struct __attribute__((packed)) {
    uint32_t seq, ts, up_s;
    uint8_t  type, sev;
    uint16_t chan;
    char     info[40];
} ev_rec_t;
typedef struct { uint32_t magic, next_seq; ev_rec_t r[EV_N]; } ev_file_t;
static EXT_RAM_BSS_ATTR ev_file_t s_ev;
static StaticSemaphore_t s_ev_mx_buf;
static SemaphoreHandle_t s_ev_mx;
static bool     s_ev_loaded;
static uint32_t s_ev_boot_seq;      /* first seq of this boot */
static bool     s_ev_ts_pending;    /* this boot has records with ts 0 */

static const char *ev_type_name(uint8_t t)
{
    switch (t) {
    case EV_REBOOT: return "reboot"; case EV_CONFIG: return "config_changed"; case EV_MODULE_LOST: return "module_lost";
    case EV_MODULE_BACK: return "module_back"; case EV_CLOCK: return "clock_unsynced"; case EV_PRICE: return "price_missing";
    default: return "other";
    }
}

static void ev_save(void)
{
    if (littlefs_raw_write(EV_FILE, &s_ev, sizeof(s_ev)) != ESP_OK) rbpower_diag_printf("FAIL llm: %s not saved", EV_FILE);
}

static void ev_add(uint8_t type, uint8_t sev, uint16_t chan, const char *info)
{
    if (!s_ev_loaded || !s_ev_mx) return;
    xSemaphoreTake(s_ev_mx, portMAX_DELAY);
    uint32_t seq = s_ev.next_seq++;
    ev_rec_t *r = &s_ev.r[seq % EV_N];
    memset(r, 0, sizeof(*r));
    r->seq = seq;
    r->up_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    r->ts = rbtime_is_time_valid() ? now_s() : 0;
    if (!r->ts) s_ev_ts_pending = true;
    r->type = type; r->sev = sev; r->chan = chan;
    utf8_clean(info, r->info, sizeof(r->info));
    ev_save();
    xSemaphoreGive(s_ev_mx);
}

static void ev_boot(void)
{
    if (!s_ev_mx) s_ev_mx = xSemaphoreCreateMutexStatic(&s_ev_mx_buf);
    if (s_ev_loaded || !littlefs_raw_is_mounted()) return;
    size_t sz = 0, got = 0;
    bool ok = littlefs_raw_size(EV_FILE, &sz) == ESP_OK && sz == sizeof(s_ev) &&
              littlefs_raw_read(EV_FILE, &s_ev, sizeof(s_ev), &got) == ESP_OK && got == sizeof(s_ev) && s_ev.magic == EV_MAGIC;
    if (!ok) { memset(&s_ev, 0, sizeof(s_ev)); s_ev.magic = EV_MAGIC; s_ev.next_seq = 1; }
    s_ev_loaded = true;
    s_ev_boot_seq = s_ev.next_seq;
    esp_reset_reason_t rr = esp_reset_reason();
    bool bad = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT;
    ev_add(EV_REBOOT, bad ? SEV_WARNING : SEV_INFO, 0, reset_name(rr));
}

/* ---- the watcher: called on the worker task about every 10 s */
#define W_LOST_S        30     /* a point gone this long is lost */
#define W_NEVER_S      120     /* a point never seen this boot is reported after this uptime */
static EXT_RAM_BSS_ATTR rbgrid_point_snap_t s_w_snap[RBLLM_MAX_POINTS];
static EXT_RAM_BSS_ATTR struct { uint16_t uid; bool seen, lost; int64_t gone_us; } s_w[RBLLM_MAX_POINTS];
static int  s_w_n;
static bool s_w_clock_ev;
static char s_w_price[16];
static int64_t s_w_last_us;

static void ev_watch(void)
{
    int64_t t = esp_timer_get_time();
    if (!s_ev_loaded || t - s_w_last_us < 9000000LL) return;
#if CONFIG_RBGRID_IF_HTTP
    extern bool rbwebserver_ota_in_progress(void);
    if (rbwebserver_ota_in_progress()) return;   /* not during an OTA write (cache stops; see rbtelemetry.c) */
#endif
    s_w_last_us = t;
    uint32_t up = (uint32_t)(t / 1000000LL);
    bool clk = rbtime_is_time_valid();
    /* clock */
    if (!clk && up >= 180 && !s_w_clock_ev) { s_w_clock_ev = true; ev_add(EV_CLOCK, SEV_WARNING, 0, "no time sync 3 min after start"); }
    if (clk && s_ev_ts_pending) {
        xSemaphoreTake(s_ev_mx, portMAX_DELAY);
        uint32_t n = now_s();
        for (int i = 0; i < EV_N; i++) {
            ev_rec_t *r = &s_ev.r[i];
            if (r->seq >= s_ev_boot_seq && !r->ts) r->ts = n - (up - r->up_s);
        }
        s_ev_ts_pending = false;
        ev_save();
        xSemaphoreGive(s_ev_mx);
    }
    /* modules: presence per point */
    if (rbpower_home_lock_timeout(100)) {
        int n = rbgrid_core_points_snapshot(s_w_snap, RBLLM_MAX_POINTS);
        rbpower_home_unlock();
        for (int i = 0; i < n; i++) {
            int k = 0;
            while (k < s_w_n && s_w[k].uid != s_w_snap[i].unit_id) k++;
            if (k == s_w_n) { if (s_w_n >= RBLLM_MAX_POINTS) continue; s_w_n++; memset(&s_w[k], 0, sizeof(s_w[k])); s_w[k].uid = s_w_snap[i].unit_id; }
            if (s_w_snap[i].present) {
                s_w[k].seen = true; s_w[k].gone_us = 0;
                if (s_w[k].lost) { s_w[k].lost = false; ev_add(EV_MODULE_BACK, SEV_INFO, s_w[k].uid, s_w_snap[i].name); }
            } else if (!s_w[k].lost) {
                if (!s_w[k].gone_us) s_w[k].gone_us = t;
                bool due = s_w[k].seen ? (t - s_w[k].gone_us) >= W_LOST_S * 1000000LL : up >= W_NEVER_S;
                if (due) { s_w[k].lost = true; ev_add(EV_MODULE_LOST, SEV_WARNING, s_w[k].uid, s_w_snap[i].name); }
            }
        }
    }
    /* dynamic prices: entering a state without tomorrow's (or today's) prices */
    if (clk && up >= W_NEVER_S) {
        const char *st = rbdynprice_state();
        bool bad = !strcmp(st, "today_only") || !strcmp(st, "stale") || !strcmp(st, "plan_fallback");
        bool was_bad = !strcmp(s_w_price, "today_only") || !strcmp(s_w_price, "stale") || !strcmp(s_w_price, "plan_fallback");
        if (bad && !was_bad) ev_add(EV_PRICE, SEV_WARNING, 0, st);
        strlcpy(s_w_price, st, sizeof(s_w_price));
    }
}

static cJSON *events_typed(uint32_t since, uint32_t lim, rbllm_cx_t *cx)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "events");
    if (!s_ev_loaded) { cJSON_AddNumberToObject(r, "next", since); cJSON_AddBoolToObject(r, "more", false); return r; }
    xSemaphoreTake(s_ev_mx, portMAX_DELAY);
    uint32_t top = s_ev.next_seq;                       /* exclusive */
    uint32_t oldest = top > EV_N ? top - EV_N : 1;
    while (oldest < top && s_ev.r[oldest % EV_N].seq != oldest) oldest++;
    uint32_t s = since + 1 > oldest ? since + 1 : oldest;
    uint32_t last = since, cnt = 0, last_ts = 0;
    for (uint32_t q = s; q < top && cnt < lim; q++) {
        const ev_rec_t *e = &s_ev.r[q % EV_N];
        if (e->seq != q) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", q);
        if (e->ts > RBGRID_TS_SANE_MIN) { add_time(o, "time", e->ts); last_ts = e->ts; } else cJSON_AddNullToObject(o, "time");
        cJSON_AddStringToObject(o, "type", ev_type_name(e->type));
        cJSON_AddStringToObject(o, "severity", e->sev == SEV_CRITICAL ? "critical" : e->sev == SEV_WARNING ? "warning" : "info");
        if (e->chan) { char h[5]; uid_hex(e->chan, h); cJSON_AddStringToObject(o, "channel", h); }
        cJSON *d = cJSON_AddObjectToObject(o, "data");
        char msg[96];
        switch (e->type) {
        case EV_REBOOT: cJSON_AddStringToObject(d, "reason", e->info); snprintf(msg, sizeof(msg), "Hub started (reset: %s)", e->info); break;
        case EV_CONFIG: {
            char b[40]; strlcpy(b, e->info, sizeof(b));
            char *bar = strchr(b, '|');
            if (bar) { *bar = 0; if (atoi(b) > 0) cJSON_AddNumberToObject(d, "version", atoi(b)); else cJSON_AddNullToObject(d, "version"); cJSON_AddStringToObject(d, "source", bar + 1); }
            if (atoi(b) > 0) snprintf(msg, sizeof(msg), "Configuration version %s saved (%s)", b, bar ? bar + 1 : "?");
            else snprintf(msg, sizeof(msg), "Configuration changed, not versioned (%s)", bar ? bar + 1 : "?");
            break; }
        case EV_MODULE_LOST: cJSON_AddStringToObject(d, "name", e->info); snprintf(msg, sizeof(msg), "No data from channel \"%s\"", e->info); break;
        case EV_MODULE_BACK: cJSON_AddStringToObject(d, "name", e->info); snprintf(msg, sizeof(msg), "Channel \"%s\" reports again", e->info); break;
        case EV_CLOCK: snprintf(msg, sizeof(msg), "Clock not set: %s", e->info); break;
        case EV_PRICE: cJSON_AddStringToObject(d, "state", e->info); snprintf(msg, sizeof(msg), "Dynamic prices missing (state %s); the plan price is used", e->info); break;
        default: snprintf(msg, sizeof(msg), "%s", e->info); break;
        }
        char mc[112]; utf8_clean(msg, mc, sizeof(mc));
        cJSON_AddStringToObject(o, "message", mc);
        cJSON_AddItemToArray(arr, o);
        last = q; cnt++;
    }
    bool more = last + 1 < top && cnt >= lim;
    xSemaphoreGive(s_ev_mx);
    cJSON_AddNumberToObject(r, "next", last);
    cJSON_AddBoolToObject(r, "more", more);
    if (since && since + 1 < oldest) cJSON_AddNumberToObject(r, "lost", oldest - since - 1);
    cx->as_of = last_ts ? last_ts : now_s();
    return r;
}

static cJSON *t_get_events(const cJSON *a, rbllm_cx_t *cx)
{
    const cJSON *js = cJSON_GetObjectItem(a, "since"), *jl = cJSON_GetObjectItem(a, "limit");
    uint32_t since = cJSON_IsNumber(js) && js->valuedouble > 0 ? (js->valuedouble > 4e9 ? 4000000000u : (uint32_t)js->valuedouble) : 0;
    uint32_t lim = cJSON_IsNumber(jl) ? (jl->valuedouble > 50 ? 50 : jl->valuedouble < 1 ? 1 : (uint32_t)jl->valuedouble) : 30;
    const char *src_s = cJSON_GetStringValue(cJSON_GetObjectItem(a, "source"));
    if (src_s && strcmp(src_s, "events") && strcmp(src_s, "log"))
        return fail(cx, "invalid_args", "source: \"events\" or \"log\"", NULL);
    if (!src_s || !strcmp(src_s, "events")) return events_typed(since, lim, cx);
    if (!s_events_reader) return fail(cx, "internal", "event log not available in this build", NULL);
    char *raw = s_events_reader(since, lim, 1);   /* events + FAIL lines, not the periodic trace */
    cJSON *src = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!src) return fail(cx, "internal", "event log read failed", NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "events");
    const cJSON *e;
    uint32_t last_ts = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(src, "lines")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", cJSON_GetNumberValue(cJSON_GetObjectItem(e, "seq")));
        uint32_t ts = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "ts"));
        if (ts > RBGRID_TS_SANE_MIN) { add_time(o, "time", ts); last_ts = ts; }
        else cJSON_AddNullToObject(o, "time");
        const char *lv = cJSON_GetStringValue(cJSON_GetObjectItem(e, "lvl"));
        const char *mm = cJSON_GetStringValue(cJSON_GetObjectItem(e, "msg"));
        /* a line with a security consequence is a warning even though the log files it as an event */
        bool sec = mm && (strstr(mm, "AP password = setup password") || strstr(mm, "should change it"));
        cJSON_AddStringToObject(o, "severity", (lv && !strcmp(lv, "fail")) || sec ? "warning" : "info");
        cJSON_AddStringToObject(o, "type", "log");
        char mc[224];
        utf8_clean(cJSON_GetStringValue(cJSON_GetObjectItem(e, "msg")), mc, sizeof(mc));
        cJSON_AddStringToObject(o, "message", mc);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(r, "next", cJSON_GetNumberValue(cJSON_GetObjectItem(src, "next")));
    cJSON_AddBoolToObject(r, "more", cJSON_IsTrue(cJSON_GetObjectItem(src, "more")));
    double first = cJSON_GetNumberValue(cJSON_GetObjectItem(src, "first"));
    if (since && first > 0 && since + 1 < (uint32_t)first) cJSON_AddNumberToObject(r, "lost", first - since - 1);
    cJSON_Delete(src);
    note(cx, "Messages are the hub's own English log lines (this boot); quote them, don't translate the codes in them.");
    cx->as_of = last_ts ? last_ts : now_s();
    return r;
}

/* Resolution unit + its length for the row budget */
static char res_unit(const char *s)
{
    if (!s) return 0;
    if (!strcmp(s, "hour")) return 'h';
    if (!strcmp(s, "day")) return 'd';
    if (!strcmp(s, "week")) return 'w';
    if (!strcmp(s, "month")) return 'm';
    if (!strcmp(s, "year")) return 'y';
    if (!strcmp(s, "total")) return 't';
    return 0;
}
static const char *res_str(char u)
{
    switch (u) { case 'h': return "hour"; case 'd': return "day"; case 'w': return "week"; case 'm': return "month";
                 case 'y': return "year"; default: return "total"; }
}
static char res_coarser(char u)
{
    switch (u) { case 'h': return 'd'; case 'd': return 'w'; case 'w': return 'm'; case 'm': return 'y'; default: return 't'; }
}
static int count_buckets(uint32_t from, uint32_t to, char u)
{
    if (u == 't') return 1;
    int n = 0;
    for (uint32_t b = local_floor(from, u); b < to && n < 2000; b = local_add(b, u, 1)) n++;
    return n;
}
static int layout(bucket_t **out, uint32_t from, uint32_t to, char u)
{
    int n = count_buckets(from, to, u);
    bucket_t *b = calloc(n, sizeof(bucket_t));
    if (!b) return -1;
    if (u == 't') b[0].from = from;
    else { uint32_t t = local_floor(from, u); for (int i = 0; i < n; i++) { b[i].from = t; t = local_add(t, u, 1); } }
    *out = b;
    return n;
}

#define ROW_BYTES  96    /* one energy row as JSON, with ISO time */
#define CH_BYTES   220   /* per-channel overhead */

static cJSON *t_get_energy_summary(const cJSON *a, rbllm_cx_t *cx)
{
    uint32_t from, to;
    if (!parse_period(cx, cJSON_GetObjectItem(a, "period"), &from, &to)) return NULL;
    const char *rs = cJSON_GetStringValue(cJSON_GetObjectItem(a, "resolution"));
    char u = res_unit(rs);
    if (rs && !u) return fail(cx, "invalid_args", "resolution: hour, day, week, month, year or total", NULL);
    uint32_t span = to - from;
    if (!u) u = span <= 2u * 86400u ? 'h' : span <= 62u * 86400u ? 'd' : span <= 3u * 366u * 86400u ? 'm' : 'y';
    if (u == 'h' && now_s() - from > HOUR_DEPTH_S) {   /* §5: coarsen to the finest stored resolution, don't refuse */
        u = 'd'; cx->truncated = true;
        note(cx, "Hourly history is kept for 60 days; this older range is answered per day.");
    }

    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    if (!p) return fail(cx, "internal", "out of memory", NULL);
    int n = snapshot(p, RBLLM_MAX_POINTS);
    bool sel[RBLLM_MAX_POINTS];
    if (!select_points(cx, cJSON_GetObjectItem(a, "channels"), p, n, sel)) { free(p); return NULL; }
    int nsel = 0; for (int i = 0; i < n; i++) nsel += sel[i];

    /* row budget: coarsen before building */
    const char *asked = res_str(u);
    while (u != 't' && nsel * (CH_BYTES + count_buckets(from, to, u) * ROW_BYTES) > RBLLM_MAX_RESPONSE_BYTES - 1024) u = res_coarser(u);
    if (strcmp(asked, res_str(u)) != 0) {
        cx->truncated = true;
        note(cx, "The resolution was lowered to fit the answer size; narrow the period or pick fewer channels for finer rows.");
    }

    cJSON *r = cJSON_CreateObject();
    add_period(r, from, to);
    cJSON_AddStringToObject(r, "resolution", res_str(u));
    cJSON *arr = cJSON_AddArrayToObject(r, "channels");
    uint32_t newest = 0;
    for (int i = 0; i < n; i++) {
        if (!sel[i]) continue;
        bucket_t *b = NULL;
        int nb = layout(&b, from, to, u);
        if (nb < 0) { cJSON_Delete(r); free(p); return fail(cx, "internal", "out of memory", NULL); }
        sum_ctx_t c = { .b = b, .nb = nb, .unit = u, .cx = cx };
        sum_range(&c, p[i].unit_id, from, to);
        if (c.timed_out) { free(b); cJSON_Delete(r); free(p);
            return fail(cx, "timeout", "the history read took too long", "Narrow the period, use a coarser resolution or fewer channels."); }
        cJSON *o = cJSON_CreateObject();
        char h[5]; uid_hex(p[i].unit_id, h);
        cJSON_AddStringToObject(o, "id", h);
        add_name(o, p[i].name);
        cJSON_AddStringToObject(o, "role", role_class(p[i].role));
        double ti = 0, te = 0;
        cJSON *rows = (u == 't') ? NULL : cJSON_AddArrayToObject(o, "rows");
        int empty = 0;
        for (int k = 0; k < nb; k++) {
            ti += b[k].e_in; te += b[k].e_out;
            if (!b[k].recs) empty++;
            if (!rows) continue;
            cJSON *row = cJSON_CreateObject();
            add_time(row, "from", b[k].from);
            if (b[k].recs) {
                cJSON_AddNumberToObject(row, "energy_kwh", r4(b[k].e_in / 1000.0));
                cJSON_AddNumberToObject(row, "export_kwh", r4(b[k].e_out / 1000.0));
            } else { cJSON_AddNullToObject(row, "energy_kwh"); cJSON_AddNullToObject(row, "export_kwh"); }
            cJSON_AddItemToArray(rows, row);
        }
        cJSON *tot = cJSON_AddObjectToObject(o, "total");
        if (empty < nb) {
            cJSON_AddNumberToObject(tot, "energy_kwh", r4(ti / 1000.0));
            cJSON_AddNumberToObject(tot, "export_kwh", r4(te / 1000.0));
        } else { cJSON_AddNullToObject(tot, "energy_kwh"); cJSON_AddNullToObject(tot, "export_kwh"); }
        if (empty) { cx->gaps = true; note(cx, "Rows with null have no stored record (hub off, point added later, or beyond the history depth)."); }
        if (!c.hour_secs && !c.day_secs) cx->gaps = true;
        else if (!empty && sum_partial(&c)) { cx->gaps = true; note(cx, PARTIAL_NOTE); }
        if (c.newest_end > newest) newest = c.newest_end;
        cJSON_AddItemToArray(arr, o);
        free(b);
    }
    free(p);
    cx->as_of = newest;
    if (newest && to > newest + 60 && !cx->open_hour) note(cx, "Data runs up to as_of; the newest 15-min period is booked when it closes.");
    cx->resolution = res_str(u);
    return r;
}

/* Totals per selected point over [from,to): fills the out arrays. Returns false on timeout. */
typedef struct { double e_in, e_out, cost, rev; uint32_t newest; bool any; double open_cost, open_rev; bool partial; } tot_t;
static bool totals(rbllm_cx_t *cx, const rbgrid_point_snap_t *p, int n, const bool sel[], uint32_t from, uint32_t to, tot_t *t)
{
    for (int i = 0; i < n; i++) {
        memset(&t[i], 0, sizeof(t[i]));
        if (!sel[i]) continue;
        bucket_t b = { .from = from };
        sum_ctx_t c = { .b = &b, .nb = 1, .unit = 't', .cx = cx };
        sum_range(&c, p[i].unit_id, from, to);
        if (c.timed_out) return false;
        t[i] = (tot_t){ b.e_in, b.e_out, b.cost, b.rev, c.newest_end, b.recs > 0, c.open_cost, c.open_rev, b.recs > 0 && sum_partial(&c) };
        if (t[i].partial) { cx->gaps = true; note(cx, PARTIAL_NOTE); }
    }
    return true;
}

static cJSON *t_get_cost_breakdown(const cJSON *a, rbllm_cx_t *cx)
{
    uint32_t from, to;
    if (!parse_period(cx, cJSON_GetObjectItem(a, "period"), &from, &to)) return NULL;
    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    tot_t *t = calloc(RBLLM_MAX_POINTS, sizeof(tot_t));
    if (!p || !t) { free(p); free(t); return fail(cx, "internal", "out of memory", NULL); }
    int n = snapshot(p, RBLLM_MAX_POINTS);
    bool sel[RBLLM_MAX_POINTS];
    if (!select_points(cx, cJSON_GetObjectItem(a, "channels"), p, n, sel)) { free(p); free(t); return NULL; }
    for (int i = 0; i < n; i++) if (p[i].role == RBPOWER_ROLE_MAIN_SUPPLY) sel[i] = true;   /* the bill needs the main feed */
    if (!totals(cx, p, n, sel, from, to, t)) { free(p); free(t);
        return fail(cx, "timeout", "the history read took too long", "Narrow the period."); }

    cJSON *r = cJSON_CreateObject();
    add_period(r, from, to);
    cJSON_AddStringToObject(r, "currency", rbpower_tariff_currency());
    double cost = 0, rev = 0, imp = 0, exp = 0; int mains = 0; uint32_t newest = 0; bool any = false;
    for (int i = 0; i < n; i++) {
        if (p[i].role != RBPOWER_ROLE_MAIN_SUPPLY) continue;
        mains++; cost += t[i].cost; rev += t[i].rev; imp += t[i].e_in; exp += t[i].e_out; any |= t[i].any;
        if (t[i].newest > newest) newest = t[i].newest;
    }
    /* The open hour of the main supply: the hub's own day figures are exact (booked per closed 15-min period), so use
     * cost_day − today's closed hour records instead of the estimate at the price now — the bill figure then equals the
     * hub page's "cost today". Hour records hold whole cents, so a period that takes in all of today (from ≤ local
     * midnight, up to now) uses cost_day for the whole day: the cent rounding of the closed hours drops out too. */
    bool exact_open = false;
    double oc = 0, orv = 0;
    for (int i = 0; i < n; i++) if (p[i].role == RBPOWER_ROLE_MAIN_SUPPLY) { oc += t[i].open_cost; orv += t[i].open_rev; }
    uint32_t today0 = local_floor(now_s(), 'd');
    /* whole_today: the period reaches now (the open hour may hold nothing yet, e.g. just after a restart) */
    bool whole_today = from <= today0 && to + 5 >= now_s() && rbtime_is_time_valid();
    if (mains && (whole_today || (cx->open_hour && (oc > 0 || orv > 0)))) {
        rbgrid_system_totals_t st;
        rbpower_home_lock(); rbgrid_core_system_totals(&st, true); rbpower_home_unlock();
        double closed_c = 0, closed_r = 0;
        bool okc = true;
        for (int i = 0; i < n && okc; i++) {
            if (p[i].role != RBPOWER_ROLE_MAIN_SUPPLY) continue;
            bucket_t b = { .from = today0 };
            sum_ctx_t c2 = { .b = &b, .nb = 1, .unit = 't', .cx = cx, .no_open = true };
            sum_range(&c2, p[i].unit_id, today0, now_s());
            if (c2.timed_out) okc = false;
            closed_c += b.cost; closed_r += b.rev;
        }
        double ex_c = st.cost_day - closed_c, ex_r = st.rev_day - closed_r;
        if (okc && whole_today) {
            cost += ex_c - oc;      /* today's part becomes cost_day exactly; ex_c may be a hair below 0 (cent rounding) */
            rev += ex_r - orv;
            exact_open = true;
            cx->open_cost_exact = true;
        } else if (okc && ex_c > -0.00005 && ex_r > -0.00005) {
            cost += (ex_c < 0 ? 0 : ex_c) - oc;
            rev += (ex_r < 0 ? 0 : ex_r) - orv;
            exact_open = true;
            cx->open_cost_exact = true;
        }
    }
    if (mains && !any) {
        const char *k[] = { "total_cost", "import_kwh", "export_revenue", "export_kwh", "net_cost" };
        for (size_t q = 0; q < 5; q++) cJSON_AddNullToObject(r, k[q]);
        cx->gaps = true; note(cx, "No stored record of the main supply in this period: no data, not zero.");
    } else if (mains) {
        cJSON_AddNumberToObject(r, "total_cost", r4(cost));
        cJSON_AddNumberToObject(r, "import_kwh", r4(imp / 1000.0));
        cJSON_AddNumberToObject(r, "export_revenue", r4(rev));
        cJSON_AddNumberToObject(r, "export_kwh", r4(exp / 1000.0));
        cJSON_AddNumberToObject(r, "net_cost", r4(cost - rev));
    } else {
        cJSON_AddNullToObject(r, "total_cost");
        note(cx, "No point is set as main supply, so there is no bill figure; costs per channel are attribution only.");
    }
    cJSON *bc = cJSON_AddArrayToObject(r, "by_channel");
    for (int i = 0; i < n; i++) {
        if (!sel[i] || p[i].role == RBPOWER_ROLE_MAIN_SUPPLY) continue;
        cJSON *o = cJSON_CreateObject();
        char h[5]; uid_hex(p[i].unit_id, h);
        cJSON_AddStringToObject(o, "id", h);
        add_name(o, p[i].name);
        cJSON_AddStringToObject(o, "role", role_class(p[i].role));
        if (t[i].any) { cJSON_AddNumberToObject(o, "cost", r4(t[i].cost)); cJSON_AddNumberToObject(o, "energy_kwh", r4(t[i].e_in / 1000.0)); }
        else { cJSON_AddNullToObject(o, "cost"); cJSON_AddNullToObject(o, "energy_kwh"); }
        if (t[i].e_out > 0) { cJSON_AddNumberToObject(o, "export_kwh", r4(t[i].e_out / 1000.0)); cJSON_AddNumberToObject(o, "export_value", r4(t[i].rev)); }
        cJSON_AddItemToArray(bc, o);
        if (t[i].newest > newest) newest = t[i].newest;
    }
    rbgrid_tariff_now_t tn;
    rbgrid_core_tariff_now(&tn);
    cJSON *tj = cJSON_AddObjectToObject(r, "tariff_now");
    cJSON_AddStringToObject(tj, "price_source", tn.external ? "dynamic" : tn.placeholder ? "demo" : "plan");
    cJSON_AddBoolToObject(r, "tariff_saved", !tn.placeholder);
    if (tn.zone[0]) cJSON_AddStringToObject(tj, "zone", tn.zone);
    cJSON_AddNumberToObject(tj, "buy_price_per_kwh", r4(tn.buy_rate_all_in));
    if (tn.placeholder) { cx->estimated = true; note(cx, "No tariff plan is saved: money is computed off the built-in demo plan."); }
    if (exact_open) cJSON_AddBoolToObject(r, "open_hour_cost_exact", true);
    note(cx, "Costs are gross (VAT included) as the hub books them; fixed charges are not included.");
    note(cx, "by_channel is attribution behind the main feed and does not add up to the bill.");
    free(p); free(t);
    cx->as_of = newest;
    return r;
}

static cJSON *t_compare_periods(const cJSON *a, rbllm_cx_t *cx)
{
    uint32_t af, at, bf, bt;
    const char *bl = cJSON_GetStringValue(cJSON_GetObjectItem(a, "baseline"));
    if (!cJSON_GetObjectItem(a, "a") || (!bl && !cJSON_GetObjectItem(a, "b")))
        return fail(cx, "invalid_args", "a and either b or baseline required",
                    "Pass a:{preset:\"this_month\"} with baseline:\"previous_period\", or a and b as periods.");
    if (!parse_period(cx, cJSON_GetObjectItem(a, "a"), &af, &at)) return NULL;
    if (bl) {
        if (!strcmp(bl, "previous_period")) { bt = af; bf = af - (at - af); }
        else if (!strcmp(bl, "previous_month")) {   /* calendar months: a = October -> September 1..October 1, never by a's length in seconds */
            bf = local_month_back(af); bt = local_month_back(at);
            if (bt > af) bt = af;
        }
        else if (!strcmp(bl, "same_month_last_year")) { bf = local_add(af, 'y', -1); bt = local_add(at, 'y', -1); }
        else return fail(cx, "invalid_args", "baseline: previous_period, previous_month or same_month_last_year", NULL);
        if (now_s() - bf > DAY_DEPTH_S + 86400u) return fail(cx, "out_of_history", "the baseline is beyond the stored daily history (400 days)", NULL);
    } else if (!parse_period(cx, cJSON_GetObjectItem(a, "b"), &bf, &bt)) return NULL;

    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    tot_t *ta = calloc(RBLLM_MAX_POINTS, sizeof(tot_t)), *tb = calloc(RBLLM_MAX_POINTS, sizeof(tot_t));
    if (!p || !ta || !tb) { free(p); free(ta); free(tb); return fail(cx, "internal", "out of memory", NULL); }
    int n = snapshot(p, RBLLM_MAX_POINTS);
    bool sel[RBLLM_MAX_POINTS];
    cJSON *r = NULL;
    if (!select_points(cx, cJSON_GetObjectItem(a, "channels"), p, n, sel)) goto out;
    if (!totals(cx, p, n, sel, af, at, ta) || !totals(cx, p, n, sel, bf, bt, tb)) {
        fail(cx, "timeout", "the history read took too long", "Narrow the periods or pick fewer channels."); goto out;
    }
    r = cJSON_CreateObject();
    cJSON *pa = cJSON_AddObjectToObject(r, "a"); add_time(pa, "from", af); add_time(pa, "to", at);
    cJSON *pb = cJSON_AddObjectToObject(r, "b"); add_time(pb, "from", bf); add_time(pb, "to", bt);
    cJSON_AddStringToObject(r, "currency", rbpower_tariff_currency());
    cJSON *arr = cJSON_AddArrayToObject(r, "channels");
    uint32_t newest = 0;
    for (int i = 0; i < n; i++) {
        if (!sel[i]) continue;
        cJSON *o = cJSON_CreateObject();
        char h[5]; uid_hex(p[i].unit_id, h);
        cJSON_AddStringToObject(o, "id", h);
        add_name(o, p[i].name);
        if (ta[i].any) cJSON_AddNumberToObject(o, "a_energy_kwh", r4(ta[i].e_in / 1000.0)); else cJSON_AddNullToObject(o, "a_energy_kwh");
        if (tb[i].any) cJSON_AddNumberToObject(o, "b_energy_kwh", r4(tb[i].e_in / 1000.0)); else cJSON_AddNullToObject(o, "b_energy_kwh");
        if (ta[i].any && tb[i].any) {
            double d = (ta[i].e_in - tb[i].e_in) / 1000.0;
            cJSON_AddNumberToObject(o, "delta_kwh", r4(d));
            if (tb[i].e_in > 1.0) cJSON_AddNumberToObject(o, "delta_pct", round(1000.0 * d * 1000.0 / tb[i].e_in) / 10.0);
            else cJSON_AddNullToObject(o, "delta_pct");
            cJSON_AddNumberToObject(o, "delta_cost", r4(ta[i].cost - tb[i].cost));
        } else { cJSON_AddNullToObject(o, "delta_kwh"); cJSON_AddNullToObject(o, "delta_pct"); cJSON_AddNullToObject(o, "delta_cost"); }
        /* the other direction: generation, a battery's discharge, the grid feed's export */
        if (ta[i].any) cJSON_AddNumberToObject(o, "a_export_kwh", r4(ta[i].e_out / 1000.0)); else cJSON_AddNullToObject(o, "a_export_kwh");
        if (tb[i].any) cJSON_AddNumberToObject(o, "b_export_kwh", r4(tb[i].e_out / 1000.0)); else cJSON_AddNullToObject(o, "b_export_kwh");
        if (ta[i].any && tb[i].any) {
            cJSON_AddNumberToObject(o, "delta_export_kwh", r4((ta[i].e_out - tb[i].e_out) / 1000.0));
            cJSON_AddNumberToObject(o, "delta_revenue", r4(ta[i].rev - tb[i].rev));
        } else { cJSON_AddNullToObject(o, "delta_export_kwh"); cJSON_AddNullToObject(o, "delta_revenue"); }
        if (!ta[i].any || !tb[i].any) cx->gaps = true;
        if (ta[i].newest > newest) newest = ta[i].newest;
        cJSON_AddItemToArray(arr, o);
    }
    if (cx->gaps) note(cx, "A period has no stored record for some channel (hub off, point added later, or beyond the history depth).");
    if ((at - af) != (bt - bf)) note(cx, "The two periods differ in length; compare per-day figures if that matters.");
    cx->as_of = newest;
out:
    free(p); free(ta); free(tb);
    return r;
}

/* ---- get_load_profile: avg / min / max power per stored record (hour or day) ---- */
typedef struct {
    cJSON *rows; int n, cap; uint32_t newest; bool no_ext; rbllm_cx_t *cx; bool is_hour; bool timed_out;
    uint32_t ts[400]; double avg[400], mn[400], mx[400]; bool ext[400];
} prof_ctx_t;
static void prof_emit(const rbpower_period_statistics_t *s, void *v)
{
    prof_ctx_t *c = (prof_ctx_t *)v;
    if (c->n >= c->cap || c->timed_out) return;
    uint32_t ts = rec_start(s, c->is_hour);
    uint32_t dur = s->v4_dur_s ? s->v4_dur_s : (c->is_hour ? 3600u : 86400u);
    double e = s->exact.valid ? s->exact.ec_mwh / 1000.0 : s->energy_period.consumed_wh;
    int k = c->n++;
    c->ts[k] = ts;
    c->avg[k] = dur ? e * 3600.0 / dur : 0;
    c->mn[k] = s->extremes.power_min; c->mx[k] = s->extremes.power_max;
    /* a record without extremes carries 0/0 (BASIC and recomputed DAY+ records); report null, not 0 */
    c->ext[k] = !(s->extremes.power_min == 0.0f && s->extremes.power_max == 0.0f) && s->extremes.power_max >= s->extremes.power_min;
    if (!c->ext[k]) c->no_ext = true;
    if (clamp_now(rec_end(s)) > c->newest) c->newest = clamp_now(rec_end(s));
    if (late(c->cx)) c->timed_out = true;
}
static cJSON *t_get_load_profile(const cJSON *a, rbllm_cx_t *cx)
{
    uint32_t from, to;
    if (!parse_period(cx, cJSON_GetObjectItem(a, "period"), &from, &to)) return NULL;
    const char *rs = cJSON_GetStringValue(cJSON_GetObjectItem(a, "resolution"));
    bool hour = !rs || !strcmp(rs, "hour");
    if (rs && !hour && strcmp(rs, "day")) return fail(cx, "invalid_args", "resolution: hour or day", NULL);
    if (hour && now_s() - from > HOUR_DEPTH_S) {   /* §5: coarsen, don't refuse */
        hour = false; cx->truncated = true;
        note(cx, "Hourly history is kept for 60 days; this older range is answered per day.");
    }
    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    if (!p) return fail(cx, "internal", "out of memory", NULL);
    int n = snapshot(p, RBLLM_MAX_POINTS);
    const char *ch = cJSON_GetStringValue(cJSON_GetObjectItem(a, "channel"));
    int idx = -1;
    for (int i = 0; ch && i < n; i++) { char h[5]; uid_hex(p[i].unit_id, h); if (!strcasecmp(ch, h) || (p[i].point[0] && !strcmp(ch, p[i].point))) idx = i; }
    if (idx < 0) { free(p); return fail(cx, "not_found", "unknown channel", "Call list_channels for the channel ids."); }
    const char *asked = hour ? "hour" : "day";
    int cap = (RBLLM_MAX_RESPONSE_BYTES - 1500) / 110;   /* ~110 B per row */
    if (cap > 400) cap = 400;
    if (hour && (to - from) / 3600u > (uint32_t)cap) { hour = false; cx->truncated = true;
        note(cx, "The resolution was lowered to day to fit the answer size; narrow the period for hourly rows."); }
    prof_ctx_t *c = heap_caps_calloc(1, sizeof(prof_ctx_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) { free(p); return fail(cx, "internal", "out of memory", NULL); }
    c->cap = cap; c->cx = cx; c->is_hour = hour;
    uint32_t now = now_s(), t2 = to > now ? now : to;
    if (hour) rbgrid_foreach_valid_record_ex(p[idx].unit_id, RBPOWER_STAT_PERIOD_1HOUR, RBPOWER_PERIOD_HOUR, from + 1, local_add(local_floor(t2 - 1, 'h'), 'h', 1), true, cx->deadline_us, prof_emit, c);
    else {
        uint32_t d1 = local_floor(t2, 'd');
        if (from < d1) rbgrid_foreach_valid_record_ex(p[idx].unit_id, RBPOWER_STAT_PERIOD_DAILY, RBPOWER_PERIOD_DAY, local_floor(from, 'd') + 1, d1, true, cx->deadline_us, prof_emit, c);
        if (t2 > d1) note(cx, "Today is not in the daily rows until the day closes; ask with resolution=hour for today.");
    }
    if (c->timed_out) { heap_caps_free(c); free(p); return fail(cx, "timeout", "the history read took too long", "Narrow the period."); }
    if (hour && c->n < c->cap) {   /* open hour, newest: prepend in record order (rows are emitted reversed) */
        uint32_t oh = 0, cov = 0; double oi = 0, oo = 0;
        rbpower_home_lock();
        bool have = rbpower_stats_open_hour(p[idx].unit_id, &oh, &cov, &oi, &oo);
        rbpower_home_unlock();
        bool dup = false;
        for (int k = 0; k < c->n; k++) if (c->ts[k] == oh) dup = true;
        if (have && cov && !dup && oh < to && oh + 3600u > from) {
            memmove(&c->ts[1], &c->ts[0], c->n * sizeof(c->ts[0])); memmove(&c->avg[1], &c->avg[0], c->n * sizeof(c->avg[0]));
            memmove(&c->mn[1], &c->mn[0], c->n * sizeof(c->mn[0])); memmove(&c->mx[1], &c->mx[0], c->n * sizeof(c->mx[0]));
            memmove(&c->ext[1], &c->ext[0], c->n * sizeof(c->ext[0]));
            c->ts[0] = oh; c->avg[0] = oi * 3600.0 / cov; c->ext[0] = false; c->n++; c->no_ext = true;
            if (oh + cov > c->newest) c->newest = clamp_now(oh + cov);
            cx->open_hour = true;
        }
    }
    cJSON *r = cJSON_CreateObject();
    add_period(r, from, to);
    char h[5]; uid_hex(p[idx].unit_id, h);
    cJSON_AddStringToObject(r, "channel", h);
    add_name(r, p[idx].name);
    cJSON_AddStringToObject(r, "resolution", hour ? "hour" : "day");
    cJSON *rows = cJSON_AddArrayToObject(r, "rows");
    for (int k = c->n - 1; k >= 0; k--) {   /* records come newest first */
        cJSON *o = cJSON_CreateObject();
        add_time(o, "from", c->ts[k]);
        cJSON_AddNumberToObject(o, "avg_w", r2(c->avg[k]));
        if (c->ext[k]) { cJSON_AddNumberToObject(o, "min_w", r2(c->mn[k])); cJSON_AddNumberToObject(o, "max_w", r2(c->mx[k])); }
        else { cJSON_AddNullToObject(o, "min_w"); cJSON_AddNullToObject(o, "max_w"); }
        cJSON_AddItemToArray(rows, o);
    }
    if (c->no_ext) note(cx, "min_w/max_w are null where the stored record carries no extremes; avg_w is energy over the period.");
    if (!c->n) { cx->gaps = true; note(cx, "No stored record in this period."); }
    if (strcmp(asked, hour ? "hour" : "day")) cx->resolution = "day"; else cx->resolution = asked;
    cx->as_of = c->newest;
    heap_caps_free(c); free(p);
    return r;
}

/* ---- get_import_export_balance ---- */
static cJSON *t_get_import_export_balance(const cJSON *a, rbllm_cx_t *cx)
{
    uint32_t from, to;
    if (!parse_period(cx, cJSON_GetObjectItem(a, "period"), &from, &to)) return NULL;
    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    tot_t *t = calloc(RBLLM_MAX_POINTS, sizeof(tot_t));
    if (!p || !t) { free(p); free(t); return fail(cx, "internal", "out of memory", NULL); }
    int n = snapshot(p, RBLLM_MAX_POINTS);
    bool sel[RBLLM_MAX_POINTS];
    int mains = 0, gens = 0;
    for (int i = 0; i < n; i++) {
        const char *rc = role_class(p[i].role);
        sel[i] = !strcmp(rc, "main") || !strcmp(rc, "generation");
        mains += !strcmp(rc, "main"); gens += !strcmp(rc, "generation");
    }
    if (!totals(cx, p, n, sel, from, to, t)) { free(p); free(t); return fail(cx, "timeout", "the history read took too long", "Narrow the period."); }
    double imp = 0, exp = 0, gen = 0; uint32_t newest = 0; bool any = false;
    for (int i = 0; i < n; i++) {
        if (!sel[i]) continue;
        if (t[i].newest > newest) newest = t[i].newest;
        any |= t[i].any;
        if (p[i].role == RBPOWER_ROLE_MAIN_SUPPLY) { imp += t[i].e_in; exp += t[i].e_out; }
        else gen += t[i].e_out;   /* production is energy out of a generation point (role-flipped convention) */
    }
    cJSON *r = cJSON_CreateObject();
    add_period(r, from, to);
    if (mains && any) { cJSON_AddNumberToObject(r, "import_kwh", r4(imp / 1000.0)); cJSON_AddNumberToObject(r, "export_kwh", r4(exp / 1000.0)); }
    else if (mains) { cJSON_AddNullToObject(r, "import_kwh"); cJSON_AddNullToObject(r, "export_kwh"); }
    else { cJSON_AddNullToObject(r, "import_kwh"); cJSON_AddNullToObject(r, "export_kwh");
           note(cx, "No point is set as main supply, so grid import and export are unknown."); }
    if (gens) {
        double self = gen - exp; if (self < 0) self = 0;
        cJSON_AddNumberToObject(r, "generation_kwh", r4(gen / 1000.0));
        cJSON_AddNumberToObject(r, "self_consumption_kwh", r4(self / 1000.0));
        double use = imp + self;
        if (gen > 0) cJSON_AddNumberToObject(r, "self_consumption_share", round(1000.0 * self / gen) / 1000.0); else cJSON_AddNullToObject(r, "self_consumption_share");
        if (use > 0) cJSON_AddNumberToObject(r, "self_sufficiency_share", round(1000.0 * self / use) / 1000.0); else cJSON_AddNullToObject(r, "self_sufficiency_share");
    } else {
        cJSON_AddNullToObject(r, "generation_kwh"); cJSON_AddNullToObject(r, "self_consumption_kwh");
        cJSON_AddNullToObject(r, "self_consumption_share"); cJSON_AddNullToObject(r, "self_sufficiency_share");
        note(cx, "No generation point (solar, inverter) is configured, so there is no production or self-consumption figure.");
    }
    if (!any) { cx->gaps = true; note(cx, "No stored record in this period."); }
    cx->as_of = newest;
    free(p); free(t);
    return r;
}

/* ---- check_installation: detects only; the fix is physical or in the module ---- */
static void finding(cJSON *arr, const char *type, const char *sev, const char *ch, const char *evidence, const char *fix)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", type);
    cJSON_AddStringToObject(o, "severity", sev);
    if (ch) cJSON_AddStringToObject(o, "channel", ch); else cJSON_AddNullToObject(o, "channel");
    cJSON_AddStringToObject(o, "evidence", evidence);
    cJSON_AddStringToObject(o, "fix_hint", fix);
    cJSON_AddItemToArray(arr, o);
}
/* check_installation reads history internally (export, records per point): those reads must not leave their period
 * meta (gaps, open hour, estimated cost, their notes) on an answer that returns findings, not figures. */
#define CX_QUIET_BEGIN(cx) cJSON *q_notes = (cx)->notes; bool q_g = (cx)->gaps, q_o = (cx)->open_hour, q_e = (cx)->estimated; (cx)->notes = NULL
#define CX_QUIET_END(cx) do { cJSON_Delete((cx)->notes); (cx)->notes = q_notes; (cx)->gaps = q_g; (cx)->open_hour = q_o; \
                              (cx)->estimated = q_e; (cx)->open_cost_exact = false; } while (0)
static cJSON *t_check_installation(const cJSON *a, rbllm_cx_t *cx)
{
    (void)a;
    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    if (!p) return fail(cx, "internal", "out of memory", NULL);
    int n = snapshot(p, RBLLM_MAX_POINTS);
    cJSON *r = cJSON_CreateObject();
    cJSON *f = cJSON_AddArrayToObject(r, "findings");
    char ev[200], h[5];
    int mains = 0;
    double main_w = 0, loads_w = 0; int loads_in_main = 0;
    uint16_t main_id = 0;
    for (int i = 0; i < n; i++) if (p[i].role == RBPOWER_ROLE_MAIN_SUPPLY) { mains++; main_w += p[i].p_w; if (!main_id) main_id = p[i].unit_id; }
    if (!mains) finding(f, "no_main_supply", "warning", NULL, "No point is set as main supply.",
                        "Set the role of the point on the grid feed to main supply on the hub's points page; cost and peaks need it.");
    /* sensors list: CT model, range, correction per point */
    char *sj = rbgrid_core_json_ctx() ? rbgrid_json_create_sensors_list(rbgrid_core_json_ctx()) : NULL;
    cJSON *sl = sj ? cJSON_Parse(sj) : NULL;
    free(sj);
    for (int i = 0; i < n; i++) {
        uid_hex(p[i].unit_id, h);
        if (!p[i].present) {
            snprintf(ev, sizeof(ev), "'%.40s' has no live source (module or node not heard).", p[i].name);
            finding(f, "channel_absent", "warning", h, ev, "Check the module or node behind it: cable, power, I2C address; the event log says when it went.");
            continue;
        }
        if (p[i].i_a > 0.3f && p[i].v_v < 1.0f) {
            snprintf(ev, sizeof(ev), "%.2f A flows but no voltage is measured, so no power or energy is booked.", p[i].i_a);
            finding(f, "no_voltage", "critical", h, ev, "Connect the module's voltage input (mains reference); current alone does not give energy.");
        }
        bool gen = !strcmp(role_class(p[i].role), "generation") || p[i].role == RBPOWER_ROLE_STORAGE_SYSTEM || p[i].role == RBPOWER_ROLE_MAIN_SUPPLY;
        if (!gen && p[i].p_w < -20.0f) {
            snprintf(ev, sizeof(ev), "A load point reads %.0f W (negative) right now.", p[i].p_w);
            finding(f, "ct_reversed", "warning", h, ev, "Turn the clamp round so its arrow points to the load. A software sign flip is not the fix.");
        }
        if (p[i].inside && p[i].inside == main_id) { loads_w += p[i].p_w; loads_in_main++; }
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(sl, "sensors")) {
            const char *uid = cJSON_GetStringValue(cJSON_GetObjectItem(e, "unit_id"));
            if (!uid || strcasecmp(uid, h)) continue;
            double fs = cJSON_GetNumberValue(cJSON_GetObjectItem(e, "ct_fs_a"));
            const char *corr = cJSON_GetStringValue(cJSON_GetObjectItem(e, "sct_correction"));
            const char *ctn = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ct_name"));
            if (fs > 0 && p[i].i_a > 0.95 * fs) {
                snprintf(ev, sizeof(ev), "%.1f A is at the clamp's measuring limit (%.1f A for %.30s).", p[i].i_a, fs, ctn ? ctn : "this clamp");
                finding(f, "ct_rating_mismatch", "warning", h, ev, "Fit a clamp with a higher rating and select its model for this channel.");
            }
            if (corr && !strcmp(corr, "uncalibrated")) {
                finding(f, "module_uncalibrated", "warning", h, "The module reports this channel as uncalibrated.", "Readings may be off; contact the module supplier.");
            }
        }
    }
    cJSON_Delete(sl);
    if (mains && loads_in_main && loads_w > main_w * 1.15 + 50.0) {
        snprintf(ev, sizeof(ev), "The points inside the main supply draw %.0f W together, more than the main supply's %.0f W.", loads_w, main_w);
        finding(f, "sum_mismatch", "warning", NULL, ev, "A sub-meter clamp may sit on the wrong wire or phase, or a point is set inside the wrong parent.");
    }
    for (uint8_t m = 0; m < RBPOWER_MODULE_DESC_MAX; m++) {
        rbpower_module_desc_t d;
        if (!rbpower_get_module_desc(m, &d) || d.present) continue;
        snprintf(ev, sizeof(ev), "Module %u (%.15s at 0x%02X) does not answer.", m, d.model, d.addr);
        finding(f, "module_missing", "critical", NULL, ev, "Check its cable and power; its channels read no energy meanwhile.");
    }
    /* reverse flow seen by the module (BASIC: negative power is not booked) — from this boot's event log */
    if (s_events_reader) {
        char *raw = s_events_reader(0, 100, 1);
        cJSON *src = raw ? cJSON_Parse(raw) : NULL;
        free(raw);
        char last_m[24] = "";
        bool active = false;
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(src, "lines")) {
            const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(e, "msg"));
            if (!m) continue;
            if (strstr(m, "reverse flow on a BASIC module")) { active = true; sscanf(m, "FAIL %23[^ ]", last_m); }
            else if (strstr(m, "reverse flow ended")) active = false;
        }
        cJSON_Delete(src);
        if (last_m[0]) {
            snprintf(ev, sizeof(ev), "The module reported reverse power on %s %s this boot; on a BASIC module that energy is not booked.",
                     last_m, active ? "(still ongoing)" : "(it ended since)");
            finding(f, "ct_reversed", active ? "critical" : "info", NULL, ev, "Turn the clamp round so its arrow points to the load.");
        }
    }
    if (!rbtime_is_time_valid()) finding(f, "clock_unsynced", "warning", NULL, "The hub clock is not synced.", "Check the hub's internet access (NTP); hours and costs need the clock.");
    {   /* time zone still the default: days, presets and tariff zones run in UTC */
        rbtime_config_t tc; memset(&tc, 0, sizeof(tc));
        rbtime_get_config(&tc);
        if (!tc.timezone[0] || !strcmp(tc.timezone, "UTC0") || !strcmp(tc.timezone, "UTC"))
            finding(f, "timezone_not_set", "warning", NULL, "The hub's time zone is UTC (the default).",
                    "Ask where the hub is and set the time zone: propose_config {region: {timezone}}; days, yesterday/last week and tariff zones follow it.");
        else if (rbtime_is_time_valid()) {   /* a zone set, but far from where the tariff's currency is used */
            time_t tn0 = (time_t)now_s(); struct tm lt0; rbgrid_localtime_r(&tn0, &lt0);
            double off_h = (double)(civil_seconds(&lt0) - (int64_t)tn0) / 3600.0;
            static const struct { const char *cur; double lo, hi; } CZ[] = {
                { "EUR", -1, 3 }, { "GBP", 0, 1 }, { "CHF", 1, 2 }, { "PLN", 1, 2 }, { "CZK", 1, 2 }, { "HUF", 1, 2 }, { "SEK", 1, 2 },
                { "NOK", 1, 2 }, { "DKK", 1, 2 }, { "RON", 2, 3 }, { "BGN", 2, 3 }, { "UAH", 2, 3 }, { "TRY", 3, 3 }, { "RUB", 2, 12 },
                { "CNY", 8, 8 }, { "JPY", 9, 9 }, { "KRW", 9, 9 }, { "INR", 5.5, 5.5 }, { "AUD", 8, 11 }, { "USD", -10, -4 }, { "CAD", -8, -2.5 } };
            const char *cur = rbpower_tariff_currency();
            for (size_t q = 0; cur && q < sizeof(CZ) / sizeof(CZ[0]); q++) {
                if (strcmp(cur, CZ[q].cur)) continue;
                if (off_h < CZ[q].lo - 0.01 || off_h > CZ[q].hi + 0.01) {
                    snprintf(ev, sizeof(ev), "The time zone puts the hub at UTC%+.1f h, far from where %s is used; days and tariff zones are cut at those hours.", off_h, cur);
                    finding(f, "timezone_implausible", "warning", NULL, ev, "Ask where the hub is; if the zone is wrong, propose_config {region: {timezone}}. A whole-hour error with clock.source internet is a zone error, not a clock error: do not use set_clock.");
                }
                break;
            }
        }
    }
    {   /* a module on the factory address: the next new module would collide with it */
        int ist = 0;
        cJSON *is = i2c_do("state", -1, -1, -1, &ist);
        const cJSON *im;
        cJSON_ArrayForEach(im, cJSON_GetObjectItem(is, "modules")) {
            if ((int)cJSON_GetNumberValue(cJSON_GetObjectItem(im, "addr")) != 0x50) continue;
            int mm = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(im, "module")), bb = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(im, "bus"));
            snprintf(ev, sizeof(ev), "Module %d on bus %d sits at 0x50, the address every new module arrives with.", mm, bb);
            finding(f, "module_on_factory_address", "info", NULL, ev,
                    "Move it to a free address (set_module_address, e.g. 0x51) before connecting another module on the same bus.");
        }
        cJSON_ArrayForEach(im, cJSON_GetObjectItem(is, "modules")) {   /* on the hub and answering, but measuring nothing */
            if (!cJSON_IsTrue(cJSON_GetObjectItem(im, "present")) || cJSON_GetNumberValue(cJSON_GetObjectItem(im, "metering_points")) > 0) continue;
            int mm = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(im, "module"));
            snprintf(ev, sizeof(ev), "Module %d answers but none of its current inputs carries a point, so it measures nothing.", mm);
            finding(f, "module_without_points", "warning", NULL, ev, "list_module_inputs, then create_point on each input in use.");
        }
        cJSON_Delete(is);
    }
    {   /* the grid feed exports but no generation point exists: a generator the hub cannot see (or a reversed grid clamp) */
        int gens = 0, mi = -1;
        for (int i = 0; i < n; i++) { const char *rc = role_class(p[i].role); if (strcmp(rc, "main") && strcmp(rc, "load")) gens++; if (p[i].role == RBPOWER_ROLE_MAIN_SUPPLY && mi < 0) mi = i; }
        if (!gens && mi >= 0) {
            bool sel1[RBLLM_MAX_POINTS] = { false }; sel1[mi] = true;
            tot_t *tt = calloc(RBLLM_MAX_POINTS, sizeof(tot_t));
            uint32_t to = now_s(), from = local_add(local_floor(to, 'd'), 'd', -7);
            CX_QUIET_BEGIN(cx);
            bool okt = tt && totals(cx, p, n, sel1, from, to, tt);
            CX_QUIET_END(cx);
            if (okt && tt[mi].any && tt[mi].e_out > 1000.0) {
                uid_hex(p[mi].unit_id, h);
                snprintf(ev, sizeof(ev), "The grid feed sent %.1f kWh to the grid in the last 7 days (took %.1f kWh), and no point is set as generation.", tt[mi].e_out / 1000.0, tt[mi].e_in / 1000.0);
                finding(f, "generation_not_configured", "warning", h, ev, "Ask the user about solar panels, an inverter or a battery: if there is one, create_point on its clamp with role renewable_source (or storage_system). If there is none, the grid clamp may face the wrong way.");
            }
            free(tt);
        }
    }
    {   /* a generation point that books mostly consumption: its clamp faces the wrong way, so production lands in energy_kwh
         * (live power positive, the same side as its kWh). ct_reversed covers loads only; storage is two-way and is left out. */
        bool selg[RBLLM_MAX_POINTS] = { false }; int ng = 0;
        for (int i = 0; i < n; i++)
            if (p[i].present && (p[i].role == RBPOWER_ROLE_RENEWABLE_SOURCE || p[i].role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY)) { selg[i] = true; ng++; }
        tot_t *tg = ng ? calloc(RBLLM_MAX_POINTS, sizeof(tot_t)) : NULL;
        bool okg = false;
        if (tg) {
            uint32_t to = now_s(), from = local_add(local_floor(to, 'd'), 'd', -7);
            CX_QUIET_BEGIN(cx);
            okg = totals(cx, p, n, selg, from, to, tg);
            CX_QUIET_END(cx);
        }
        for (int i = 0; i < n; i++) {
            if (!selg[i]) continue;
            uid_hex(p[i].unit_id, h);
            if (okg && tg[i].any && tg[i].e_in > 2000.0 && tg[i].e_out < 0.1 * tg[i].e_in) {
                snprintf(ev, sizeof(ev), "Generation point '%.40s' took %.1f kWh and delivered %.1f kWh in the last 7 days: its production is booked as consumption.",
                         p[i].name, tg[i].e_in / 1000.0, tg[i].e_out / 1000.0);
                finding(f, "generation_reversed", "warning", h, ev, "Turn this clamp round on the generator's wire. Until then its production counts as consumption in every report.");
            } else if (p[i].p_w > 200.0f) {
                snprintf(ev, sizeof(ev), "Generation point '%.40s' reads %.0f W (positive, the consumption side) right now.", p[i].name, p[i].p_w);
                finding(f, "generation_reversed", "info", h, ev, "If the generator is producing now, its clamp faces the wrong way: turn it round. A software sign flip is not the fix.");
            }
        }
        free(tg);
    }
    {   /* a point that is live but has stored nothing for a day: its energy is missing from every report */
        uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
        if (up_s > 7200) {
            bool sel2[RBLLM_MAX_POINTS]; int k2 = 0;
            for (int i = 0; i < n; i++) { sel2[i] = p[i].present; k2 += sel2[i]; }
            tot_t *t2 = k2 ? calloc(RBLLM_MAX_POINTS, sizeof(tot_t)) : NULL;
            uint32_t to = now_s(), from = to > 86400 ? to - 86400 : 0;
            CX_QUIET_BEGIN(cx);
            bool okt = t2 && totals(cx, p, n, sel2, from, to, t2);
            CX_QUIET_END(cx);
            if (okt) {
                for (int i = 0; i < n; i++) {
                    if (!sel2[i] || t2[i].any) continue;
                    uid_hex(p[i].unit_id, h);
                    snprintf(ev, sizeof(ev), "'%.40s' is live (%.0f W now) but has no stored record in the last 24 h, so its energy is missing from every report.", p[i].name, p[i].p_w);
                    finding(f, "channel_no_history", "warning", h, ev, "The point's records are not being written: note the time and report it; a restart (restart_hub) may resume writing. get_events {source: log} shows storage errors.");
                }
            }
            free(t2);
        }
    }
    {   /* the hub's own Wi-Fi access point still uses the printed setup password */
        char apw[72];
        if (!rbgrid_ap_user_password(apw, sizeof(apw)))
            finding(f, "ap_password_default", "warning", NULL, "The hub's own Wi-Fi access point still uses the setup password printed for all hubs, so anyone nearby who knows it can join the hub's network.",
                    "Ask the user to set their own access point password on the hub's Wi-Fi page (get_hub_info.hub_page_only: wifi).");
        memset(apw, 0, sizeof(apw));
    }
    if (s_ev_loaded && s_ev_mx) {   /* unexpected restarts (panic, watchdog, brownout) in the last 7 days, from the event log */
        int cnt = 0; uint32_t lts = 0; char lr[16] = ""; uint32_t nw = now_s();
        xSemaphoreTake(s_ev_mx, portMAX_DELAY);
        for (int i = 0; i < EV_N; i++) {
            const ev_rec_t *e = &s_ev.r[i];
            if (e->type != EV_REBOOT || e->sev != SEV_WARNING || !e->ts || nw < e->ts || nw - e->ts > 7u * 86400u) continue;
            cnt++;
            if (e->ts > lts) { lts = e->ts; strlcpy(lr, e->info, sizeof(lr)); }
        }
        xSemaphoreGive(s_ev_mx);
        if (cnt) {
            char ev[200], tb[32];
            time_t tt = (time_t)lts; struct tm lt; rbgrid_localtime_r(&tt, &lt); strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M", &lt);
            snprintf(ev, sizeof(ev), "The hub restarted unexpectedly %d time(s) in the last 7 days; the last at %s hub time (reset: %s).", cnt, tb, lr);
            finding(f, "unexpected_restart", cnt > 1 ? "warning" : "info", NULL, ev,
                    !strcmp(lr, "brownout") ? "A brownout is a power dip: ask the user to check the hub's power supply and cable."
                                            : "Stored data is kept across restarts. If it repeats, note what was happening at that time (get_events) and report it to the maker.");
        }
    }
    rbgrid_tariff_now_t tn; rbgrid_core_tariff_now(&tn);
    if (tn.placeholder) finding(f, "tariff_not_set", "warning", NULL, "No tariff plan is saved; every money figure is computed off a demo plan and is not the user's cost.", "Ask the user for their tariff and enter it with propose_config {tariff}.");
    cJSON_AddNumberToObject(r, "checked_channels", n);
    if (!cJSON_GetArraySize(f)) note(cx, "No installation problem found in the live data and this boot's log.");
    note(cx, "This tool detects only; it changes nothing on the hub.");
    free(p);
    cx->as_of = now_s();
    return r;
}

/* ---- detect_anomalies: heuristics over the stored hours and days, flagged as such ---- */
typedef struct { double night[2]; int night_n[2]; uint32_t split; double day[64]; uint32_t day_ts[64]; int nd; } anom_ctx_t;
static void anom_hour(const rbpower_period_statistics_t *s, void *v)
{
    anom_ctx_t *c = (anom_ctx_t *)v;
    uint32_t ts = rec_start(s, true);
    uint32_t dur = s->v4_dur_s ? s->v4_dur_s : 3600;
    if (dur < 3540) return;
    time_t mid = (time_t)(ts + 1800); struct tm lt; rbgrid_localtime_r(&mid, &lt);
    if (lt.tm_hour < 1 || lt.tm_hour >= 5) return;
    int k = ts >= c->split ? 1 : 0;
    double e = s->exact.valid ? s->exact.ec_mwh / 1000.0 : s->energy_period.consumed_wh;
    c->night[k] += e * 3600.0 / dur; c->night_n[k]++;
}
static void anom_day(const rbpower_period_statistics_t *s, void *v)
{
    anom_ctx_t *c = (anom_ctx_t *)v;
    if (c->nd >= 64) return;
    c->day_ts[c->nd] = rec_start(s, false);
    c->day[c->nd++] = s->exact.valid ? s->exact.ec_mwh / 1000.0 : s->energy_period.consumed_wh;
}
static cJSON *t_detect_anomalies(const cJSON *a, rbllm_cx_t *cx)
{
    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    if (!p) return fail(cx, "internal", "out of memory", NULL);
    int n = snapshot(p, RBLLM_MAX_POINTS);
    bool sel[RBLLM_MAX_POINTS];
    if (!select_points(cx, cJSON_GetObjectItem(a, "channels"), p, n, sel)) { free(p); return NULL; }
    uint32_t now = now_s();
    if (now < RBGRID_TS_SANE_MIN) { free(p); cx->retry_ms = 5000; return fail(cx, "clock_unsynced", "the hub clock is not set yet", "Retry in a few seconds."); }
    uint32_t today0 = local_floor(now, 'd');
    cJSON *r = cJSON_CreateObject();
    cJSON *f = cJSON_AddArrayToObject(r, "findings");
    anom_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) { cJSON_Delete(r); free(p); return fail(cx, "internal", "out of memory", NULL); }
    char ev[220], h[5];
    int analysed = 0;
    for (int i = 0; i < n; i++) {
        if (!sel[i] || late(cx)) continue;
        memset(c, 0, sizeof(*c));
        uid_hex(p[i].unit_id, h);
        /* night baseline: this week vs the week before, hours 01–05 local */
        c->split = local_add(today0, 'd', -7);
        rbgrid_foreach_valid_record_ex(p[i].unit_id, RBPOWER_STAT_PERIOD_1HOUR, RBPOWER_PERIOD_HOUR, local_add(today0, 'd', -14) + 1, now + 3600, true,
                                       cx->deadline_us, anom_hour, c);
        if (c->night_n[0] >= 8 && c->night_n[1] >= 8) {
            double w0 = c->night[0] / c->night_n[0], w1 = c->night[1] / c->night_n[1];
            if (w1 > w0 * 1.3 && w1 - w0 > 20.0) {
                snprintf(ev, sizeof(ev), "Night standby (01:00–05:00) averaged %.0f W over the last 7 days against %.0f W the week before.", w1, w0);
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "type", "night_baseline_rise"); cJSON_AddStringToObject(o, "channel", h);
                cJSON_AddStringToObject(o, "evidence", ev);
                cJSON_AddNumberToObject(o, "confidence", w1 - w0 > 60 ? 0.8 : 0.6);
                cJSON_AddItemToArray(f, o);
            }
            analysed++;
        }
        /* unusual day: a closed day above mean + 2.5 sigma of the last 30 */
        rbgrid_foreach_valid_record_ex(p[i].unit_id, RBPOWER_STAT_PERIOD_DAILY, RBPOWER_PERIOD_DAY, local_add(today0, 'd', -30) + 1, today0, true,
                                       cx->deadline_us, anom_day, c);
        if (c->nd >= 10) {
            double m = 0, v = 0;
            for (int k = 0; k < c->nd; k++) m += c->day[k];
            m /= c->nd;
            for (int k = 0; k < c->nd; k++) v += (c->day[k] - m) * (c->day[k] - m);
            double sd = sqrt(v / c->nd);
            for (int k = 0; k < c->nd; k++) {
                if (sd > 0 && c->day[k] > m + 2.5 * sd && c->day[k] - m > 500.0) {
                    char d[48]; iso_time(c->day_ts[k], d); d[10] = '\0';
                    snprintf(ev, sizeof(ev), "%s used %.1f kWh against a 30-day average of %.1f kWh.", d, c->day[k] / 1000.0, m / 1000.0);
                    cJSON *o = cJSON_CreateObject();
                    cJSON_AddStringToObject(o, "type", "unusual_day"); cJSON_AddStringToObject(o, "channel", h);
                    cJSON_AddStringToObject(o, "evidence", ev);
                    cJSON_AddNumberToObject(o, "confidence", 0.7);
                    cJSON_AddItemToArray(f, o);
                }
            }
            analysed++;
        }
    }
    free(c); free(p);
    if (late(cx)) { cJSON_Delete(r); return fail(cx, "timeout", "the history read took too long", "Pick fewer channels."); }
    cJSON_AddNumberToObject(r, "checks_with_enough_data", analysed);
    note(cx, "Findings are heuristics over the stored hours and days, not measurements of a fault; say so when you report them.");
    if (!analysed) { cx->gaps = true; note(cx, "Not enough history yet: the night check needs 14 days of hours, the day check 10 closed days."); }
    cx->as_of = today0;
    return r;
}

/* ---- get_regional_context: what the hub itself knows for a country (dynamic price sources, currency) ---- */
static cJSON *t_get_regional_context(const cJSON *a, rbllm_cx_t *cx)
{
    const char *cc = cJSON_GetStringValue(cJSON_GetObjectItem(a, "country"));
    if (cc && (strlen(cc) != 2)) return fail(cx, "invalid_args", "country: ISO 3166-1 alpha-2 code, e.g. DE", NULL);
    cJSON *cfg = rbdynprice_config_json();
    if (!cfg) return fail(cx, "internal", "out of memory", NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "hub_currency", rbpower_tariff_currency());
    if (cc) cJSON_AddStringToObject(r, "country", cc);
    cJSON *prov = cJSON_AddArrayToObject(r, "dynamic_price_sources");
    cJSON *countries = cc ? NULL : cJSON_AddArrayToObject(r, "countries_with_dynamic_prices");
    const cJSON *pv;
    cJSON_ArrayForEach(pv, cJSON_GetObjectItem(cfg, "providers")) {
        cJSON *zs = NULL;
        const cJSON *z;
        cJSON_ArrayForEach(z, cJSON_GetObjectItem(pv, "zones")) {
            const char *zc = cJSON_GetStringValue(cJSON_GetObjectItem(z, "country"));
            if (!zc) continue;
            if (countries) {
                bool dup = false; const cJSON *q;
                cJSON_ArrayForEach(q, countries) if (!strcmp(cJSON_GetStringValue(q), zc)) dup = true;
                if (!dup) cJSON_AddItemToArray(countries, cJSON_CreateString(zc));
                continue;
            }
            if (strcasecmp(zc, cc)) continue;
            if (!zs) zs = cJSON_CreateArray();
            cJSON *zo = cJSON_CreateObject();
            cJSON_AddStringToObject(zo, "zone", cJSON_GetStringValue(cJSON_GetObjectItem(z, "id")));
            cJSON_AddStringToObject(zo, "name", cJSON_GetStringValue(cJSON_GetObjectItem(z, "name")));
            cJSON_AddStringToObject(zo, "currency", cJSON_GetStringValue(cJSON_GetObjectItem(z, "currency")));
            cJSON_AddItemToArray(zs, zo);
        }
        bool local = cJSON_IsTrue(cJSON_GetObjectItem(pv, "local"));
        if (cc && !zs && !local) continue;
        if (!cc && !local) continue;   /* without a country: list only the country-free sources (Home Assistant push) */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", cJSON_GetStringValue(cJSON_GetObjectItem(pv, "id")));
        cJSON_AddStringToObject(o, "name", cJSON_GetStringValue(cJSON_GetObjectItem(pv, "name")));
        cJSON_AddStringToObject(o, "price_class", cJSON_GetStringValue(cJSON_GetObjectItem(pv, "class")));
        cJSON_AddBoolToObject(o, "needs_token", cJSON_IsTrue(cJSON_GetObjectItem(pv, "needs_token")));
        cJSON_AddBoolToObject(o, "pushed_to_hub", local);
        if (zs) cJSON_AddItemToObject(o, "zones", zs);
        cJSON_AddItemToArray(prov, o);
    }
    { const char *ps = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "state"));
      if (ps && ps[0]) cJSON_AddStringToObject(r, "price_state_now", ps); else cJSON_AddNullToObject(r, "price_state_now"); }   /* no data = null */
    cJSON_Delete(cfg);
    note(cx, "spot = wholesale day-ahead price without fees and VAT; all_in = the price a supplier charges. Set the source on the hub's dynamic price page.");
    note(cx, "Tariff templates and local terms are not built in yet; ask the user for the supplier's price sheet.");
    cx->as_of = now_s();
    return r;
}

/* ---- get_config: compact, LLM-facing projection of the settings (never the raw files, never a token) ---- */
/* The per-point fields propose_config writes beyond name/role/inside/clamp (get_config_schema: channels.writable). */
static const char *const CH_NUM_K[] = { "phase", "rated_power_w", "max_current_a", "charge_threshold_w", "idle_timeout_min" };
static void chan_extras(cJSON *o, uint16_t uid)
{
    char sn[48] = ""; double v[5] = { 0 }; bool have = false;
    rbpower_home_lock();
    const rbpower_unit_definition_t *d = rbpower_home_get_unit_definition((rbpower_unit_id_t)uid);
    if (d) {
        utf8_clean(d->short_name, sn, sizeof(sn));
        v[0] = d->phase; v[1] = r2(d->rated_power_w); v[2] = r2(d->max_current_a); v[3] = d->ev_charge_threshold_w; v[4] = d->ev_idle_timeout_min;
        have = true;
    }
    rbpower_home_unlock();
    if (!have) return;
    cJSON_AddStringToObject(o, "short_name", sn);
    for (int k = 0; k < 5; k++) cJSON_AddNumberToObject(o, CH_NUM_K[k], v[k]);
}
static void cfg_channels(cJSON *r)
{
    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    if (!p) return;
    int n = snapshot(p, RBLLM_MAX_POINTS);
    char *sj = rbgrid_core_json_ctx() ? rbgrid_json_create_sensors_list(rbgrid_core_json_ctx()) : NULL;
    cJSON *sl = sj ? cJSON_Parse(sj) : NULL;
    free(sj);
    cJSON *arr = cJSON_AddArrayToObject(r, "channels");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        char h[5]; uid_hex(p[i].unit_id, h);
        cJSON_AddStringToObject(o, "id", h);
        add_name(o, p[i].name);
        cJSON_AddStringToObject(o, "point", p[i].point);
        cJSON_AddStringToObject(o, "role_detail", role_name(p[i].role));
        if (p[i].inside) { char ih[5]; uid_hex(p[i].inside, ih); cJSON_AddStringToObject(o, "inside", ih); } else cJSON_AddNullToObject(o, "inside");
        chan_extras(o, p[i].unit_id);
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(sl, "sensors")) {
            const char *uid = cJSON_GetStringValue(cJSON_GetObjectItem(e, "unit_id"));
            if (!uid || strcasecmp(uid, h)) continue;
            cJSON *src = cJSON_AddObjectToObject(o, "source");
            const char *ifc = cJSON_GetStringValue(cJSON_GetObjectItem(e, "iface"));
            cJSON_AddStringToObject(src, "iface", ifc ? ifc : "");
            cJSON_AddNumberToObject(src, "module", cJSON_GetNumberValue(cJSON_GetObjectItem(e, "module")));
            cJSON_AddNumberToObject(src, "input", cJSON_GetNumberValue(cJSON_GetObjectItem(e, "ch")));
            const char *ctn = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ct_name"));
            if (ctn) { char sh[24]; ct_short(ctn, sh); cJSON_AddStringToObject(src, "ct_model", sh); }
            double ra = cJSON_GetNumberValue(cJSON_GetObjectItem(e, "ct_rated_a"));
            if (ra > 0) cJSON_AddNumberToObject(src, "ct_rating_a", ra);
            break;
        }
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_Delete(sl);
    free(p);
}
static void cfg_tariff(cJSON *r, rbllm_cx_t *cx)
{
    cJSON *t = cJSON_AddObjectToObject(r, "tariff");
    const rbpower_tariff_plan_t *tp = rbpower_tariff_get_current_plan();
    if (!tp) { cJSON_AddNullToObject(t, "plan"); return; }
    bool ph = rbpower_tariff_plan_is_placeholder(tp->plan_name, tp->provider_name);
    cJSON_AddBoolToObject(t, "plan_saved", !ph);
    if (ph) note(cx, "No tariff plan is saved: the figures are the built-in demo plan.");
    char b[64];
    utf8_clean(tp->plan_name, b, sizeof(b)); cJSON_AddStringToObject(t, "plan_name", b);
    utf8_clean(tp->provider_name, b, sizeof(b)); cJSON_AddStringToObject(t, "provider", b);
    cJSON_AddStringToObject(t, "currency", tp->currency_code);
    cJSON_AddNumberToObject(t, "vat_percent", r2(tp->vat_percent));
    cJSON_AddNumberToObject(t, "energy_tax_per_kwh", r4(tp->energy_tax_per_kwh));
    cJSON_AddNumberToObject(t, "grid_fee_per_kwh", r4(tp->grid_usage_fee_per_kwh));
    cJSON_AddNumberToObject(t, "levy_per_kwh", r4(tp->renewable_levy_per_kwh));
    cJSON_AddBoolToObject(t, "net_metering", tp->net_metering_enabled);
    { int dz = (int)tp->default_zone_id; cJSON_AddStringToObject(t, "default_zone", dz >= 0 && dz < RBPOWER_TARIFF_ZONE_COUNT ? ZONE_N[dz] : ""); }
    cJSON *zs = cJSON_AddArrayToObject(t, "zones");
    for (int z = 0; z < plan_nzones(tp); z++) {
        const rbpower_tariff_zone_t *zo = &tp->zones[z];
        if (!zo->is_enabled) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "zone", zone_nm(zo));
        utf8_clean(zo->zone_name, b, sizeof(b)); cJSON_AddStringToObject(o, "name", b);
        cJSON_AddNumberToObject(o, "buy_per_kwh", r4(zo->rate_buying_per_kwh));
        cJSON_AddNumberToObject(o, "sell_per_kwh", r4(zo->rate_selling_per_kwh));
        char w[12];
        if (zone_window(zo, w)) cJSON_AddStringToObject(o, "window", w); else cJSON_AddNullToObject(o, "window");
        char dd[32]; zone_days(zo, dd); cJSON_AddStringToObject(o, "days", dd[0] ? dd : "all");
        if (zo->conditions_count) cJSON_AddNumberToObject(o, "priority", zo->conditions[0].priority);
        cJSON_AddItemToArray(zs, o);
    }
    note(cx, "Zone rates are before fees and VAT. window is the zone's time of day [from, to) in hub time; null = the zone is set by weekdays, dates or several windows (edit those on the hub's tariff page). Where zones overlap, the hub's zone priority decides.");
    cJSON *dp = rbdynprice_config_json();
    if (dp) {
        cJSON *d = cJSON_AddObjectToObject(t, "dynamic_price");
        const char *k[] = { "enabled", "source", "backup_source", "zone", "country", "sell_mode", "state" };
        for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
            const cJSON *v = cJSON_GetObjectItem(dp, k[i]);
            if (v) cJSON_AddItemToObject(d, k[i], cJSON_Duplicate(v, false));
        }
        cJSON_Delete(dp);
    }
}
static cJSON *t_get_config(const cJSON *a, rbllm_cx_t *cx)
{
    const char *sec = cJSON_GetStringValue(cJSON_GetObjectItem(a, "section"));
    bool all = !sec;
    if (sec && strcmp(sec, "channels") && strcmp(sec, "tariff") && strcmp(sec, "notifications") && strcmp(sec, "region") &&
        strcmp(sec, "hub") && strcmp(sec, "integrations") && strcmp(sec, "dynamic_price"))
        return fail(cx, "invalid_args", "section: channels, tariff, notifications, region, hub, integrations or dynamic_price", NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "version", ver_sync("hub", NULL));   /* the number list_config_versions and rollback_to use */
    if (all || !strcmp(sec, "channels")) cfg_channels(r);
    if (all || !strcmp(sec, "tariff")) cfg_tariff(r, cx);
    if (all || !strcmp(sec, "notifications")) {
        cJSON *n = cJSON_AddObjectToObject(r, "notifications");
        cJSON *ru = rbact_rules_json(), *sg = rbact_signal_json(), *pk = rbpeak_json();
        if (ru) { cJSON *x = cJSON_DetachItemFromObject(ru, "rules"); cJSON_AddItemToObject(n, "price_rules", x ? x : cJSON_CreateArray()); cJSON_Delete(ru); }
        if (sg) { cJSON_DeleteItemFromObject(sg, "type"); cJSON_AddItemToObject(n, "price_signal", sg); }
        if (pk) { cJSON *c = cJSON_GetObjectItem(pk, "cycle_day"); cJSON_AddNumberToObject(n, "peaks_cycle_day", c ? c->valuedouble : 1); cJSON_Delete(pk); }
    }
    if (all || !strcmp(sec, "region")) {
        cJSON *g = cJSON_AddObjectToObject(r, "region");
        rbtime_config_t tc; memset(&tc, 0, sizeof(tc)); rbtime_get_config(&tc);
        cJSON_AddStringToObject(g, "timezone", tc.timezone);
        cJSON_AddStringToObject(g, "currency", rbpower_tariff_currency());
        const rbpower_tariff_plan_t *tp = rbpower_tariff_get_current_plan();
        cJSON_AddStringToObject(g, "region_code", tp ? tp->region_code : "");
        const rbpower_extended_config_t *ecr = rbgrid_core_ext_cfg();
        const rbpower_hardware_config_t *hw = ecr ? ecr->base_hardware_config : NULL;
        if (hw && hw->network.nominal_voltage > 0) cJSON_AddNumberToObject(g, "nominal_voltage", hw->network.nominal_voltage); else cJSON_AddNullToObject(g, "nominal_voltage");
        cJSON_AddNumberToObject(g, "nominal_frequency", hw && hw->network.nominal_frequency ? hw->network.nominal_frequency : 50);
    }
    if (all || !strcmp(sec, "hub")) {
        const rbpower_extended_config_t *ec = rbgrid_core_ext_cfg();
        cJSON *h = cJSON_AddObjectToObject(r, "hub");
        char b[48];
        utf8_clean(ec && ec->home_name ? ec->home_name : "", b, sizeof(b)); cJSON_AddStringToObject(h, "name", b);
        utf8_clean(ec && ec->location ? ec->location : "", b, sizeof(b)); cJSON_AddStringToObject(h, "location", b);
        static const char *const GS[] = { "", "1P", "SPLIT", "3P" };
        int gs = ec ? ec->grid_system : 0;
        if (gs > 0 && gs < 4) cJSON_AddStringToObject(h, "grid_system", GS[gs]); else cJSON_AddNullToObject(h, "grid_system");
    }
    if (all || !strcmp(sec, "integrations")) {
        cJSON *g = cJSON_AddObjectToObject(r, "integrations");
        cJSON *mq = cJSON_AddObjectToObject(g, "mqtt");
        char u[160], mu[176]; mqtt_saved_uri(u, sizeof(u));
        mask_uri(u[0] ? u : rbmqtt_broker_uri(), mu, sizeof(mu));
        cJSON_AddStringToObject(mq, "broker_uri", mu);
        cJSON_AddBoolToObject(mq, "connected", rbmqtt_is_connected());
        cJSON_AddStringToObject(mq, "home_assistant", "MQTT discovery on the same broker");
        cJSON *tl = cJSON_AddObjectToObject(g, "telemetry");
        cJSON_AddStringToObject(tl, "profile", rbgrid_telemetry_get_lean() ? "lean" : "full");
        cJSON_AddNumberToObject(tl, "frame", rbgrid_telemetry_get_frame());
    }
    if (sec && !strcmp(sec, "dynamic_price")) {
        cJSON *dp = rbdynprice_config_json();
        if (dp) { cJSON_DeleteItemFromObject(dp, "type"); cJSON_AddItemToObject(r, "dynamic_price", dp); }
        else cJSON_AddNullToObject(r, "dynamic_price");
    }
    note(cx, "To change: get_config_schema, then propose_config and, after the user's yes, apply_config. Points on new inputs: create_point.");
    cx->as_of = now_s();
    return r;
}

/* ============================================================================================ config writes (stage 2) */
/* Writable over LLM (v1): per channel name / role (supply roles allowed with a warning, operator 23.11) /
 * inside; notifications.peaks_cycle_day. Versions: /llm_cfgver.json keeps the last 5 states of exactly these fields.
 * A change made elsewhere (hub page, REST, HA) is noticed by its hash and recorded as source "hub" at the next config
 * call, so every path gets a version (§1.8). */
static bool s_cfg_changed;   /* set by apply for the call log (worker task only) */
#define CFGVER_FILE  "/llm_cfgver.json"
#define CFGVER_N     5
#define PROP_N       4
#define PROP_TTL_S   600

typedef struct { char token[33]; char client[33]; int64_t exp_us; char base[17]; cJSON *patch; char summary[120]; } prop_t;
static EXT_RAM_BSS_ATTR prop_t s_prop[PROP_N];

/* CT models the user can pick (schema enums.ct_model: code -> "SCT-013-030, 30 A"). */
static cJSON *ct_models(void)
{
    char *sc = rbgrid_build_schema_json();
    cJSON *j = sc ? cJSON_Parse(sc) : NULL;
    free(sc);
    cJSON *m = j ? cJSON_DetachItemFromObject(cJSON_GetObjectItem(j, "enums"), "ct_model") : NULL;
    cJSON_Delete(j);
    return m ? m : cJSON_CreateObject();
}
static void ct_short(const char *desc, char out[24])   /* "SCT-013-030, 30 A" -> "SCT-013-030" */
{
    strlcpy(out, desc ? desc : "", 24);
    char *c = strchr(out, ','); if (c) *c = '\0';
}
/* Per channel: its CT model (short name, "" if none) and whether the module lets the user pick one. */
typedef struct { char id[5]; char ct[24]; bool settable; } ct_info_t;
static int ct_infos(ct_info_t *out, int max)
{
    char *sj = rbgrid_core_json_ctx() ? rbgrid_json_create_sensors_list(rbgrid_core_json_ctx()) : NULL;
    cJSON *sl = sj ? cJSON_Parse(sj) : NULL;
    free(sj);
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(sl, "sensors")) {
        if (n >= max) break;
        const char *uid = cJSON_GetStringValue(cJSON_GetObjectItem(e, "unit_id"));
        const char *lbl = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ch_label"));
        if (!uid || (lbl && !strcmp(lbl, "V"))) continue;   /* the voltage input has no clamp */
        ct_info_t *c = &out[n++];
        strlcpy(c->id, uid, sizeof(c->id)); for (char *q = c->id; *q; q++) if (*q >= 'a' && *q <= 'f') *q -= 32;
        ct_short(cJSON_GetStringValue(cJSON_GetObjectItem(e, "ct_name")), c->ct);
        int mod = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "module"));
        const cJSON *m;
        c->settable = false;
        cJSON_ArrayForEach(m, cJSON_GetObjectItem(sl, "modules"))
            if ((int)cJSON_GetNumberValue(cJSON_GetObjectItem(m, "module")) == mod) c->settable = cJSON_IsTrue(cJSON_GetObjectItem(m, "ct_settable"));
    }
    cJSON_Delete(sl);
    return n;
}

/* Current state of the writable fields: {"channels":{"0006":{"name","role","inside","ct_model"}}, "peaks_cycle_day":n} */
static cJSON *cfg_capture(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *ch = cJSON_AddObjectToObject(r, "channels");
    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    if (p) {
        int n = snapshot(p, RBLLM_MAX_POINTS);
        for (int i = 0; i < n; i++) {
            char h[5]; uid_hex(p[i].unit_id, h);
            cJSON *o = cJSON_AddObjectToObject(ch, h);
            char b[48]; utf8_clean(p[i].name, b, sizeof(b));
            cJSON_AddStringToObject(o, "name", b);
            cJSON_AddStringToObject(o, "role", role_name(p[i].role));
            if (p[i].inside) { char ih[5]; uid_hex(p[i].inside, ih); cJSON_AddStringToObject(o, "inside", ih); }
            else cJSON_AddStringToObject(o, "inside", "");
            chan_extras(o, p[i].unit_id);
        }
        free(p);
    }
    ct_info_t ci[RBLLM_MAX_POINTS];
    int nci = ct_infos(ci, RBLLM_MAX_POINTS);
    for (int k = 0; k < nci; k++) {
        cJSON *o = cJSON_GetObjectItem(ch, ci[k].id);
        if (o && ci[k].ct[0]) cJSON_AddStringToObject(o, "ct_model", ci[k].ct);
    }
    cJSON *pk = rbpeak_json();
    const cJSON *cd = pk ? cJSON_GetObjectItem(pk, "cycle_day") : NULL;
    cJSON_AddNumberToObject(r, "peaks_cycle_day", cd ? cd->valuedouble : 1);
    cJSON_Delete(pk);
    const rbpower_tariff_plan_t *tp = rbpower_tariff_get_current_plan();
    if (tp) {
        cJSON *t = cJSON_AddObjectToObject(r, "tariff");
        char b[48];
        utf8_clean(tp->plan_name, b, sizeof(b)); cJSON_AddStringToObject(t, "plan_name", b);
        utf8_clean(tp->provider_name, b, sizeof(b)); cJSON_AddStringToObject(t, "provider", b);
        cJSON_AddStringToObject(t, "currency", tp->currency_code);
        cJSON_AddNumberToObject(t, "vat_percent", r4(tp->vat_percent));
        cJSON_AddNumberToObject(t, "energy_tax_per_kwh", r4(tp->energy_tax_per_kwh));
        cJSON_AddNumberToObject(t, "grid_fee_per_kwh", r4(tp->grid_usage_fee_per_kwh));
        cJSON_AddNumberToObject(t, "levy_per_kwh", r4(tp->renewable_levy_per_kwh));
        cJSON *zs = cJSON_AddObjectToObject(t, "zones");
        for (int z = 0; z < plan_nzones(tp); z++) {
            if (!tp->zones[z].is_enabled) continue;
            cJSON *o = cJSON_AddObjectToObject(zs, zone_nm(&tp->zones[z]));
            cJSON_AddNumberToObject(o, "buy_per_kwh", r4(tp->zones[z].rate_buying_per_kwh));
            cJSON_AddNumberToObject(o, "sell_per_kwh", r4(tp->zones[z].rate_selling_per_kwh));
            char w[12]; if (zone_window(&tp->zones[z], w)) cJSON_AddStringToObject(o, "window", w);
            char dd[32]; zone_days(&tp->zones[z], dd); cJSON_AddStringToObject(o, "days", dd);
        }
        { int dz = (int)tp->default_zone_id; cJSON_AddStringToObject(t, "default_zone", dz >= 0 && dz < RBPOWER_TARIFF_ZONE_COUNT ? ZONE_N[dz] : ""); }
    }
    {   /* the time zone and the hub's labels: versioned too, so apply_config never reports a version it did not record */
        rbtime_config_t tc; memset(&tc, 0, sizeof(tc)); rbtime_get_config(&tc);
        cJSON_AddStringToObject(cJSON_AddObjectToObject(r, "region"), "timezone", tc.timezone);
        const rbpower_extended_config_t *ec = rbgrid_core_ext_cfg();
        cJSON *h = cJSON_AddObjectToObject(r, "hub");
        char b[48];
        utf8_clean(ec && ec->home_name ? ec->home_name : "", b, sizeof(b)); cJSON_AddStringToObject(h, "name", b);
        utf8_clean(ec && ec->location ? ec->location : "", b, sizeof(b)); cJSON_AddStringToObject(h, "location", b);
    }
    return r;
}
/* The zone's time window "HH:MM-HH:MM" when it has exactly one active TIME_RANGE condition (the only shape writable over
 * LLM: other conditions — weekdays, dates, holidays — stay as they are). Half-open [from, to); to < from wraps midnight. */
static bool zone_window(const rbpower_tariff_zone_t *zo, char out[12])
{
    int n = 0, k = -1;
    for (int j = 0; j < 4 && j < zo->conditions_count; j++)
        if (zo->conditions[j].is_active && zo->conditions[j].condition_type == RBPOWER_TARIFF_CONDITION_TIME_RANGE) { n++; k = j; }
    if (n != 1) return false;
    const typeof(zo->conditions[k].params.time_range) *tr = &zo->conditions[k].params.time_range;
    snprintf(out, 12, "%02u:%02u-%02u:%02u", (unsigned)(tr->start_hour % 24), (unsigned)(tr->start_minute % 60),
             (unsigned)(tr->end_hour % 24), (unsigned)(tr->end_minute % 60));
    return true;
}
/* Parse "HH:MM-HH:MM" (minutes on the 15-min grid, 24:00 allowed as an end). */
static bool parse_window(const char *w, int *sh, int *sm, int *eh, int *em)
{
    if (!w || strlen(w) != 11 || w[2] != ':' || w[5] != '-' || w[8] != ':') return false;
    for (int i = 0; i < 11; i++) if (i != 2 && i != 5 && i != 8 && (w[i] < '0' || w[i] > '9')) return false;
    *sh = (w[0] - '0') * 10 + (w[1] - '0'); *sm = (w[3] - '0') * 10 + (w[4] - '0');
    *eh = (w[6] - '0') * 10 + (w[7] - '0'); *em = (w[9] - '0') * 10 + (w[10] - '0');
    if (*eh == 24 && *em == 0) *eh = 0;
    if (*sh > 23 || *eh > 23 || *sm % 15 || *em % 15 || *sm > 45 || *em > 45) return false;
    return (*sh * 60 + *sm) != (*eh * 60 + *em);
}

static void cfg_hash(const cJSON *cap, char out[17])
{
    char *t = cJSON_PrintUnformatted(cap);
    uint8_t h[32] = {0};
    if (t) { mbedtls_sha256((const unsigned char *)t, strlen(t), h, 0); free(t); }
    hex_of(h, 8, out);
}
static cJSON *ver_load(void)
{
    size_t sz = 0;
    cJSON *v = NULL;
    if (littlefs_raw_size(CFGVER_FILE, &sz) == ESP_OK && sz > 0 && sz < 32768) {
        char *b = malloc(sz + 1); size_t got = 0;
        if (b && littlefs_raw_read(CFGVER_FILE, b, sz, &got) == ESP_OK) { b[got] = '\0'; v = cJSON_Parse(b); }
        free(b);
    }
    if (!cJSON_IsObject(v)) { cJSON_Delete(v); v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "next", 1); cJSON_AddArrayToObject(v, "versions"); }
    return v;
}
static void ver_save(cJSON *v)
{
    char *t = cJSON_PrintUnformatted(v);
    if (t) { if (littlefs_raw_write(CFGVER_FILE, t, strlen(t)) != ESP_OK) rbpower_diag_printf("FAIL llm: %s not saved", CFGVER_FILE); free(t); }
}
/* Record the current state as a new version if it differs from the newest one. Returns the newest version number. */
static int ver_sync(const char *source, const char *summary)
{
    cJSON *cap = cfg_capture();
    char h[17]; cfg_hash(cap, h);
    cJSON *v = ver_load();
    cJSON *arr = cJSON_GetObjectItem(v, "versions");
    int n = cJSON_GetArraySize(arr);
    cJSON *last = n ? cJSON_GetArrayItem(arr, n - 1) : NULL;
    const char *lh = last ? cJSON_GetStringValue(cJSON_GetObjectItem(last, "hash")) : NULL;
    int num = last ? (int)cJSON_GetNumberValue(cJSON_GetObjectItem(last, "version")) : 0;
    cJSON *lst = last ? cJSON_GetObjectItem(last, "state") : NULL;
    const cJSON *lch0 = lst ? cJSON_GetArrayItem(cJSON_GetObjectItem(lst, "channels"), 0) : NULL;
    bool old_fmt = lst && (!cJSON_GetObjectItem(lst, "region") || (lch0 && !cJSON_GetObjectItem(lch0, "phase")));
    if (lh && strcmp(lh, h) && old_fmt) {
        /* the newest version was recorded before the time zone, hub labels or the per-point extras were versioned: if the
         * rest is equal, add them to that version in place instead of recording a spurious "changed outside the LLM API" one */
        cJSON *old = cJSON_Duplicate(cap, true);
        if (!cJSON_GetObjectItem(lst, "region")) { cJSON_DeleteItemFromObject(old, "region"); cJSON_DeleteItemFromObject(old, "hub"); }
        if (lch0 && !cJSON_GetObjectItem(lch0, "phase")) {
            cJSON *c;
            cJSON_ArrayForEach(c, cJSON_GetObjectItem(old, "channels")) {
                cJSON_DeleteItemFromObject(c, "short_name");
                for (int k = 0; k < 5; k++) cJSON_DeleteItemFromObject(c, CH_NUM_K[k]);
            }
        }
        char ho[17]; cfg_hash(old, ho);
        cJSON_Delete(old);
        if (!strcmp(ho, lh)) {
            cJSON_ReplaceItemInObject(last, "state", cJSON_Duplicate(cap, true));
            cJSON_ReplaceItemInObject(last, "hash", cJSON_CreateString(h));
            ver_save(v);
            lh = h;
        }
    }
    if (!lh || strcmp(lh, h)) {
        int next = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(v, "next"));
        if (next < 1) next = 1;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "version", next);
        cJSON_AddNumberToObject(e, "ts", now_s());
        cJSON_AddStringToObject(e, "source", source);
        cJSON_AddStringToObject(e, "summary", summary ? summary : (last ? "changed outside the LLM API" : "state when the LLM API first read the config"));
        cJSON_AddStringToObject(e, "hash", h);
        cJSON_AddItemToObject(e, "state", cap); cap = NULL;
        cJSON_AddItemToArray(arr, e);
        while (cJSON_GetArraySize(arr) > CFGVER_N) cJSON_DeleteItemFromArray(arr, 0);
        cJSON_ReplaceItemInObject(v, "next", cJSON_CreateNumber(next + 1));
        ver_save(v);
        num = next;
        char inf[40]; snprintf(inf, sizeof(inf), "%d|%s", next, source ? source : "?");
        ev_add(EV_CONFIG, SEV_INFO, 0, inf);
    }
    cJSON_Delete(cap);
    cJSON_Delete(v);
    return num;
}

static bool valid_name(const char *s)
{
    if (!s || !s[0] || strlen(s) > 31) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p < 0x20) return false;
    char b[48]; utf8_clean(s, b, sizeof(b));
    return !strcmp(b, s);
}
static int role_from_name(const char *s)
{
    for (int r = 0; r < 10; r++) if (s && !strcmp(s, role_name((uint8_t)r))) return r;
    return -1;
}

/* ---- settings beyond channels/tariff (operator 23.11: the LLM interface covers the hub's settings, not half of them) ---- */
static const char *const PATCH_KEYS[] = { "channels", "tariff", "peaks_cycle_day", "region", "hub", "price_rules", "price_signal", "peaks_reset", "dynamic_price", "mqtt", "telemetry" };
static bool patch_key_ok(const char *k)
{
    for (size_t i = 0; i < sizeof(PATCH_KEYS) / sizeof(PATCH_KEYS[0]); i++) if (k && !strcmp(k, PATCH_KEYS[i])) return true;
    return false;
}
static void dadd(cJSON *diff, const char *path, const char *from, const char *to)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "path", path); cJSON_AddStringToObject(d, "from", from ? from : ""); cJSON_AddStringToObject(d, "to", to ? to : "");
    cJSON_AddItemToArray(diff, d);
}
static bool mqtt_uri_ok(const char *u)
{
    return u && (!strncmp(u, "mqtt://", 7) || !strncmp(u, "mqtts://", 8)) && strlen(u) < 160 && !strstr(u, "://***@");
}
static void mqtt_saved_uri(char *out, size_t n)
{
    size_t act = 0;
    out[0] = '\0';
    if (!(littlefs_raw_exists("/mqtt_broker") && littlefs_raw_read("/mqtt_broker", out, n - 1, &act) == ESP_OK)) out[0] = '\0';
    else out[act < n ? act : n - 1] = '\0';
}
/* Common IANA zone names -> their POSIX rule (tzdata, current rules), so an assistant can pass "Europe/Lisbon" instead of
 * composing a DST rule from memory. Unknown names are not guessed: the caller then asks for the POSIX string. */
static const struct { const char *iana, *posix; } TZ_IANA[] = {
    { "UTC", "UTC0" },
    { "Etc/UTC", "UTC0" },
    { "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Atlantic/Madeira", "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Atlantic/Canary", "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0" },
    { "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Budapest", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Belgrade", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zagreb", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Ljubljana", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Bratislava", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Luxembourg", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Monaco", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Malta", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Andorra", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Tirane", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Sarajevo", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Skopje", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Podgorica", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Vaduz", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/San_Marino", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Vatican", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Gibraltar", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Busingen", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Africa/Ceuta", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Kiev", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Riga", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Tallinn", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Vilnius", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Sofia", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Chisinau", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Mariehamn", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Asia/Nicosia", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Nicosia", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Istanbul", "<+03>-3" },
    { "Europe/Moscow", "MSK-3" },
    { "Europe/Minsk", "<+03>-3" },
    { "Europe/Kaliningrad", "EET-2" },
    { "Europe/Samara", "<+04>-4" },
    { "Asia/Yekaterinburg", "<+05>-5" },
    { "Asia/Omsk", "<+06>-6" },
    { "Asia/Novosibirsk", "<+07>-7" },
    { "Asia/Krasnoyarsk", "<+07>-7" },
    { "Asia/Irkutsk", "<+08>-8" },
    { "Asia/Yakutsk", "<+09>-9" },
    { "Asia/Vladivostok", "<+10>-10" },
    { "Asia/Magadan", "<+11>-11" },
    { "Asia/Kamchatka", "<+12>-12" },
    { "Asia/Tbilisi", "<+04>-4" },
    { "Asia/Yerevan", "<+04>-4" },
    { "Asia/Baku", "<+04>-4" },
    { "Asia/Tashkent", "<+05>-5" },
    { "Asia/Almaty", "<+05>-5" },
    { "Asia/Dubai", "<+04>-4" },
    { "Asia/Kolkata", "IST-5:30" },
    { "Asia/Calcutta", "IST-5:30" },
    { "Asia/Shanghai", "CST-8" },
    { "Asia/Hong_Kong", "HKT-8" },
    { "Asia/Taipei", "CST-8" },
    { "Asia/Singapore", "<+08>-8" },
    { "Asia/Kuala_Lumpur", "<+08>-8" },
    { "Asia/Bangkok", "<+07>-7" },
    { "Asia/Ho_Chi_Minh", "<+07>-7" },
    { "Asia/Jakarta", "WIB-7" },
    { "Asia/Manila", "PST-8" },
    { "Asia/Tokyo", "JST-9" },
    { "Asia/Seoul", "KST-9" },
    { "Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "Africa/Johannesburg", "SAST-2" },
    { "Africa/Lagos", "WAT-1" },
    { "Africa/Nairobi", "EAT-3" },
    { "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Detroit", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Toronto", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Chicago", "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Winnipeg", "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Denver", "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Edmonton", "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Phoenix", "MST7" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Vancouver", "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Halifax", "AST4ADT,M3.2.0,M11.1.0" },
    { "Pacific/Honolulu", "HST10" },
    { "America/Mexico_City", "CST6" },
    { "America/Sao_Paulo", "<-03>3" },
    { "America/Argentina/Buenos_Aires", "<-03>3" },
    { "America/Bogota", "<-05>5" },
    { "America/Lima", "<-05>5" },
    { "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Hobart", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Brisbane", "AEST-10" },
    { "Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "Australia/Darwin", "ACST-9:30" },
    { "Australia/Perth", "AWST-8" },
    { "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
static const char *tz_from_iana(const char *n)
{
    if (!n || !strchr(n, '/')) return strcmp(n ? n : "", "UTC") ? NULL : "UTC0";
    for (size_t i = 0; i < sizeof(TZ_IANA) / sizeof(TZ_IANA[0]); i++) if (!strcasecmp(n, TZ_IANA[i].iana)) return TZ_IANA[i].posix;
    return NULL;
}
/* Validate the extra sections into norm; false = fail set. */
static bool check_more(rbllm_cx_t *cx, const cJSON *patch, cJSON *norm, cJSON *diff, cJSON *warn)
{
    char m[160];
    const cJSON *rg = cJSON_GetObjectItem(patch, "region");
    if (rg) {
        const cJSON *k;
        cJSON_ArrayForEach(k, rg) if (strcmp(k->string, "timezone") && strcmp(k->string, "nominal_voltage") && strcmp(k->string, "nominal_frequency")) {
            snprintf(m, sizeof(m), "region.%.24s: timezone, nominal_voltage or nominal_frequency (the currency is in tariff)", k->string); fail(cx, "invalid_args", m, NULL); return false;
        }
        const cJSON *jtz = cJSON_GetObjectItem(rg, "timezone");
        if (jtz) {
            const char *tz = cJSON_GetStringValue(jtz);
            const char *iana = NULL;
            /* an IANA name is Area/City: a slash and no comma (a POSIX rule has its slashes only inside its comma-separated
             * DST rules, e.g. CET-1CEST,M3.5.0,M10.5.0/3) */
            if (tz && ((strchr(tz, '/') && !strchr(tz, ',') && !strchr(tz, '<')) || !strcmp(tz, "UTC"))) {   /* an IANA name: take its POSIX rule from the table, never a guess */
                const char *px = tz_from_iana(tz);
                if (!px) { snprintf(m, sizeof(m), "region.timezone: \"%.40s\" is not in the hub's zone table", tz);
                           fail(cx, "invalid_args", m, "Pass the POSIX TZ string for that place, or the IANA name of a nearby city in the same zone (e.g. Europe/Berlin, America/New_York)."); return false; }
                iana = tz; tz = px;
            }
            if (!tz || !tz[0] || strlen(tz) > 31 || !rbtime_timezone_is_valid(tz)) {
                fail(cx, "invalid_args", "region.timezone: a POSIX TZ string with its offset", "E.g. \"CET-1CEST,M3.5.0,M10.5.0/3\" (Berlin), \"EET-2EEST,M3.5.0/3,M10.5.0/4\" (Kyiv), \"MSK-3\", \"CST-8\" (China), \"<+04>-4\"."); return false;
            }
            rbtime_config_t tc; memset(&tc, 0, sizeof(tc)); rbtime_get_config(&tc);
            if (strcmp(tc.timezone, tz)) {
                cJSON *ng = cJSON_GetObjectItem(norm, "region"); if (!ng) ng = cJSON_AddObjectToObject(norm, "region");
                cJSON_AddStringToObject(ng, "timezone", tz);
                dadd(diff, "region.timezone", tc.timezone, tz);
                if (iana) { char wv[120]; snprintf(wv, sizeof(wv), "%.40s is stored as its rule %s.", iana, tz); cJSON_AddItemToArray(warn, cJSON_CreateString(wv)); }
                cJSON_AddItemToArray(warn, cJSON_CreateString("From now on days, yesterday/last week and tariff zones follow the new time zone. Hourly figures already stored are kept by their instant, so they show at their new local hours; closed days, weeks and months keep the day boundaries of the old time zone; costs already booked keep the zone and price they were booked at."));
            }
        }
        const rbpower_extended_config_t *ecn = rbgrid_core_ext_cfg();
        const rbpower_hardware_config_t *hw = ecn ? ecn->base_hardware_config : NULL;
        const cJSON *jv = cJSON_GetObjectItem(rg, "nominal_voltage"), *jf = cJSON_GetObjectItem(rg, "nominal_frequency");
        if (jv) {
            if (!cJSON_IsNumber(jv) || jv->valuedouble < 90 || jv->valuedouble > 260) { fail(cx, "invalid_args", "region.nominal_voltage: the mains line-to-neutral voltage, 90..260 V", "230 in most of Europe and Asia, 220 in China and the CIS, 120 in North America, 100 in Japan."); return false; }
            float was = hw ? hw->network.nominal_voltage : 0;
            if (fabsf(was - (float)jv->valuedouble) > 0.05f) {
                cJSON *ng = cJSON_GetObjectItem(norm, "region"); if (!ng) ng = cJSON_AddObjectToObject(norm, "region");
                cJSON_AddNumberToObject(ng, "nominal_voltage", jv->valuedouble);
                char a[16], b[16]; snprintf(a, sizeof(a), was > 0 ? "%.0f V" : "unset", was); snprintf(b, sizeof(b), "%.0f V", jv->valuedouble);
                dadd(diff, "region.nominal_voltage", a, b);
                cJSON_AddItemToArray(warn, cJSON_CreateString("The nominal voltage is used where a channel measures current only (power = voltage x current). It takes full effect after restart_hub."));
            }
        }
        if (jf) {
            if (!cJSON_IsNumber(jf) || (jf->valueint != 50 && jf->valueint != 60) || jf->valuedouble != jf->valueint) { fail(cx, "invalid_args", "region.nominal_frequency: 50 or 60 (Hz)", NULL); return false; }
            int was = hw ? hw->network.nominal_frequency : 0;
            if (was != jf->valueint) {
                cJSON *ng = cJSON_GetObjectItem(norm, "region"); if (!ng) ng = cJSON_AddObjectToObject(norm, "region");
                cJSON_AddNumberToObject(ng, "nominal_frequency", jf->valueint);
                char a[16], b[16]; snprintf(a, sizeof(a), "%d Hz", was); snprintf(b, sizeof(b), "%d Hz", jf->valueint);
                dadd(diff, "region.nominal_frequency", a, b);
                cJSON_AddItemToArray(warn, cJSON_CreateString("The nominal frequency applies after restart_hub; it is the fallback while no module reports a measured one."));
            }
        }
    }
    const cJSON *hb = cJSON_GetObjectItem(patch, "hub");
    if (hb) {
        const rbpower_extended_config_t *ec = rbgrid_core_ext_cfg();
        cJSON *nh = NULL;
        const cJSON *k;
        cJSON_ArrayForEach(k, hb) {
            if (!strcmp(k->string, "grid_system")) {
                static const char *const GS[] = { "", "1P", "SPLIT", "3P" };
                const char *v = cJSON_GetStringValue(k);
                int gi = -1; for (int q = 1; q < 4; q++) if (v && !strcmp(v, GS[q])) gi = q;
                if (gi < 0) { fail(cx, "invalid_args", "hub.grid_system: \"1P\" (single phase), \"SPLIT\" (split phase, 120/240 V) or \"3P\" (three phase)", NULL); return false; }
                int wg = ec ? ec->grid_system : 0;
                if (wg == gi) continue;
                if (!nh) nh = cJSON_AddObjectToObject(norm, "hub");
                cJSON_AddStringToObject(nh, "grid_system", v);
                dadd(diff, "hub.grid_system", GS[wg < 4 ? wg : 0][0] ? GS[wg < 4 ? wg : 0] : "unset", v);
                continue;
            }
            if (strcmp(k->string, "name") && strcmp(k->string, "location")) { snprintf(m, sizeof(m), "hub.%.24s: name, location or grid_system", k->string); fail(cx, "invalid_args", m, NULL); return false; }
            if (!strcmp(k->string, "location") && (cJSON_IsNull(k) || (cJSON_IsString(k) && !k->valuestring[0]))) {   /* clear it */
                const char *wl = ec && ec->location ? ec->location : "";
                if (!wl[0]) continue;
                if (!nh) nh = cJSON_AddObjectToObject(norm, "hub");
                cJSON_AddStringToObject(nh, "location", "");
                dadd(diff, "hub.location", wl, "");
                continue;
            }
            if (!valid_name(cJSON_GetStringValue(k))) { snprintf(m, sizeof(m), !strcmp(k->string, "location") ? "hub.location: 1..31 bytes of UTF-8 text, or \"\" / null to clear it" : "hub.%s: 1..31 bytes of UTF-8 text", k->string); fail(cx, "invalid_args", m, NULL); return false; }
            const char *was = ec ? (!strcmp(k->string, "name") ? ec->home_name : ec->location) : "";
            if (was && !strcmp(was, k->valuestring)) continue;
            if (!nh) nh = cJSON_AddObjectToObject(norm, "hub");
            cJSON_AddStringToObject(nh, k->string, k->valuestring);
            char path[24]; snprintf(path, sizeof(path), "hub.%s", k->string);
            dadd(diff, path, was, k->valuestring);
        }
    }
    const cJSON *pr = cJSON_GetObjectItem(patch, "price_rules"), *ps = cJSON_GetObjectItem(patch, "price_signal");
    if (pr) {
        if (!cJSON_IsArray(pr)) { fail(cx, "invalid_args", "price_rules: the whole list (array), as get_config notifications.price_rules shows it", NULL); return false; }
        cJSON *cr = rbact_rules_json(); int was = cJSON_GetArraySize(cJSON_GetObjectItem(cr, "rules")); cJSON_Delete(cr);
        cJSON_AddItemToObject(norm, "price_rules", cJSON_Duplicate(pr, true));
        char a[24], b[24]; snprintf(a, sizeof(a), "%d rule(s)", was); snprintf(b, sizeof(b), "%d rule(s)", cJSON_GetArraySize(pr));
        dadd(diff, "notifications.price_rules", a, b);
        cJSON_AddItemToArray(warn, cJSON_CreateString("The list replaces the current rules. The hub checks each rule when it saves them; a bad rule is refused at apply_config with its field named, and nothing is changed."));
    }
    if (ps) {
        if (!cJSON_IsObject(ps)) { fail(cx, "invalid_args", "price_signal: an object, as get_config notifications.price_signal shows it", NULL); return false; }
        cJSON_AddItemToObject(norm, "price_signal", cJSON_Duplicate(ps, true));
        dadd(diff, "notifications.price_signal", "current", "updated");
    }
    const cJSON *pkr = cJSON_GetObjectItem(patch, "peaks_reset");
    if (pkr) {
        if (!cJSON_IsTrue(pkr)) { fail(cx, "invalid_args", "peaks_reset: true", NULL); return false; }
        cJSON_AddTrueToObject(norm, "peaks_reset");
        dadd(diff, "notifications.peaks", "this cycle's peaks", "cleared");
        cJSON_AddItemToArray(warn, cJSON_CreateString("The peaks of the current cycle are cleared; this cannot be undone."));
    }
    const cJSON *dp = cJSON_GetObjectItem(patch, "dynamic_price");
    if (dp) {
        if (!cJSON_IsObject(dp)) { fail(cx, "invalid_args", "dynamic_price: an object with the fields get_config_schema {section: dynamic_price} lists", NULL); return false; }
        cJSON_AddItemToObject(norm, "dynamic_price", cJSON_Duplicate(dp, true));
        const cJSON *k;
        cJSON_ArrayForEach(k, dp) {
            char path[48]; snprintf(path, sizeof(path), "dynamic_price.%.30s", k->string);
            char to[40]; if (cJSON_IsString(k)) snprintf(to, sizeof(to), "%.36s", k->valuestring); else if (cJSON_IsNumber(k)) snprintf(to, sizeof(to), "%g", k->valuedouble); else if (cJSON_IsBool(k)) snprintf(to, sizeof(to), "%s", cJSON_IsTrue(k) ? "true" : "false"); else snprintf(to, sizeof(to), "(set)");
            dadd(diff, path, "", to);
        }
        cJSON_AddItemToArray(warn, cJSON_CreateString("The hub checks the provider settings when it saves them; a bad field is refused at apply_config with its name, and nothing is changed. Prices fetched later replace the zone prices for the periods they cover."));
    }
    const cJSON *te = cJSON_GetObjectItem(patch, "telemetry");
    if (te) {
        const cJSON *k;
        cJSON_ArrayForEach(k, te) if (strcmp(k->string, "profile") && strcmp(k->string, "frame")) { fail(cx, "invalid_args", "telemetry: {profile: lean|full, frame: 1|2}", NULL); return false; }
        const char *pf = cJSON_GetStringValue(cJSON_GetObjectItem(te, "profile"));
        const cJSON *fr = cJSON_GetObjectItem(te, "frame");
        if (cJSON_GetObjectItem(te, "profile") && (!pf || (strcmp(pf, "lean") && strcmp(pf, "full")))) { fail(cx, "invalid_args", "telemetry.profile: lean or full", NULL); return false; }
        if (fr && (!cJSON_IsNumber(fr) || (fr->valueint != 1 && fr->valueint != 2))) { fail(cx, "invalid_args", "telemetry.frame: 1 or 2", NULL); return false; }
        cJSON *nt2 = NULL;
        const char *wp = rbgrid_telemetry_get_lean() ? "lean" : "full";
        if (pf && strcmp(pf, wp)) { nt2 = cJSON_AddObjectToObject(norm, "telemetry"); cJSON_AddStringToObject(nt2, "profile", pf); dadd(diff, "integrations.telemetry.profile", wp, pf); }
        int wf = rbgrid_telemetry_get_frame();
        if (fr && fr->valueint != wf) {
            if (!nt2) nt2 = cJSON_AddObjectToObject(norm, "telemetry");
            cJSON_AddNumberToObject(nt2, "frame", fr->valueint);
            char a[4], b[4]; snprintf(a, sizeof(a), "%d", wf); snprintf(b, sizeof(b), "%d", fr->valueint);
            dadd(diff, "integrations.telemetry.frame", a, b);
            cJSON_AddItemToArray(warn, cJSON_CreateString("The frame version changes the keys in the hub's MQTT telemetry; the hub re-announces its Home Assistant entities to follow. Other MQTT readers must use the new keys."));
        }
    }
    const cJSON *mq = cJSON_GetObjectItem(patch, "mqtt");
    if (mq) {
        const cJSON *k;
        cJSON_ArrayForEach(k, mq) if (strcmp(k->string, "broker_uri")) { fail(cx, "invalid_args", "mqtt: {broker_uri}", NULL); return false; }
        const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(mq, "broker_uri"));
        if (!mqtt_uri_ok(u)) { fail(cx, "invalid_args", "mqtt.broker_uri: mqtt://host:port or mqtts://user:password@host:port (< 160 chars)", NULL); return false; }
        char was[160]; mqtt_saved_uri(was, sizeof(was));
        if (strcmp(was, u)) {
            cJSON_AddStringToObject(cJSON_AddObjectToObject(norm, "mqtt"), "broker_uri", u);
            char a[176], b[176]; mask_uri(was, a, sizeof(a)); mask_uri(u, b, sizeof(b));
            dadd(diff, "mqtt.broker_uri", a, b);
            cJSON_AddItemToArray(warn, cJSON_CreateString("The hub connects to the new broker after a restart (restart_hub). Home Assistant finds the hub on that broker by MQTT discovery."));
        }
    }
    return true;
}
static bool patch_restarts_hub(const cJSON *norm)
{
    const cJSON *rg = cJSON_GetObjectItem(norm, "region");
    return cJSON_GetObjectItem(norm, "mqtt") != NULL || cJSON_GetObjectItem(rg, "nominal_voltage") || cJSON_GetObjectItem(rg, "nominal_frequency");
}

/* Sections that the hub validates only when it saves them go FIRST, so a refusal leaves everything unchanged. */
static bool apply_more_first(const cJSON *norm, char *m, size_t n)
{
    const cJSON *pr = cJSON_GetObjectItem(norm, "price_rules"), *ps = cJSON_GetObjectItem(norm, "price_signal");
    if (pr || ps) {
        cJSON *d = cJSON_CreateObject();
        if (pr) cJSON_AddItemToObject(d, "rules", cJSON_Duplicate(pr, true));
        if (ps) cJSON_AddItemToObject(d, "signal", cJSON_Duplicate(ps, true));
        char ef[24] = "", em[128] = "";
        esp_err_t e = rbact_set(d, ef, sizeof(ef), em, sizeof(em));
        cJSON_Delete(d);
        if (e != ESP_OK) { snprintf(m, n, "price rules: the hub refused %s (%s)", ef[0] ? ef : "them", em); return false; }
    }
    const cJSON *dp = cJSON_GetObjectItem(norm, "dynamic_price");
    if (dp) {
        char ef[24] = "", em[128] = "";
        if (rbdynprice_set_config(dp, ef, sizeof(ef), em, sizeof(em)) != ESP_OK) { snprintf(m, n, "dynamic_price.%s: %s", ef[0] ? ef : "?", em); return false; }
    }
    return true;
}
static bool apply_more_last(const cJSON *norm, char *m, size_t n)
{
    const char *tz = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(norm, "region"), "timezone"));
    if (tz) {
        cJSON *d = cJSON_CreateObject(); cJSON_AddStringToObject(d, "timezone", tz);
        cJSON *ack = core_cmd("set", "time", d);
        cJSON_Delete(d);
        bool ok = core_ok(ack, m, n);
        cJSON_Delete(ack);
        if (!ok) return false;
    }
    const cJSON *hb = cJSON_GetObjectItem(norm, "hub");
    if (hb) {
        rbpower_extended_config_t *lc = calloc(1, sizeof(*lc));
        if (!lc) { snprintf(m, n, "hub: out of memory"); return false; }
        (void)rbgrid_storage_load_system(lc);
        cJSON *sys = cJSON_Duplicate(hb, true);
        const cJSON *nm = cJSON_GetObjectItem(sys, "name");
        if (nm) cJSON_AddStringToObject(sys, "home_name", nm->valuestring);
        cJSON_DeleteItemFromObject(sys, "name");
        const char *gsv = cJSON_GetStringValue(cJSON_GetObjectItem(sys, "grid_system"));
        if (gsv) { cJSON_AddStringToObject(cJSON_AddObjectToObject(sys, "grid"), "system", gsv); cJSON_DeleteItemFromObject(sys, "grid_system"); }
        rbgrid_json_err_t pe = rbgrid_json_parse_system_section(sys, lc);
        cJSON_Delete(sys);
        esp_err_t se = pe == RBGRID_JSON_OK ? rbgrid_storage_save_system(lc) : ESP_FAIL;
        rbpower_extended_config_t *ec = rbgrid_core_ext_cfg();
        if (se == ESP_OK && ec) {   /* labels apply live, as on the hub page */
            if (lc->home_name) { char *c = strdup(lc->home_name); if (c) ec->home_name = c; }
            if (lc->location)  { char *c = strdup(lc->location);  if (c) ec->location = c; }
            ec->grid_system = lc->grid_system;
        }
        rbgrid_json_free_config_strings(lc);
        free(lc);
        if (se != ESP_OK) { snprintf(m, n, "hub: not saved (%s)", pe != RBGRID_JSON_OK ? "parse" : esp_err_to_name(se)); return false; }
    }
    const cJSON *rgn = cJSON_GetObjectItem(norm, "region");
    const cJSON *nv = cJSON_GetObjectItem(rgn, "nominal_voltage"), *nf = cJSON_GetObjectItem(rgn, "nominal_frequency");
    if (nv || nf) {   /* the hardware section, as the hub page's hardware-config save: stored, full effect after a restart */
        rbpower_hardware_config_t *hc = calloc(1, sizeof(*hc));
        if (!hc) { snprintf(m, n, "region: out of memory"); return false; }
        rbpower_extended_config_t *ec = rbgrid_core_ext_cfg();
        if (rbgrid_storage_load_hardware(hc) != ESP_OK && ec && ec->base_hardware_config) memcpy(hc, ec->base_hardware_config, sizeof(*hc));
        if (nv) hc->network.nominal_voltage = (float)nv->valuedouble;
        if (nf) hc->network.nominal_frequency = (uint16_t)nf->valueint;
        esp_err_t he = rbgrid_storage_save_hardware(hc);
        free(hc);
        if (he != ESP_OK) { snprintf(m, n, "region: the mains values were not saved (%s)", esp_err_to_name(he)); return false; }
        if (nv && ec && ec->base_hardware_config) ec->base_hardware_config->network.nominal_voltage = (float)nv->valuedouble;   /* current-only nodes use it at once */
    }
    const cJSON *tel = cJSON_GetObjectItem(norm, "telemetry");
    if (tel) {   /* as the hub page's telemetry-config save: applied at once and kept */
        const char *pf = cJSON_GetStringValue(cJSON_GetObjectItem(tel, "profile"));
        const cJSON *fr = cJSON_GetObjectItem(tel, "frame");
        if (pf) { bool lean = !strcmp(pf, "lean"); rbgrid_telemetry_set_lean(lean); littlefs_raw_write("/tele_profile", pf, 5); }
        if (fr) { rbgrid_telemetry_set_frame((uint8_t)fr->valueint); littlefs_raw_write("/tele_frame", fr->valueint == 2 ? "2" : "1", 1); rbgrid_ha_discovery_request(); }
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(norm, "peaks_reset"))) {
        cJSON *d = cJSON_CreateObject(); cJSON_AddTrueToObject(d, "reset");
        char em[64] = "";
        esp_err_t e = rbpeak_set(d, em, sizeof(em));
        cJSON_Delete(d);
        if (e != ESP_OK) { snprintf(m, n, "peaks reset: %s", em); return false; }
    }
    const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(norm, "mqtt"), "broker_uri"));
    if (u && littlefs_raw_write("/mqtt_broker", u, strlen(u) + 1) != ESP_OK) { snprintf(m, n, "mqtt: the broker was not saved"); return false; }
    return true;
}

/* Validate a channels patch against the current state; fill diff/warnings; return a normalized patch or NULL (fail set). */
static cJSON *check_patch(rbllm_cx_t *cx, const cJSON *patch, const cJSON *cur, cJSON *diff, cJSON *warn)
{
    const cJSON *pch = cJSON_GetObjectItem(patch, "channels"), *pcd = cJSON_GetObjectItem(patch, "peaks_cycle_day"),
                *ptf = cJSON_GetObjectItem(patch, "tariff");
    {   /* known top-level keys only, at least one */
        const cJSON *k; bool any = false;
        cJSON_ArrayForEach(k, patch) {
            if (!patch_key_ok(k->string)) {
                char mm[200]; snprintf(mm, sizeof(mm), "patch.%.24s: not a setting (channels, tariff, peaks_cycle_day, region, hub, price_rules, price_signal, peaks_reset, dynamic_price, mqtt, telemetry)", k->string);
                return fail(cx, "invalid_args", mm, "get_config_schema lists the writable fields per section.");
            }
            any = true;
        }
        if (!any) return fail(cx, "invalid_args", "patch: an object with one or more sections", "get_config_schema lists the writable fields per section.");
    }
    if (pch && !cJSON_IsObject(pch)) return fail(cx, "invalid_args", "patch.channels must be an object keyed by channel id", NULL);
    cJSON *norm = cJSON_CreateObject();
    cJSON *nch = cJSON_AddObjectToObject(norm, "channels");
    const cJSON *cch = cJSON_GetObjectItem(cur, "channels");
    const cJSON *e;
    char m[160];
    cJSON_ArrayForEach(e, pch) {
        char id[5]; strlcpy(id, e->string ? e->string : "", sizeof(id));
        for (char *q = id; *q; q++) if (*q >= 'a' && *q <= 'f') *q -= 32;
        const cJSON *c = cJSON_GetObjectItem(cch, id);
        if (!c) { snprintf(m, sizeof(m), "patch.channels: unknown channel '%.8s'", e->string ? e->string : ""); cJSON_Delete(norm); return fail(cx, "not_found", m, "Call list_channels for the ids."); }
        if (!cJSON_IsObject(e)) { cJSON_Delete(norm); return fail(cx, "invalid_args", "each channel patch is an object {name?, role?, inside?}", NULL); }
        cJSON *o = NULL;
        const cJSON *f;
        cJSON_ArrayForEach(f, e) {
            const char *k = f->string;
            const char *was = cJSON_GetStringValue(cJSON_GetObjectItem(c, k));
            if (!strcmp(k, "name")) {
                if (!valid_name(cJSON_GetStringValue(f))) { snprintf(m, sizeof(m), "channels.%s.name: 1..31 bytes of UTF-8 text, no control characters", id); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
            } else if (!strcmp(k, "role")) {
                const char *nr = cJSON_GetStringValue(f);
                int r = role_from_name(nr);
                if (r < 0) { snprintf(m, sizeof(m), "channels.%s.role: unknown role", id); cJSON_Delete(norm); return fail(cx, "invalid_args", m, "get_config_schema {section: channels} lists the roles (main_supply = the grid feed)."); }
                int cr = role_from_name(was);
                bool cs = cr == RBPOWER_ROLE_MAIN_SUPPLY || cr == RBPOWER_ROLE_ALTERNATIVE_SUPPLY, ns = r == RBPOWER_ROLE_MAIN_SUPPLY || r == RBPOWER_ROLE_ALTERNATIVE_SUPPLY;
                if ((cs || ns) && r != cr) {   /* supply points are topology: the hub refuses a role patch to/from them (409) */
                    snprintf(m, sizeof(m), "channels.%s.role: the supply role stays with the supply point", id); cJSON_Delete(norm);
                    return fail(cx, "invalid_args", m, "Put the main_supply point on the input with the grid clamp: move_point.");
                }
            } else if (!strcmp(k, "inside")) {
                const char *iv = cJSON_GetStringValue(f);
                if (!iv) { cJSON_Delete(norm); return fail(cx, "invalid_args", "inside: a channel id, or \"\" for top level", NULL); }
                if (iv[0]) {
                    char iid[5]; strlcpy(iid, iv, sizeof(iid)); for (char *q = iid; *q; q++) if (*q >= 'a' && *q <= 'f') *q -= 32;
                    if (!cJSON_GetObjectItem(cch, iid) || !strcmp(iid, id)) { snprintf(m, sizeof(m), "channels.%s.inside: another existing channel id", id); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
                }
            } else if (!strcmp(k, "ct_model")) {
                const char *nv = cJSON_GetStringValue(f);
                ct_info_t ci[RBLLM_MAX_POINTS]; int nci = ct_infos(ci, RBLLM_MAX_POINTS); bool settable = false;
                for (int q = 0; q < nci; q++) if (!strcmp(ci[q].id, id)) settable = ci[q].settable;
                if (!settable) { snprintf(m, sizeof(m), "channels.%s.ct_model: this channel's module has a fixed clamp (or none)", id); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
                cJSON *cm = ct_models(); const cJSON *x; bool known = false;
                cJSON_ArrayForEach(x, cm) { char sh[24]; ct_short(x->valuestring, sh); if (nv && !strcmp(sh, nv)) known = true; }
                cJSON_Delete(cm);
                if (!known) { snprintf(m, sizeof(m), "channels.%s.ct_model: one of the models in get_config_schema (e.g. SCT-013-030)", id); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
            } else if (!strcmp(k, "short_name")) {
                const char *sv = cJSON_GetStringValue(f);
                if (!sv || strlen(sv) > 24) { snprintf(m, sizeof(m), "channels.%s.short_name: text, up to 24 bytes", id); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
            } else if (!strcmp(k, "phase") || !strcmp(k, "rated_power_w") || !strcmp(k, "max_current_a") || !strcmp(k, "charge_threshold_w") || !strcmp(k, "idle_timeout_min")) {
                double lo = 0, hi = !strcmp(k, "phase") ? 7 : !strcmp(k, "max_current_a") ? 1000 : !strcmp(k, "idle_timeout_min") ? 1440 : !strcmp(k, "charge_threshold_w") ? 100000 : 1000000;
                bool integer = !strcmp(k, "phase") || !strcmp(k, "idle_timeout_min") || !strcmp(k, "charge_threshold_w");
                if (!cJSON_IsNumber(f) || f->valuedouble < lo || f->valuedouble > hi || (integer && f->valuedouble != (int)f->valuedouble)) {
                    snprintf(m, sizeof(m), "channels.%s.%s: %s %g..%g", id, k, integer ? "integer" : "number", lo, hi); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL);
                }
                const cJSON *wn = cJSON_GetObjectItem(c, k);
                if (cJSON_IsNumber(wn) && fabs(wn->valuedouble - f->valuedouble) < 1e-6) continue;   /* no change */
                if (!o) o = cJSON_AddObjectToObject(nch, id);
                cJSON_AddNumberToObject(o, k, f->valuedouble);
                cJSON *d = cJSON_CreateObject();
                char path[48]; snprintf(path, sizeof(path), "channels.%s.%s", id, k);
                cJSON_AddStringToObject(d, "path", path);
                if (cJSON_IsNumber(wn)) cJSON_AddNumberToObject(d, "from", wn->valuedouble); else cJSON_AddNullToObject(d, "from");
                cJSON_AddNumberToObject(d, "to", f->valuedouble);
                cJSON_AddItemToArray(diff, d);
                continue;
            } else { snprintf(m, sizeof(m), "channels.%s.%.20s: not writable", id, k); cJSON_Delete(norm); return fail(cx, "invalid_args", m, "get_config_schema {section: channels} lists the fields."); }
            const char *nv = cJSON_GetStringValue(f);
            if (was && nv && !strcmp(was, nv)) continue;   /* no change */
            if (!o) o = cJSON_AddObjectToObject(nch, id);
            cJSON_AddStringToObject(o, k, nv);
            cJSON *d = cJSON_CreateObject();
            char path[48]; snprintf(path, sizeof(path), "channels.%s.%s", id, k);
            cJSON_AddStringToObject(d, "path", path);
            cJSON_AddStringToObject(d, "from", was ? was : "");
            cJSON_AddStringToObject(d, "to", nv);
            cJSON_AddItemToArray(diff, d);
            if (!strcmp(k, "role")) cJSON_AddItemToArray(warn, cJSON_CreateString("A role change regroups the point in the app and HA; its energy history stays."));
            if (!strcmp(k, "name")) cJSON_AddItemToArray(warn, cJSON_CreateString("A rename can create new entities in Home Assistant if it is connected."));
            if (!strcmp(k, "ct_model")) cJSON_AddItemToArray(warn, cJSON_CreateString("Pick the model printed on the clamp. The module restarts (a few seconds); the 15-min period in progress is booked from the integrated power, and readings change by the ratio of the ratings."));
        }
    }
    if (pcd) {
        if (!cJSON_IsNumber(pcd) || pcd->valuedouble < 1 || pcd->valuedouble > 28 || pcd->valuedouble != (int)pcd->valuedouble) { cJSON_Delete(norm); return fail(cx, "invalid_args", "peaks_cycle_day: integer 1..28", NULL); }
        double was = cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "peaks_cycle_day"));
        if ((int)was != (int)pcd->valuedouble) {
            cJSON_AddNumberToObject(norm, "peaks_cycle_day", pcd->valuedouble);
            cJSON *d = cJSON_CreateObject();
            cJSON_AddStringToObject(d, "path", "notifications.peaks_cycle_day");
            cJSON_AddNumberToObject(d, "from", was); cJSON_AddNumberToObject(d, "to", pcd->valuedouble);
            cJSON_AddItemToArray(diff, d);
            cJSON_AddItemToArray(warn, cJSON_CreateString("A new cycle day starts a new peak cycle; the current figures move to the previous cycle."));
        }
    }
    if (ptf) {
        if (!cJSON_IsObject(ptf)) { cJSON_Delete(norm); return fail(cx, "invalid_args", "patch.tariff must be an object", NULL); }
        const cJSON *ct = cJSON_GetObjectItem(cur, "tariff");
        cJSON *nt = cJSON_CreateObject();
        const cJSON *f;
        char path[64];
        static const struct { const char *k; double lo, hi; } NUMS[] = {
            { "vat_percent", 0, 50 }, { "energy_tax_per_kwh", 0, 10 }, { "grid_fee_per_kwh", 0, 10 }, { "levy_per_kwh", 0, 10 } };
        cJSON_ArrayForEach(f, ptf) {
            const char *k = f->string;
            if (!strcmp(k, "plan_name") || !strcmp(k, "provider")) {
                if (!valid_name(cJSON_GetStringValue(f))) { snprintf(m, sizeof(m), "tariff.%s: 1..31 bytes of UTF-8 text", k); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
                const char *was = cJSON_GetStringValue(cJSON_GetObjectItem(ct, k));
                if (was && !strcmp(was, f->valuestring)) continue;
                cJSON_AddStringToObject(nt, k, f->valuestring);
                cJSON *d = cJSON_CreateObject(); snprintf(path, sizeof(path), "tariff.%s", k);
                cJSON_AddStringToObject(d, "path", path); cJSON_AddStringToObject(d, "from", was ? was : ""); cJSON_AddStringToObject(d, "to", f->valuestring);
                cJSON_AddItemToArray(diff, d);
            } else if (!strcmp(k, "currency")) {
                const char *v = cJSON_GetStringValue(f);
                bool ok3 = v && strlen(v) == 3 && v[0] >= 'A' && v[0] <= 'Z' && v[1] >= 'A' && v[1] <= 'Z' && v[2] >= 'A' && v[2] <= 'Z';
                if (!ok3) { cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", "tariff.currency: ISO 4217 code, e.g. EUR", NULL); }
                const char *was = cJSON_GetStringValue(cJSON_GetObjectItem(ct, k));
                if (was && !strcmp(was, v)) continue;
                cJSON_AddStringToObject(nt, k, v);
                cJSON *d = cJSON_CreateObject();
                cJSON_AddStringToObject(d, "path", "tariff.currency"); cJSON_AddStringToObject(d, "from", was ? was : ""); cJSON_AddStringToObject(d, "to", v);
                cJSON_AddItemToArray(diff, d);
                cJSON_AddItemToArray(warn, cJSON_CreateString("Money already booked keeps its old currency figures; only new costs use the new currency label."));
            } else if (!strcmp(k, "zones")) {
                const cJSON *z;
                cJSON *nz = cJSON_AddObjectToObject(nt, "zones");
                const rbpower_tariff_plan_t *tpl = rbpower_tariff_get_current_plan();
                cJSON_ArrayForEach(z, f) {
                    int zi = zone_idx(z->string);
                    if (zi < 0) { snprintf(m, sizeof(m), "tariff.zones.%.20s: zone types are night, morning_peak, day, evening_peak, late_evening, weekend, holiday, critical_peak", z->string); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
                    if (!cJSON_IsObject(z)) { cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", "each tariff zone is an object", NULL); }
                    const cJSON *cz = cJSON_GetObjectItem(cJSON_GetObjectItem(ct, "zones"), z->string);
                    const rbpower_tariff_zone_t *pz = plan_zone(tpl, zi);
                    bool isnew = !cz;
                    cJSON *oz = NULL;
                    if (isnew) {
                        if (cJSON_IsFalse(cJSON_GetObjectItem(z, "enabled"))) continue;   /* removing a zone the plan does not have */
                        if (!cJSON_IsNumber(cJSON_GetObjectItem(z, "buy_per_kwh")) || (!cJSON_GetObjectItem(z, "window") && !cJSON_GetObjectItem(z, "days"))) {
                            snprintf(m, sizeof(m), "tariff.zones.%s: a new zone needs buy_per_kwh and a window and/or days", z->string); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL);
                        }
                        oz = cJSON_AddObjectToObject(nz, z->string);
                        cJSON_AddTrueToObject(oz, "new");
                        cJSON *d = cJSON_CreateObject(); snprintf(path, sizeof(path), "tariff.zones.%s", z->string);
                        cJSON_AddStringToObject(d, "path", path); cJSON_AddStringToObject(d, "from", ""); cJSON_AddStringToObject(d, "to", "added");
                        cJSON_AddItemToArray(diff, d);
                        cJSON_AddItemToArray(warn, cJSON_CreateString("A new zone counts from the next 15-min period. Where zones overlap, the zone with the higher priority wins; hours no zone covers count at the default zone."));
                    }
                    const cJSON *r;
                    cJSON_ArrayForEach(r, z) {
                        const char *rk = r->string;
                        if (!strcmp(rk, "enabled")) {
                            if (!cJSON_IsBool(r)) { cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", "tariff zone enabled: true or false (false removes the zone)", NULL); }
                            if (isnew || cJSON_IsTrue(r)) continue;
                            {   /* the plan's default zone: a new default must come in the same patch, or hours no zone covers point at a removed zone */
                                const rbpower_tariff_plan_t *tpd = rbpower_tariff_get_current_plan();
                                const char *ndz = cJSON_GetStringValue(cJSON_GetObjectItem(ptf, "default_zone"));
                                if (tpd && (int)tpd->default_zone_id == zone_idx(z->string) && (!ndz || !strcmp(ndz, z->string))) {
                                    snprintf(m, sizeof(m), "tariff.zones.%.20s: this is the plan's default zone", z->string);
                                    cJSON_Delete(nt); cJSON_Delete(norm);
                                    return fail(cx, "invalid_args", m, "Set tariff.default_zone to another zone in the same patch.");
                                }
                            }
                            if (!oz) oz = cJSON_AddObjectToObject(nz, z->string);
                            cJSON_AddFalseToObject(oz, "enabled");
                            cJSON *d = cJSON_CreateObject(); snprintf(path, sizeof(path), "tariff.zones.%s", z->string);
                            cJSON_AddStringToObject(d, "path", path); cJSON_AddStringToObject(d, "from", "on"); cJSON_AddStringToObject(d, "to", "removed");
                            cJSON_AddItemToArray(diff, d);
                            cJSON_AddItemToArray(warn, cJSON_CreateString("A removed zone's hours count at the remaining zones (or the default zone) from the next 15-min period; hours already booked keep their zone."));
                            continue;
                        }
                        if (!strcmp(rk, "days")) {
                            int mask = parse_days(cJSON_GetStringValue(r));
                            if (mask < 0) { snprintf(m, sizeof(m), "tariff.zones.%s.days: \"mon-fri\", \"sat,sun\", \"mon,wed-fri\", \"weekdays\", \"weekend\" or \"all\"", z->string); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
                            char was[32] = "", nd[32]; if (pz) zone_days(pz, was);
                            days_str(mask, nd);
                            if (!isnew && !strcmp(was, nd)) continue;
                            if (!oz) oz = cJSON_AddObjectToObject(nz, z->string);
                            cJSON_AddStringToObject(oz, "days", nd);
                            cJSON *d = cJSON_CreateObject(); snprintf(path, sizeof(path), "tariff.zones.%s.days", z->string);
                            cJSON_AddStringToObject(d, "path", path); cJSON_AddStringToObject(d, "from", was[0] || isnew ? was : "every day"); cJSON_AddStringToObject(d, "to", nd[0] ? nd : "every day");
                            cJSON_AddItemToArray(diff, d);
                            continue;
                        }
                        if (!strcmp(rk, "priority")) {
                            if (!cJSON_IsNumber(r) || r->valuedouble < 1 || r->valuedouble > 100 || r->valuedouble != (int)r->valuedouble) { cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", "tariff zone priority: integer 1..100 (higher wins where zones overlap)", NULL); }
                            int wasp = (pz && pz->conditions_count) ? pz->conditions[0].priority : 0;
                            if (!isnew && wasp == r->valueint) continue;
                            if (!oz) oz = cJSON_AddObjectToObject(nz, z->string);
                            cJSON_AddNumberToObject(oz, "priority", r->valueint);
                            cJSON *d = cJSON_CreateObject(); snprintf(path, sizeof(path), "tariff.zones.%s.priority", z->string);
                            cJSON_AddStringToObject(d, "path", path); cJSON_AddNumberToObject(d, "from", wasp); cJSON_AddNumberToObject(d, "to", r->valueint);
                            cJSON_AddItemToArray(diff, d);
                            continue;
                        }
                        if (!strcmp(rk, "window")) {
                            int sh, sm, eh, em;
                            const char *was = cJSON_GetStringValue(cJSON_GetObjectItem(cz, "window"));
                            if (!was && !isnew && zone_tr_count(pz) > 1) { snprintf(m, sizeof(m), "tariff.zones.%.20s.window: this zone has several time windows", z->string); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, "Remove the zone (enabled:false) and add it again with one window, or keep it."); }
                            if (!cJSON_IsString(r) || !parse_window(r->valuestring, &sh, &sm, &eh, &em)) { snprintf(m, sizeof(m), "tariff.zones.%.20s.window: \"HH:MM-HH:MM\", minutes 00/15/30/45, from != to", z->string); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, "Example \"22:00-06:00\" (wraps midnight); 24:00 is allowed as an end."); }
                            char nw[12]; snprintf(nw, sizeof(nw), "%02d:%02d-%02d:%02d", sh, sm, eh, em);
                            if (was && !strcmp(nw, was)) continue;
                            if (!oz) oz = cJSON_AddObjectToObject(nz, z->string);
                            cJSON_AddStringToObject(oz, "window", nw);
                            cJSON *d = cJSON_CreateObject(); snprintf(path, sizeof(path), "tariff.zones.%s.window", z->string);
                            cJSON_AddStringToObject(d, "path", path); cJSON_AddStringToObject(d, "from", was ? was : (isnew ? "" : "all day")); cJSON_AddStringToObject(d, "to", nw);
                            cJSON_AddItemToArray(diff, d);
                            cJSON_AddItemToArray(warn, cJSON_CreateString("A new zone window applies from the next 15-min period; hours already booked keep their zone. Hours no zone covers fall to the plan's default zone; overlaps go to the zone with the higher priority."));
                            continue;
                        }
                        if (strcmp(rk, "buy_per_kwh") && strcmp(rk, "sell_per_kwh")) { snprintf(m, sizeof(m), "tariff.zones.%s.%.20s: buy_per_kwh, sell_per_kwh, window, days, priority or enabled", z->string, rk); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
                        if (!cJSON_IsNumber(r) || r->valuedouble < 0 || r->valuedouble > 100) { snprintf(m, sizeof(m), "tariff.zones.%s.%s: 0..100 per kWh (before fees and VAT)", z->string, rk); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, "A price per MWh must be divided by 1000."); }
                        double was = cz ? cJSON_GetNumberValue(cJSON_GetObjectItem(cz, rk)) : 0;
                        if (!isnew && fabs(was - r->valuedouble) < 1e-6) continue;
                        if (!oz) oz = cJSON_AddObjectToObject(nz, z->string);
                        cJSON_AddNumberToObject(oz, rk, r->valuedouble);
                        cJSON *d = cJSON_CreateObject(); snprintf(path, sizeof(path), "tariff.zones.%s.%s", z->string, rk);
                        cJSON_AddStringToObject(d, "path", path); cJSON_AddNumberToObject(d, "from", was); cJSON_AddNumberToObject(d, "to", r->valuedouble);
                        cJSON_AddItemToArray(diff, d);
                    }
                }
            } else if (!strcmp(k, "default_zone")) {
                const char *dz = cJSON_GetStringValue(f);
                int zi = zone_idx(dz);
                const rbpower_tariff_plan_t *tpl = rbpower_tariff_get_current_plan();
                const cJSON *pzs = cJSON_GetObjectItem(ptf, "zones");
                bool there = zi >= 0 && (plan_zone(tpl, zi) || (cJSON_GetObjectItem(pzs, dz) && !cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetObjectItem(pzs, dz), "enabled"))));
                if (!there) { cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", "tariff.default_zone: one of the plan's zones (get_config tariff), or a zone added in the same patch", NULL); }
                int wz = tpl ? (int)tpl->default_zone_id : -1;
                if (wz == zi) continue;
                cJSON_AddStringToObject(nt, "default_zone", dz);
                cJSON *d = cJSON_CreateObject();
                cJSON_AddStringToObject(d, "path", "tariff.default_zone"); cJSON_AddStringToObject(d, "from", wz >= 0 && wz < RBPOWER_TARIFF_ZONE_COUNT ? ZONE_N[wz] : ""); cJSON_AddStringToObject(d, "to", dz);
                cJSON_AddItemToArray(diff, d);
                cJSON_AddItemToArray(warn, cJSON_CreateString("Hours that no zone covers count at the default zone."));
            } else {
                size_t q; bool hit = false;
                for (q = 0; q < sizeof(NUMS) / sizeof(NUMS[0]); q++) if (!strcmp(k, NUMS[q].k)) { hit = true; break; }
                if (!hit) { snprintf(m, sizeof(m), "tariff.%.24s: not writable", k); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, "get_config_schema {section: tariff} lists the fields."); }
                if (!cJSON_IsNumber(f) || f->valuedouble < NUMS[q].lo || f->valuedouble > NUMS[q].hi) { snprintf(m, sizeof(m), "tariff.%s: %g..%g", k, NUMS[q].lo, NUMS[q].hi); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
                double was = cJSON_GetNumberValue(cJSON_GetObjectItem(ct, k));
                if (fabs(was - f->valuedouble) < 1e-6) continue;
                cJSON_AddNumberToObject(nt, k, f->valuedouble);
                cJSON *d = cJSON_CreateObject(); snprintf(path, sizeof(path), "tariff.%s", k);
                cJSON_AddStringToObject(d, "path", path); cJSON_AddNumberToObject(d, "from", was); cJSON_AddNumberToObject(d, "to", f->valuedouble);
                cJSON_AddItemToArray(diff, d);
            }
        }
        cJSON *zz = cJSON_GetObjectItem(nt, "zones");
        if (zz && !cJSON_GetArraySize(zz)) cJSON_DeleteItemFromObject(nt, "zones");
        if (cJSON_GetArraySize(nt)) {
            cJSON_AddItemToObject(norm, "tariff", nt);
            cJSON_AddItemToArray(warn, cJSON_CreateString("New prices apply from now on; costs already booked stay as they were."));
            cJSON_AddItemToArray(warn, cJSON_CreateString("Zone prices are before fees and VAT; the hub adds the per-kWh fees and VAT on top."));
            const char *pn = cJSON_GetStringValue(cJSON_GetObjectItem(ct, "plan_name")), *pv = cJSON_GetStringValue(cJSON_GetObjectItem(ct, "provider"));
            if (rbpower_tariff_plan_is_placeholder(pn ? pn : "", pv ? pv : "") && !cJSON_GetObjectItem(nt, "plan_name") && !cJSON_GetObjectItem(nt, "provider"))
                cJSON_AddItemToArray(warn, cJSON_CreateString("The hub still runs its demo plan: also set plan_name and provider (the user's supplier) so money stops counting as demo figures."));
        } else cJSON_Delete(nt);
    }
    if (!check_more(cx, patch, norm, diff, warn)) { cJSON_Delete(norm); return NULL; }
    /* an empty diff is an answer, not an error: "is my price right?" -> the hub already has exactly these values */
    return norm;
}

/* Write the tariff part of a normalized patch into the hub's plan document, then save + apply it (the HTTP path). */
static bool apply_tariff(rbllm_cx_t *cx, const cJSON *nt, char *m, size_t mlen)
{
    char *doc = rbgrid_core_json_ctx() ? rbgrid_json_create_rbpower_tariff_plan_config(rbgrid_core_json_ctx()) : NULL;
    cJSON *d = doc ? cJSON_Parse(doc) : NULL;
    free(doc);
    cJSON *plan = d ? cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(d, "data"), "list"), 0) : NULL;
    if (!plan) { cJSON_Delete(d); snprintf(m, mlen, "tariff: the plan document could not be read"); return false; }
    static const struct { const char *patch, *doc; } MAP[] = {
        { "plan_name", "plan_name" }, { "provider", "provider_name" }, { "currency", "currency_code" }, { "vat_percent", "vat_percent" },
        { "energy_tax_per_kwh", "energy_tax_per_kwh" }, { "grid_fee_per_kwh", "grid_usage_fee_per_kwh" }, { "levy_per_kwh", "renewable_levy_per_kwh" } };
    for (size_t q = 0; q < sizeof(MAP) / sizeof(MAP[0]); q++) {
        const cJSON *v = cJSON_GetObjectItem(nt, MAP[q].patch);
        if (!v) continue;
        cJSON_ReplaceItemInObject(plan, MAP[q].doc, cJSON_Duplicate(v, false));
    }
    if (cJSON_GetObjectItem(nt, "plan_name") || cJSON_GetObjectItem(nt, "provider")) cJSON_ReplaceItemInObject(plan, "is_placeholder", cJSON_CreateFalse());
    /* the demo plan built at boot before the clock synced carries 1970 validity stamps (metadata only, never read by the
     * engine): don't persist them */
    const cJSON *vf = cJSON_GetObjectItem(plan, "valid_from_timestamp");
    if (vf && vf->valuedouble < RBGRID_TS_SANE_MIN && now_s() > RBGRID_TS_SANE_MIN) {
        cJSON_ReplaceItemInObject(plan, "valid_from_timestamp", cJSON_CreateNumber(now_s()));
        cJSON_ReplaceItemInObject(plan, "valid_until_timestamp", cJSON_CreateNumber((double)now_s() + 365.0 * 86400.0));
    }
    const cJSON *z;
    cJSON *dzs = cJSON_GetObjectItem(plan, "zones");
    if (!dzs) dzs = cJSON_AddArrayToObject(plan, "zones");
    cJSON_ArrayForEach(z, cJSON_GetObjectItem(nt, "zones")) {
        char up[24]; strlcpy(up, z->string, sizeof(up)); for (char *q = up; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
        cJSON *dz = NULL, *e;
        int di = 0, at = -1;
        cJSON_ArrayForEach(e, dzs) { const char *zid = cJSON_GetStringValue(cJSON_GetObjectItem(e, "zone_id")); if (zid && !strcmp(zid, up)) { dz = e; at = di; } di++; }
        if (cJSON_IsFalse(cJSON_GetObjectItem(z, "enabled"))) {   /* remove the zone */
            if (at >= 0) cJSON_DeleteItemFromArray(dzs, at);
            continue;
        }
        if (!dz && cJSON_IsTrue(cJSON_GetObjectItem(z, "new"))) {
            if (cJSON_GetArraySize(dzs) >= RBPOWER_TARIFF_ZONE_COUNT) { cJSON_Delete(d); snprintf(m, mlen, "tariff.zones.%s: the plan holds no more zones", z->string); return false; }
            dz = cJSON_CreateObject();
            cJSON_AddStringToObject(dz, "zone_id", up);
            char nm[24]; strlcpy(nm, z->string, sizeof(nm)); for (char *q = nm; *q; q++) if (*q == '_') *q = ' ';
            if (nm[0] >= 'a' && nm[0] <= 'z') nm[0] -= 32;
            cJSON_AddStringToObject(dz, "zone_name", nm);
            cJSON_AddStringToObject(dz, "description", "");
            cJSON_AddNumberToObject(dz, "rate_buying_per_kwh", 0);
            cJSON_AddTrueToObject(dz, "conditions_and_logic");
            cJSON_AddArrayToObject(dz, "conditions");
            cJSON_AddTrueToObject(dz, "is_enabled");
            cJSON_AddItemToArray(dzs, dz);
        }
        if (!dz) { cJSON_Delete(d); snprintf(m, mlen, "tariff.zones.%s: zone not in the plan document", z->string); return false; }
        const cJSON *b = cJSON_GetObjectItem(z, "buy_per_kwh"), *sl = cJSON_GetObjectItem(z, "sell_per_kwh");
        if (b) { cJSON_DeleteItemFromObject(dz, "rate_buying_per_kwh"); cJSON_AddNumberToObject(dz, "rate_buying_per_kwh", b->valuedouble); }
        if (sl) { cJSON_DeleteItemFromObject(dz, "rate_selling_per_kwh"); cJSON_AddNumberToObject(dz, "rate_selling_per_kwh", sl->valuedouble); }
        cJSON *conds = cJSON_GetObjectItem(dz, "conditions");
        if (!conds) conds = cJSON_AddArrayToObject(dz, "conditions");
        const cJSON *jp = cJSON_GetObjectItem(z, "priority");
        int prio = cJSON_IsNumber(jp) ? jp->valueint : -1;
        if (prio < 0) {   /* keep the zone's own priority for a condition added now */
            cJSON *c0 = cJSON_GetArrayItem(conds, 0);
            prio = c0 && cJSON_IsNumber(cJSON_GetObjectItem(c0, "priority")) ? cJSON_GetObjectItem(c0, "priority")->valueint : 10;
        }
        const char *wv = cJSON_GetStringValue(cJSON_GetObjectItem(z, "window"));
        if (wv) {
            int sh, sm, eh, em;
            cJSON *tc = NULL, *c; int ntr = 0;
            cJSON_ArrayForEach(c, conds) {
                const char *ty = cJSON_GetStringValue(cJSON_GetObjectItem(c, "condition_type"));
                if (ty && !strcmp(ty, "TIME_RANGE") && cJSON_IsTrue(cJSON_GetObjectItem(c, "is_active"))) { tc = c; ntr++; }
            }
            if (!parse_window(wv, &sh, &sm, &eh, &em) || ntr > 1) { cJSON_Delete(d); snprintf(m, mlen, "tariff.zones.%s.window: the zone has several time windows in the plan document", z->string); return false; }
            if (!tc) {
                tc = cJSON_CreateObject();
                cJSON_AddStringToObject(tc, "condition_type", "TIME_RANGE");
                cJSON_AddObjectToObject(tc, "params");
                cJSON_AddTrueToObject(tc, "is_active");
                cJSON_AddNumberToObject(tc, "priority", prio);
                cJSON_AddItemToArray(conds, tc);
            }
            cJSON *pr = cJSON_GetObjectItem(tc, "params");
            if (!pr) pr = cJSON_AddObjectToObject(tc, "params");
            const char *ks[] = { "start_hour", "start_minute", "end_hour", "end_minute" };
            int vs[] = { sh, sm, eh, em };
            for (int q = 0; q < 4; q++) { cJSON_DeleteItemFromObject(pr, ks[q]); cJSON_AddNumberToObject(pr, ks[q], vs[q]); }
            cJSON_DeleteItemFromObject(pr, "spans_midnight");
            cJSON_AddBoolToObject(pr, "spans_midnight", (sh * 60 + sm) > (eh * 60 + em));
        }
        const char *dv = cJSON_GetStringValue(cJSON_GetObjectItem(z, "days"));
        if (dv) {   /* replace the weekday conditions with one WEEKDAY mask ("" = every day = no condition) */
            for (int q = cJSON_GetArraySize(conds) - 1; q >= 0; q--) {
                const char *ty = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(conds, q), "condition_type"));
                if (ty && (!strcmp(ty, "WEEKDAY") || !strcmp(ty, "WEEKEND"))) cJSON_DeleteItemFromArray(conds, q);
            }
            int mask = parse_days(dv);
            if (mask > 0) {
                cJSON *c = cJSON_CreateObject();
                cJSON_AddStringToObject(c, "condition_type", "WEEKDAY");
                cJSON *pr = cJSON_AddObjectToObject(c, "params");
                cJSON_AddNumberToObject(pr, "weekday_mask", mask);
                cJSON_AddTrueToObject(c, "is_active");
                cJSON_AddNumberToObject(c, "priority", prio);
                cJSON_AddItemToArray(conds, c);
            }
        }
        if (cJSON_IsNumber(jp)) {
            cJSON *c;
            cJSON_ArrayForEach(c, conds) { cJSON_DeleteItemFromObject(c, "priority"); cJSON_AddNumberToObject(c, "priority", jp->valueint); }
        }
        if (cJSON_GetArraySize(conds) > 4) { cJSON_Delete(d); snprintf(m, mlen, "tariff.zones.%s: a zone holds at most 4 conditions", z->string); return false; }
    }
    const char *dfz = cJSON_GetStringValue(cJSON_GetObjectItem(nt, "default_zone"));
    if (dfz) {
        char up[24]; strlcpy(up, dfz, sizeof(up)); for (char *q = up; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
        cJSON_DeleteItemFromObject(plan, "default_zone_id"); cJSON_AddStringToObject(plan, "default_zone_id", up);
    }
    if (!cJSON_GetArraySize(dzs)) { cJSON_Delete(d); snprintf(m, mlen, "tariff: a plan keeps at least one zone"); return false; }
    cJSON_DeleteItemFromObject(plan, "active_zones_count"); cJSON_AddNumberToObject(plan, "active_zones_count", cJSON_GetArraySize(dzs));
    char *out = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    if (!out) { snprintf(m, mlen, "tariff: out of memory"); return false; }
    esp_err_t se = rbgrid_storage_save_tariffs(out);
    rbgrid_json_err_t pe = se == ESP_OK ? rbgrid_json_parse_tariff_plan_config(out) : RBGRID_JSON_OK;
    free(out);
    if (se != ESP_OK) { snprintf(m, mlen, "tariff: not saved (%s)", esp_err_to_name(se)); return false; }
    if (pe != RBGRID_JSON_OK) { snprintf(m, mlen, "tariff: saved, but the hub could not apply it (%d); it applies after a restart", (int)pe); return false; }
    (void)cx;
    return true;
}

/* A patch that sets a clamp model restarts that metering module (a few seconds without data); the hub itself never
 * restarts for any field writable in v1, so restart_required stays false. */
static bool patch_restarts_module(const cJSON *patch)
{
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(patch, "channels")) if (cJSON_GetObjectItem(c, "ct_model")) return true;
    return false;
}

/* Patch that turns `cur` into `target` (both captures). */
static cJSON *patch_between(const cJSON *cur, const cJSON *target)
{
    cJSON *p = cJSON_CreateObject();
    cJSON *ch = cJSON_AddObjectToObject(p, "channels");
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(target, "channels")) {
        const cJSON *c = cJSON_GetObjectItem(cJSON_GetObjectItem(cur, "channels"), t->string);
        if (!c) continue;   /* the point no longer exists: nothing to restore */
        cJSON *o = NULL;
        const char *ks[] = { "name", "role", "inside", "ct_model" };
        for (int k = 0; k < 4; k++) {
            const char *a = cJSON_GetStringValue(cJSON_GetObjectItem(c, ks[k])), *b = cJSON_GetStringValue(cJSON_GetObjectItem(t, ks[k]));
            if (a && b && strcmp(a, b)) { if (!o) o = cJSON_AddObjectToObject(ch, t->string); cJSON_AddStringToObject(o, ks[k], b); }
        }
        const char *sa = cJSON_GetStringValue(cJSON_GetObjectItem(c, "short_name")), *sb = cJSON_GetStringValue(cJSON_GetObjectItem(t, "short_name"));
        if (sa && sb && strcmp(sa, sb)) { if (!o) o = cJSON_AddObjectToObject(ch, t->string); cJSON_AddStringToObject(o, "short_name", sb); }
        for (int k = 0; k < 5; k++) {
            const cJSON *na = cJSON_GetObjectItem(c, CH_NUM_K[k]), *nb = cJSON_GetObjectItem(t, CH_NUM_K[k]);
            if (cJSON_IsNumber(na) && cJSON_IsNumber(nb) && fabs(na->valuedouble - nb->valuedouble) > 1e-6) {
                if (!o) o = cJSON_AddObjectToObject(ch, t->string);
                cJSON_AddNumberToObject(o, CH_NUM_K[k], nb->valuedouble);
            }
        }
    }
    double a = cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "peaks_cycle_day")), b = cJSON_GetNumberValue(cJSON_GetObjectItem(target, "peaks_cycle_day"));
    if ((int)a != (int)b && b >= 1) cJSON_AddNumberToObject(p, "peaks_cycle_day", b);
    const cJSON *tc = cJSON_GetObjectItem(cur, "tariff"), *tt = cJSON_GetObjectItem(target, "tariff");
    if (tc && tt) {
        cJSON *o = cJSON_CreateObject();
        const cJSON *f;
        cJSON_ArrayForEach(f, tt) {
            if (!strcmp(f->string, "zones")) continue;
            const cJSON *c = cJSON_GetObjectItem(tc, f->string);
            bool diff = !c || (cJSON_IsString(f) ? strcmp(f->valuestring, cJSON_GetStringValue(c) ? cJSON_GetStringValue(c) : "") != 0
                                                 : fabs(f->valuedouble - c->valuedouble) > 1e-6);
            if (diff) cJSON_AddItemToObject(o, f->string, cJSON_Duplicate(f, false));
        }
        cJSON *zo = cJSON_CreateObject();
        const cJSON *z;
        cJSON_ArrayForEach(z, cJSON_GetObjectItem(tt, "zones")) {
            const cJSON *cz = cJSON_GetObjectItem(cJSON_GetObjectItem(tc, "zones"), z->string);
            if (!cz) {   /* the zone was removed since: add it back */
                cJSON *one = cJSON_AddObjectToObject(zo, z->string);
                const char *ks[] = { "buy_per_kwh", "sell_per_kwh", "window", "days" };
                for (int q = 0; q < 4; q++) { const cJSON *v = cJSON_GetObjectItem(z, ks[q]); if (v) cJSON_AddItemToObject(one, ks[q], cJSON_Duplicate(v, false)); }
                const cJSON *dv = cJSON_GetObjectItem(one, "days");
                if (dv && !dv->valuestring[0]) cJSON_ReplaceItemInObject(one, "days", cJSON_CreateString("all"));
                continue;
            }
            cJSON *one = NULL;
            {
                const char *dx = cJSON_GetStringValue(cJSON_GetObjectItem(z, "days")), *dy = cJSON_GetStringValue(cJSON_GetObjectItem(cz, "days"));
                if (dx && dy && strcmp(dx, dy)) { one = cJSON_AddObjectToObject(zo, z->string); cJSON_AddStringToObject(one, "days", dx[0] ? dx : "all"); }
            }
            const char *ks[] = { "buy_per_kwh", "sell_per_kwh" };
            for (int q = 0; q < 2; q++) {
                double x = cJSON_GetNumberValue(cJSON_GetObjectItem(z, ks[q])), y = cJSON_GetNumberValue(cJSON_GetObjectItem(cz, ks[q]));
                if (fabs(x - y) > 1e-6) { if (!one) one = cJSON_AddObjectToObject(zo, z->string); cJSON_AddNumberToObject(one, ks[q], x); }
            }
            const char *wx = cJSON_GetStringValue(cJSON_GetObjectItem(z, "window")), *wy = cJSON_GetStringValue(cJSON_GetObjectItem(cz, "window"));
            if (wx && wy && strcmp(wx, wy)) { if (!one) one = cJSON_AddObjectToObject(zo, z->string); cJSON_AddStringToObject(one, "window", wx); }
        }
        cJSON_ArrayForEach(z, cJSON_GetObjectItem(tc, "zones"))   /* zones added since: remove them */
            if (!cJSON_GetObjectItem(cJSON_GetObjectItem(tt, "zones"), z->string)) cJSON_AddFalseToObject(cJSON_AddObjectToObject(zo, z->string), "enabled");
        if (cJSON_GetArraySize(zo)) cJSON_AddItemToObject(o, "zones", zo); else cJSON_Delete(zo);
        if (cJSON_GetArraySize(o)) cJSON_AddItemToObject(p, "tariff", o); else cJSON_Delete(o);
    }
    {   /* time zone and hub labels (versions recorded before they were versioned carry neither: nothing to restore) */
        const char *x = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(target, "region"), "timezone"));
        const char *y = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(cur, "region"), "timezone"));
        if (x && x[0] && y && strcmp(x, y)) cJSON_AddStringToObject(cJSON_AddObjectToObject(p, "region"), "timezone", x);
        cJSON *hp = NULL;
        const char *hk[] = { "name", "location" };
        for (int q = 0; q < 2; q++) {
            x = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(target, "hub"), hk[q]));
            y = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(cur, "hub"), hk[q]));
            if (!x || !y || !strcmp(x, y) || (q == 0 && !x[0])) continue;
            if (!hp) hp = cJSON_AddObjectToObject(p, "hub");
            cJSON_AddStringToObject(hp, hk[q], x);
        }
    }
    return p;
}

/* The parts of a normalised patch that no version records (a rollback cannot undo them), comma-separated; "" if none. */
static void unversioned_parts(const cJSON *patch, char *unv, size_t n)
{
    size_t uo = 0;
    unv[0] = '\0';
    const cJSON *k;
    cJSON_ArrayForEach(k, patch) {
        if (!strcmp(k->string, "channels") || !strcmp(k->string, "tariff") || !strcmp(k->string, "peaks_cycle_day")) continue;
        if (!strcmp(k->string, "region") || !strcmp(k->string, "hub")) {
            const cJSON *q;
            cJSON_ArrayForEach(q, k) {
                if (!strcmp(q->string, "timezone") || !strcmp(q->string, "name") || !strcmp(q->string, "location")) continue;
                int w = snprintf(unv + uo, n - uo, "%s%s.%s", uo ? ", " : "", k->string, q->string);
                if (w > 0 && (size_t)w < n - uo) uo += (size_t)w;
            }
            continue;
        }
        int w = snprintf(unv + uo, n - uo, "%s%s", uo ? ", " : "", k->string);
        if (w > 0 && (size_t)w < n - uo) uo += (size_t)w;
    }
}

static cJSON *t_propose_config(const cJSON *a, rbllm_cx_t *cx)
{
    const cJSON *patch = cJSON_GetObjectItem(a, "patch"), *rb = cJSON_GetObjectItem(a, "rollback_to");
    if (!patch == !rb) return fail(cx, "invalid_args", "give either patch or rollback_to", NULL);
    ver_sync("hub", NULL);
    cJSON *cur = cfg_capture();
    char base[17]; cfg_hash(cur, base);
    cJSON *own = NULL;
    char summary[120] = "";
    if (rb) {
        if (!cJSON_IsNumber(rb)) { cJSON_Delete(cur); return fail(cx, "invalid_args", "rollback_to: a version number from list_config_versions", NULL); }
        cJSON *v = ver_load();
        const cJSON *e, *hit = NULL;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(v, "versions")) if ((int)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "version")) == (int)rb->valuedouble) hit = e;
        if (!hit) { cJSON_Delete(v); cJSON_Delete(cur); return fail(cx, "not_found", "no such version (the hub keeps the last 5)", "Call list_config_versions."); }
        own = patch_between(cur, cJSON_GetObjectItem(hit, "state"));
        snprintf(summary, sizeof(summary), "rollback to version %d", (int)rb->valuedouble);
        cJSON_Delete(v);
        patch = own;
    } else if (!cJSON_IsObject(patch)) { cJSON_Delete(cur); return fail(cx, "invalid_args", "patch must be an object", NULL); }
    cJSON *diff = cJSON_CreateArray(), *warn = cJSON_CreateArray();
    {   /* tariff.prices_from_bill: the zone buy prices are the bill's per-kWh price (VAT and per-kWh fees included);
         * the plan stores the energy price, so turn each one back: net = gross / (1 + VAT/100) - fees */
        const cJSON *ptf = cJSON_GetObjectItem(patch, "tariff"), *pfb = cJSON_GetObjectItem(ptf, "prices_from_bill");
        if (pfb) {
            if (!cJSON_IsBool(pfb)) { cJSON_Delete(diff); cJSON_Delete(warn); cJSON_Delete(own); cJSON_Delete(cur); return fail(cx, "invalid_args", "tariff.prices_from_bill: true or false", NULL); }
            cJSON *cp = cJSON_Duplicate(patch, true);
            cJSON *t2 = cJSON_GetObjectItem(cp, "tariff");
            cJSON_DeleteItemFromObject(t2, "prices_from_bill");
            if (cJSON_IsTrue(pfb)) {
                const rbpower_tariff_plan_t *tp = rbpower_tariff_get_current_plan();
                #define PFB_NUM(key, dflt) (cJSON_IsNumber(cJSON_GetObjectItem(t2, key)) ? cJSON_GetObjectItem(t2, key)->valuedouble : (double)(dflt))
                double vat = PFB_NUM("vat_percent", tp ? tp->vat_percent : 0);
                double fees = PFB_NUM("energy_tax_per_kwh", tp ? tp->energy_tax_per_kwh : 0) + PFB_NUM("grid_fee_per_kwh", tp ? tp->grid_usage_fee_per_kwh : 0) +
                              PFB_NUM("levy_per_kwh", tp ? tp->renewable_levy_per_kwh : 0);
                #undef PFB_NUM
                cJSON *z; int nconv = 0;
                cJSON_ArrayForEach(z, cJSON_GetObjectItem(t2, "zones")) {
                    cJSON *b = cJSON_GetObjectItem(z, "buy_per_kwh");
                    if (!cJSON_IsNumber(b)) continue;
                    double net = b->valuedouble / (1.0 + vat / 100.0) - fees;
                    if (net < 0) {
                        char m[160]; snprintf(m, sizeof(m), "tariff.zones.%.20s.buy_per_kwh: %.4f from the bill is less than the fees and VAT alone", z->string, b->valuedouble);
                        cJSON_Delete(cp); cJSON_Delete(diff); cJSON_Delete(warn); cJSON_Delete(own); cJSON_Delete(cur);
                        return fail(cx, "invalid_args", m, "Check the VAT and per-kWh fees in the plan (get_config tariff); with prices_from_bill the hub subtracts them.");
                    }
                    char wv[200]; snprintf(wv, sizeof(wv), "Zone %.20s: %.4f from the bill is stored as the energy price %.4f (divided by %.0f%% VAT, minus %.4f fees per kWh); the cost adds them back.", z->string, b->valuedouble, net, vat, fees);
                    cJSON_AddItemToArray(warn, cJSON_CreateString(wv));
                    cJSON_SetNumberValue(b, round(net * 10000.0) / 10000.0);
                    nconv++;
                }
                if (!nconv) { cJSON_Delete(cp); cJSON_Delete(diff); cJSON_Delete(warn); cJSON_Delete(own); cJSON_Delete(cur); return fail(cx, "invalid_args", "tariff.prices_from_bill: no zone buy_per_kwh in this patch to convert", NULL); }
            }
            cJSON_Delete(own); own = cp; patch = own;
        }
    }
    cJSON *norm = check_patch(cx, patch, cur, diff, warn);
    cJSON_Delete(own); cJSON_Delete(cur);
    if (!norm) { cJSON_Delete(diff); cJSON_Delete(warn); return NULL; }
    if (!cJSON_GetArraySize(diff)) {
        cJSON_Delete(norm);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "diff", diff);
        cJSON_AddItemToObject(r, "warnings", warn);
        cJSON_AddTrueToObject(r, "no_change");
        cJSON_AddNullToObject(r, "confirm_token");
        note(cx, rb ? "The settings already equal that version: nothing to roll back." : "The hub already has exactly these values: nothing to apply. Tell the user their values are already set.");
        cx->as_of = now_s();
        return r;
    }
    {
        char unv[96]; unversioned_parts(norm, unv, sizeof(unv));
        if (unv[0]) {
            char wv[200]; snprintf(wv, sizeof(wv), "Not versioned: %s. A rollback will not undo it; undoing it means proposing the old value again.", unv);
            cJSON_AddItemToArray(warn, cJSON_CreateString(wv));
        }
    }
    /* dedupe warnings */
    cJSON *w2 = cJSON_CreateArray(); const cJSON *w;
    cJSON_ArrayForEach(w, warn) { bool dup = false; const cJSON *x; cJSON_ArrayForEach(x, w2) if (!strcmp(x->valuestring, w->valuestring)) dup = true;
                                  if (!dup) cJSON_AddItemToArray(w2, cJSON_CreateString(w->valuestring)); }
    cJSON_Delete(warn);
    if (!summary[0]) {   /* the gist: the first changed fields, then a count */
        int nd = cJSON_GetArraySize(diff), k = 0; size_t o = 0;
        const cJSON *d;
        cJSON_ArrayForEach(d, diff) {
            if (k == 2) { snprintf(summary + o, sizeof(summary) - o, "; +%d more", nd - 2); break; }
            const cJSON *to = cJSON_GetObjectItem(d, "to");
            char tv[32];
            if (cJSON_IsString(to)) snprintf(tv, sizeof(tv), "%.28s", to->valuestring); else snprintf(tv, sizeof(tv), "%g", cJSON_GetNumberValue(to));
            int w = snprintf(summary + o, sizeof(summary) - o, "%s%.40s = %s", k ? "; " : "", cJSON_GetStringValue(cJSON_GetObjectItem(d, "path")), tv);
            if (w < 0 || (size_t)w >= sizeof(summary) - o) break;
            o += (size_t)w; k++;
        }
    }
    lk();
    int slot = 0; int64_t now = esp_timer_get_time();
    for (int i = 0; i < PROP_N; i++) { if (!s_prop[i].token[0] || now > s_prop[i].exp_us) { slot = i; break; } if (s_prop[i].exp_us < s_prop[slot].exp_us) slot = i; }
    prop_t *pp = &s_prop[slot];
    cJSON_Delete(pp->patch);
    memset(pp, 0, sizeof(*pp));
    rand_hex(pp->token, 16);
    strlcpy(pp->client, cx->client ? cx->client : "", sizeof(pp->client));
    pp->exp_us = now + (int64_t)PROP_TTL_S * 1000000;
    strlcpy(pp->base, base, sizeof(pp->base));
    pp->patch = norm;
    strlcpy(pp->summary, summary, sizeof(pp->summary));
    char tok[33]; strlcpy(tok, pp->token, sizeof(tok));
    ulk();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "diff", diff);
    cJSON_AddItemToObject(r, "warnings", w2);
    cJSON_AddBoolToObject(r, "restart_required", patch_restarts_hub(norm));
    if (S.read_only) { cJSON_AddTrueToObject(r, "read_only"); note(cx, "The hub is in read-only mode: this diff cannot be applied until the user switches read-only off on the hub's AI / LLM page."); }
    cJSON_AddBoolToObject(r, "module_restart", patch_restarts_module(norm));
    cJSON_AddStringToObject(r, "confirm_token", tok);
    cJSON_AddNumberToObject(r, "expires_in", PROP_TTL_S);
    { cJSON *vv = ver_load(); cJSON *va = cJSON_GetObjectItem(vv, "versions"); int nv = cJSON_GetArraySize(va);
      cJSON_AddNumberToObject(r, "base_version", nv ? cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(va, nv - 1), "version")) : 0); cJSON_Delete(vv); }
    note(cx, "Show the diff to the user and apply only after they say yes; the token works once and only for this diff.");
    cx->as_of = now_s();
    return r;
}

/* Apply a normalized patch through the core's own validated paths. On a failure mid-way the changes already made are
 * undone, so a patch is all-or-nothing. */
static bool apply_patch(rbllm_cx_t *cx, const cJSON *norm, const cJSON *before)
{
    const cJSON *e;
    cJSON *done = cJSON_CreateArray();
    bool ok = true;
    char m[200];
    if (!apply_more_first(norm, m, sizeof(m))) {   /* nothing changed yet */
        cJSON_Delete(done);
        fail(cx, "conflict", m, "Nothing was changed. Fix that part and propose again.");
        return false;
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(norm, "channels")) {
        unsigned uid = 0; sscanf(e->string, "%4x", &uid);
        cJSON *body = cJSON_CreateObject();
        const cJSON *f;
        cJSON_ArrayForEach(f, e) {
            if (!strcmp(f->string, "role")) {
                char up[24]; strlcpy(up, f->valuestring, sizeof(up)); for (char *q = up; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
                cJSON_AddStringToObject(body, "role", up);
            } else if (!strcmp(f->string, "inside")) {
                if (f->valuestring[0]) cJSON_AddStringToObject(body, "inside", f->valuestring); else cJSON_AddNumberToObject(body, "inside", 0);
            } else if (!strcmp(f->string, "ct_model")) {
                cJSON *cm = ct_models(); const cJSON *x;
                cJSON_ArrayForEach(x, cm) { char sh[24]; ct_short(x->valuestring, sh); if (!strcmp(sh, f->valuestring)) cJSON_AddNumberToObject(body, "ct_code", atoi(x->string)); }
                cJSON_Delete(cm);
            } else if (cJSON_IsNumber(f)) cJSON_AddNumberToObject(body, f->string, f->valuedouble);
            else cJSON_AddStringToObject(body, f->string, f->valuestring);
        }
        char *js = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        int st = 0;
        char *rep = js ? rbgrid_units_mutate((rbpower_unit_id_t)uid, "", js, &st) : NULL;
        free(js);
        if (st != 200) {
            cJSON *rj = rep ? cJSON_Parse(rep) : NULL;
            const char *msg = rj ? cJSON_GetStringValue(cJSON_GetObjectItem(rj, "message")) : NULL;
            snprintf(m, sizeof(m), "channel %s: the hub refused the change (%d: %.120s)", e->string, st, msg ? msg : "?");
            cJSON_Delete(rj); free(rep);
            ok = false;
            break;
        }
        free(rep);
        cJSON_AddItemToArray(done, cJSON_CreateString(e->string));
    }
    const cJSON *pcd = cJSON_GetObjectItem(norm, "peaks_cycle_day");
    const cJSON *ntf = cJSON_GetObjectItem(norm, "tariff");
    if (ok && ntf && !apply_tariff(cx, ntf, m, sizeof(m))) ok = false;
    if (ok && pcd) {
        cJSON *d = cJSON_CreateObject(); cJSON_AddNumberToObject(d, "cycle_day", pcd->valuedouble);
        char em[64] = "";
        if (rbpeak_set(d, em, sizeof(em)) != ESP_OK) { snprintf(m, sizeof(m), "peaks_cycle_day: the hub refused it (%s)", em); ok = false; }
        cJSON_Delete(d);
    }
    if (ok && !apply_more_last(norm, m, sizeof(m))) ok = false;
    if (!ok) {
        /* undo what was applied: restore the before values of those channels */
        cJSON *undo = cJSON_CreateObject(), *uch = cJSON_AddObjectToObject(undo, "channels");
        const cJSON *id;
        cJSON_ArrayForEach(id, done) {
            const cJSON *b = cJSON_GetObjectItem(cJSON_GetObjectItem(before, "channels"), id->valuestring);
            if (b) cJSON_AddItemToObject(uch, id->valuestring, cJSON_Duplicate(b, true));
        }
        if (cJSON_GetArraySize(uch)) (void)apply_patch(cx, undo, before);
        cJSON_Delete(undo);
        cJSON_Delete(done);
        fail(cx, "conflict", m, "Nothing was changed. Read get_config and propose again.");
        return false;
    }
    cJSON_Delete(done);
    return true;
}

static cJSON *t_apply_config(const cJSON *a, rbllm_cx_t *cx)
{
    const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(a, "confirm_token"));
    if (!tok) return fail(cx, "confirm_required", "confirm_token required", "Call propose_config first.");
    lk();
    prop_t *pp = NULL; bool expired = false;
    for (int i = 0; i < PROP_N; i++) if (s_prop[i].token[0] && !strcmp(s_prop[i].token, tok)) pp = &s_prop[i];
    if (pp && esp_timer_get_time() > pp->exp_us) expired = true;
    prop_t p = {0};
    if (pp && !expired) { p = *pp; pp->patch = NULL; }
    if (pp) { cJSON_Delete(pp->patch); memset(pp, 0, sizeof(*pp)); }   /* single use, also when expired */
    ulk();
    if (!pp) return fail(cx, "confirm_invalid", "unknown or used confirm_token", "Call propose_config again.");
    if (expired) return fail(cx, "confirm_expired", "the proposal expired (10 min)", "Call propose_config again.");
    cJSON *cur = cfg_capture();
    char h[17]; cfg_hash(cur, h);
    if (strcmp(h, p.base)) { cJSON_Delete(cur); cJSON_Delete(p.patch); return fail(cx, "conflict", "the settings changed since the proposal", "Read get_config and propose again."); }
    int pv = ver_sync("hub", NULL);   /* the state before, if not yet recorded */
    char unv[96]; unversioned_parts(p.patch, unv, sizeof(unv));   /* the parts of this patch a rollback cannot undo */
    bool modr = patch_restarts_module(p.patch), hubr = patch_restarts_hub(p.patch);
    bool ok = apply_patch(cx, p.patch, cur);
    cJSON_Delete(cur); cJSON_Delete(p.patch);
    if (!ok) return NULL;
    char src[48];
    if (cx->internal && !strcmp(p.client, RBLLM_CLIENT_WEB)) strlcpy(src, "web", sizeof(src));   /* the user on the hub page */
    else snprintf(src, sizeof(src), "llm:%.40s", p.client[0] ? p.client : "client");
    int v = ver_sync(src, p.summary);
    bool versioned = v != pv;
    rbpower_diag_printf("llm: config %s applied by '%s' (%s)", versioned ? "version" : "change (not versioned)", p.client, p.summary);
    if (versioned) rbpower_diag_printf("llm: config version %d", v);
    else { char inf[40]; snprintf(inf, sizeof(inf), "0|%.37s", src); ev_add(EV_CONFIG, SEV_INFO, 0, inf); }   /* still a config change in get_events */
    s_cfg_changed = true;
    cJSON *r = cJSON_CreateObject();
    if (versioned) cJSON_AddNumberToObject(r, "version", v); else cJSON_AddNullToObject(r, "version");
    cJSON_AddBoolToObject(r, "versioned", versioned);
    cJSON_AddBoolToObject(r, "applied", true);
    cJSON_AddBoolToObject(r, "restart_required", hubr);
    cJSON_AddBoolToObject(r, "module_restart", modr);
    if (hubr) note(cx, "Part of this applies after a restart: ask the user, then restart_hub.");
    cJSON_AddStringToObject(r, "summary", p.summary);
    if (versioned && !unv[0]) note(cx, "Applied. list_config_versions shows it; propose_config {rollback_to} undoes it.");
    else if (versioned) {
        char nb[260]; snprintf(nb, sizeof(nb), "Applied. list_config_versions shows it, but a rollback does not undo %s: to undo that part, propose its old value (the diff's from) again.", unv);
        note(cx, nb);
    } else {
        char nb[260]; snprintf(nb, sizeof(nb), "Applied, but not versioned (%s): no rollback undoes it. To undo it, propose the old value (the diff's from) again.", unv[0] ? unv : "no versioned setting changed");
        note(cx, nb);
    }
    cx->as_of = now_s();
    return r;
}

static cJSON *t_list_config_versions(const cJSON *a, rbllm_cx_t *cx)
{
    (void)a;
    ver_sync("hub", NULL);
    cJSON *v = ver_load();
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "versions");
    const cJSON *e;
    int n = cJSON_GetArraySize(cJSON_GetObjectItem(v, "versions"));
    for (int k = n - 1; k >= 0; k--) {   /* newest first */
        e = cJSON_GetArrayItem(cJSON_GetObjectItem(v, "versions"), k);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "version", cJSON_GetNumberValue(cJSON_GetObjectItem(e, "version")));
        uint32_t ts = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "ts"));
        if (ts > RBGRID_TS_SANE_MIN) add_time(o, "time", ts); else cJSON_AddNullToObject(o, "time");
        cJSON_AddStringToObject(o, "source", cJSON_GetStringValue(cJSON_GetObjectItem(e, "source")));
        char sm[128]; utf8_clean(cJSON_GetStringValue(cJSON_GetObjectItem(e, "summary")), sm, sizeof(sm));
        cJSON_AddStringToObject(o, "summary", sm);
        cJSON_AddBoolToObject(o, "current", k == n - 1);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_Delete(v);
    note(cx, "Versions cover channel names, short names, roles, inside, clamp models, phases, rated power, breaker current, EV thresholds, the tariff prices/VAT/fees, the peaks cycle day, the time zone and the hub's name and location. Price rules, telemetry, integrations, the dynamic price, the mains voltage/frequency and the grid system are not versioned either. Modules, module addresses, points made or moved and bus pins are not versioned: a rollback does not undo them.");
    cx->as_of = now_s();
    return r;
}

static cJSON *t_rollback_config(const cJSON *a, rbllm_cx_t *cx)
{
    if (!cJSON_GetObjectItem(a, "confirm_token"))
        return fail(cx, "confirm_required", "a rollback takes two steps", "Call propose_config {rollback_to: <version>}, show the diff, then rollback_config with its confirm_token.");
    return t_apply_config(a, cx);
}

static cJSON *t_get_config_schema(const cJSON *a, rbllm_cx_t *cx)
{
    const char *sec = cJSON_GetStringValue(cJSON_GetObjectItem(a, "section"));
    if (!sec || (strcmp(sec, "channels") && strcmp(sec, "notifications") && strcmp(sec, "tariff") && strcmp(sec, "region") &&
                 strcmp(sec, "hub") && strcmp(sec, "integrations") && strcmp(sec, "dynamic_price")))
        return fail(cx, "invalid_args", "section: channels, notifications, tariff, region, hub, integrations or dynamic_price", NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "section", sec);
    cJSON *w = cJSON_AddObjectToObject(r, "writable");
    if (!strcmp(sec, "channels")) {
        cJSON *f = cJSON_AddObjectToObject(w, "name");
        cJSON_AddStringToObject(f, "type", "string"); cJSON_AddNumberToObject(f, "max_bytes", 31);
        cJSON_AddStringToObject(f, "description", "The user's name for the point, shown in the app and HA.");
        f = cJSON_AddObjectToObject(w, "role");
        cJSON_AddStringToObject(f, "type", "string");
        cJSON *en = cJSON_AddArrayToObject(f, "enum");
        for (int q = 0; q < 10; q++) cJSON_AddItemToArray(en, cJSON_CreateString(role_name((uint8_t)q)));
        cJSON_AddStringToObject(f, "description", "What the point measures. main_supply = the grid feed (the bill and the grid figure follow it); alternative_supply = a second feed.");
        f = cJSON_AddObjectToObject(w, "inside");
        cJSON_AddStringToObject(f, "type", "string");
        cJSON_AddStringToObject(f, "description", "The channel id this point is measured inside (a sub-meter of it), or \"\" for top level.");
        f = cJSON_AddObjectToObject(w, "ct_model");
        cJSON_AddStringToObject(f, "type", "string");
        cJSON *ce = cJSON_AddArrayToObject(f, "enum");
        cJSON *cm = ct_models(); const cJSON *x;
        cJSON_ArrayForEach(x, cm) { char sh[24]; ct_short(x->valuestring, sh); cJSON_AddItemToArray(ce, cJSON_CreateString(sh)); }
        cJSON_Delete(cm);
        cJSON *on = cJSON_AddArrayToObject(f, "channels");
        ct_info_t ci[RBLLM_MAX_POINTS]; int nci = ct_infos(ci, RBLLM_MAX_POINTS);
        for (int q = 0; q < nci; q++) if (ci[q].settable) cJSON_AddItemToArray(on, cJSON_CreateString(ci[q].id));
        cJSON_AddStringToObject(f, "description", "The clamp model on the channel (printed on the clamp); only for the channels listed, whose module lets the user pick it.");
        f = cJSON_AddObjectToObject(w, "short_name"); cJSON_AddStringToObject(f, "type", "string"); cJSON_AddNumberToObject(f, "max_bytes", 24);
        cJSON_AddStringToObject(f, "description", "A short label for small screens.");
        const struct { const char *k, *t; double hi; const char *d; } CN[] = {
            { "phase", "integer", 7, "0 unset, 1..3 = L1..L3, 4 = three-phase, 5..7 = L1L2/L2L3/L3L1" },
            { "rated_power_w", "number", 1000000, "the appliance's rated power, 0 = unset (used for load percent)" },
            { "max_current_a", "number", 1000, "the circuit's breaker current, 0 = unset (used for overload warnings)" },
            { "charge_threshold_w", "integer", 100000, "EV points: power above which a charge session runs, 0 = hub default" },
            { "idle_timeout_min", "integer", 1440, "EV points: minutes below the threshold that end a session, 0 = hub default" } };
        for (int q = 0; q < 5; q++) { f = cJSON_AddObjectToObject(w, CN[q].k); cJSON_AddStringToObject(f, "type", CN[q].t); cJSON_AddNumberToObject(f, "minimum", 0); cJSON_AddNumberToObject(f, "maximum", CN[q].hi); cJSON_AddStringToObject(f, "description", CN[q].d); }
    } else if (!strcmp(sec, "notifications")) {
        cJSON *f = cJSON_AddObjectToObject(w, "peaks_cycle_day");
        cJSON_AddStringToObject(f, "type", "integer"); cJSON_AddNumberToObject(f, "minimum", 1); cJSON_AddNumberToObject(f, "maximum", 28);
        cJSON_AddStringToObject(f, "description", "Day of the month the import-peak cycle starts (the grid operator's billing day).");
        f = cJSON_AddObjectToObject(w, "price_rules"); cJSON_AddStringToObject(f, "type", "array");
        cJSON_AddStringToObject(f, "description", "patch.price_rules: the WHOLE list of cheap-window rules, each as get_config notifications.price_rules shows one (kinds threshold, cheapest, deadline); replaces the list. The hub names a bad field at apply_config.");
        f = cJSON_AddObjectToObject(w, "price_signal"); cJSON_AddStringToObject(f, "type", "object");
        cJSON_AddStringToObject(f, "description", "patch.price_signal: the price notification settings, as get_config notifications.price_signal shows them.");
        f = cJSON_AddObjectToObject(w, "peaks_reset"); cJSON_AddStringToObject(f, "type", "boolean");
        cJSON_AddStringToObject(f, "description", "patch.peaks_reset: true clears the current cycle's import peaks (cannot be undone).");
    } else if (!strcmp(sec, "region")) {
        cJSON *f = cJSON_AddObjectToObject(w, "timezone"); cJSON_AddStringToObject(f, "type", "string"); cJSON_AddNumberToObject(f, "max_bytes", 31);
        cJSON_AddStringToObject(f, "description", "patch.region.timezone: an IANA name (Europe/Lisbon, America/New_York: the hub turns common ones into their rule) or a POSIX TZ with its offset and DST rule, e.g. CET-1CEST,M3.5.0,M10.5.0/3 (Berlin, Paris), EET-2EEST,M3.5.0/3,M10.5.0/4 (Kyiv, Helsinki), GMT0BST,M3.5.0/1,M10.5.0 (London), MSK-3 (Moscow), CST-8 (China), <+04>-4. Note the sign: east of UTC is negative.");
        f = cJSON_AddObjectToObject(w, "nominal_voltage"); cJSON_AddStringToObject(f, "type", "number"); cJSON_AddNumberToObject(f, "minimum", 90); cJSON_AddNumberToObject(f, "maximum", 260);
        cJSON_AddStringToObject(f, "description", "patch.region.nominal_voltage: mains line-to-neutral volts (230 Europe, 220 China/CIS, 120 North America, 100 Japan); used for channels that measure current only. Full effect after restart_hub.");
        f = cJSON_AddObjectToObject(w, "nominal_frequency"); cJSON_AddStringToObject(f, "type", "integer"); cJSON *fe = cJSON_AddArrayToObject(f, "enum");
        cJSON_AddItemToArray(fe, cJSON_CreateNumber(50)); cJSON_AddItemToArray(fe, cJSON_CreateNumber(60));
        cJSON_AddStringToObject(f, "description", "patch.region.nominal_frequency: 50 or 60 Hz. Applies after restart_hub.");
    } else if (!strcmp(sec, "hub")) {
        const char *ss[] = { "name", "location" };
        for (int q = 0; q < 2; q++) { cJSON *f = cJSON_AddObjectToObject(w, ss[q]); cJSON_AddStringToObject(f, "type", "string"); cJSON_AddNumberToObject(f, "max_bytes", 31); }
        cJSON_AddStringToObject(cJSON_GetObjectItem(w, "name"), "description", "patch.hub.name: the hub's name in the app and Home Assistant.");
        cJSON *f = cJSON_AddObjectToObject(w, "grid_system"); cJSON_AddStringToObject(f, "type", "string"); cJSON *ge = cJSON_AddArrayToObject(f, "enum");
        cJSON_AddItemToArray(ge, cJSON_CreateString("1P")); cJSON_AddItemToArray(ge, cJSON_CreateString("SPLIT")); cJSON_AddItemToArray(ge, cJSON_CreateString("3P"));
        cJSON_AddStringToObject(f, "description", "patch.hub.grid_system: the site's supply: single phase, split phase (120/240 V) or three phase.");
    } else if (!strcmp(sec, "integrations")) {
        cJSON *f = cJSON_AddObjectToObject(w, "broker_uri"); cJSON_AddStringToObject(f, "type", "string"); cJSON_AddNumberToObject(f, "max_bytes", 159);
        cJSON_AddStringToObject(f, "description", "patch.mqtt.broker_uri: mqtt://host:1883 or mqtts://user:password@host:8883 (the Home Assistant MQTT broker). Applies after restart_hub.");
        f = cJSON_AddObjectToObject(w, "telemetry.profile"); cJSON_AddStringToObject(f, "type", "string"); cJSON *pe = cJSON_AddArrayToObject(f, "enum");
        cJSON_AddItemToArray(pe, cJSON_CreateString("lean")); cJSON_AddItemToArray(pe, cJSON_CreateString("full"));
        cJSON_AddStringToObject(f, "description", "patch.telemetry.profile: lean = the main figures every 3 s; full = every field. Applies at once.");
        f = cJSON_AddObjectToObject(w, "telemetry.frame"); cJSON_AddStringToObject(f, "type", "integer"); cJSON *fe = cJSON_AddArrayToObject(f, "enum");
        cJSON_AddItemToArray(fe, cJSON_CreateNumber(1)); cJSON_AddItemToArray(fe, cJSON_CreateNumber(2));
        cJSON_AddStringToObject(f, "description", "patch.telemetry.frame: the MQTT telemetry key layout, 1 or 2. Home Assistant entities follow; other MQTT readers must change their keys.");
    } else if (!strcmp(sec, "dynamic_price")) {
        cJSON *f = cJSON_AddObjectToObject(w, "dynamic_price"); cJSON_AddStringToObject(f, "type", "object");
        cJSON_AddStringToObject(f, "description", "patch.dynamic_price: the provider settings, with the field names get_config {section: dynamic_price} shows (it also lists the providers the hub knows). Only the fields given change.");
    } else if (!strcmp(sec, "tariff")) {
        const char *ss[] = { "plan_name", "provider" };
        for (int q = 0; q < 2; q++) { cJSON *f = cJSON_AddObjectToObject(w, ss[q]); cJSON_AddStringToObject(f, "type", "string"); cJSON_AddNumberToObject(f, "max_bytes", 31); }
        cJSON *f = cJSON_AddObjectToObject(w, "currency"); cJSON_AddStringToObject(f, "type", "string"); cJSON_AddStringToObject(f, "description", "ISO 4217 code, e.g. EUR");
        const struct { const char *k; double hi; const char *d; } NN[] = {
            { "vat_percent", 50, "VAT in percent, added on top of energy price and fees" },
            { "energy_tax_per_kwh", 10, "energy tax per kWh" }, { "grid_fee_per_kwh", 10, "grid usage fee per kWh" }, { "levy_per_kwh", 10, "renewable levy per kWh" } };
        for (int q = 0; q < 4; q++) { f = cJSON_AddObjectToObject(w, NN[q].k); cJSON_AddStringToObject(f, "type", "number"); cJSON_AddNumberToObject(f, "minimum", 0); cJSON_AddNumberToObject(f, "maximum", NN[q].hi); cJSON_AddStringToObject(f, "description", NN[q].d); }
        f = cJSON_AddObjectToObject(w, "zones");
        cJSON_AddStringToObject(f, "type", "object");
        cJSON_AddStringToObject(f, "description", "{<zone>: {buy_per_kwh?, sell_per_kwh?, window?, days?, priority?, enabled?}}. Zone types: night, morning_peak, day, evening_peak, late_evening, weekend, holiday, critical_peak. Prices 0..100 per kWh before fees and VAT (a price read off the bill includes them: send it as is with prices_from_bill:true); window \"HH:MM-HH:MM\" (15-min grid, [from,to), wraps midnight); days \"mon-fri\", \"sat,sun\" or \"all\"; priority 1..100 (higher wins on overlap); enabled:false removes a zone. A zone the plan lacks is added: it needs buy_per_kwh and a window and/or days.");
        f = cJSON_AddObjectToObject(w, "prices_from_bill"); cJSON_AddStringToObject(f, "type", "boolean");
        cJSON_AddStringToObject(f, "description", "true when the zone buy prices in this patch are the per-kWh price from the bill (VAT and per-kWh fees included), which is what a user reads; the hub divides by 1 + VAT/100 and subtracts the fees before storing. Leave it out for prices before fees and VAT. sell_per_kwh is not converted.");
        f = cJSON_AddObjectToObject(w, "default_zone"); cJSON_AddStringToObject(f, "type", "string");
        cJSON_AddStringToObject(f, "description", "The zone for hours no other zone covers.");
        note(cx, "A flat tariff on a two-zone plan: give every zone the same price.");
    } else {
    }
    cJSON_AddStringToObject(r, "how", "propose_config {patch: {...}}: channels {<id>: {field}}, tariff {...}, peaks_cycle_day, price_rules, price_signal, peaks_reset, region {timezone}, hub {name, location}, mqtt {broker_uri}, dynamic_price {...}; several sections may go in one patch.");
    cx->as_of = now_s();
    return r;
}

static cJSON *t_get_analysis_playbook(const cJSON *a, rbllm_cx_t *cx);

/* ============================================================================================ registry */

typedef cJSON *(*rbllm_fn_t)(const cJSON *args, rbllm_cx_t *cx);
typedef struct {
    const char *name;
    uint8_t     scope;
    const char *desc;
    const char *input;     /* JSON Schema (object) */
    const char *output;    /* JSON Schema of `result` (key fields) */
    rbllm_fn_t  fn;
} rbllm_tool_t;

#define S_PERIOD "{\"type\":\"object\",\"description\":\"Either preset, or from+to (ISO 8601 date or date-time; no offset = hub local time; to is exclusive).\"," \
                 "\"properties\":{\"preset\":{\"type\":\"string\",\"enum\":[\"today\",\"yesterday\",\"last_7_days\",\"last_30_days\",\"this_week\",\"last_week\",\"this_month\",\"last_month\",\"this_year\",\"last_year\"]}," \
                 "\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}},\"additionalProperties\":false}"
#define S_CHANNELS "{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Channel ids from list_channels; omit for all.\"}"

/* ============================================================================================ identify_channel
 * Two calls: start (baseline = power now, then a 1 s sampler keeps min/max per point for the window) and result (rank
 * the points by their step). The sampler is an esp_timer; it takes the home lock with a short timeout and skips a tick
 * rather than wait. State is small and lives in PSRAM; s_id_mux guards it between the timer and the worker. */
#define ID_MAX_WIN_S   120
#define ID_MIN_STEP_W  60.0f
typedef struct {
    bool     active;          /* a session exists (running or finished) */
    bool     running;         /* the sampler is still sampling */
    int64_t  t0_us, last_us;
    uint32_t win_s, samples;
    int      n;
    uint16_t uid[RBLLM_MAX_POINTS];
    uint8_t  role[RBLLM_MAX_POINTS];
    float    base[RBLLM_MAX_POINTS], mn[RBLLM_MAX_POINTS], mx[RBLLM_MAX_POINTS], now[RBLLM_MAX_POINTS];
    float    gbase, gmn, gmx, gnow;   /* grid (system) power */
} id_state_t;
static EXT_RAM_BSS_ATTR id_state_t s_id;
static EXT_RAM_BSS_ATTR rbgrid_point_snap_t s_id_snap[RBLLM_MAX_POINTS];
static portMUX_TYPE s_id_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_id_timer;

/* Take one sample into s_id (base = true: this sample is the baseline). Returns false if the lock was busy. */
static bool id_sample(bool base)
{
    if (!rbpower_home_lock_timeout(50)) return false;
    int n = rbgrid_core_points_snapshot(s_id_snap, RBLLM_MAX_POINTS);
    rbgrid_system_totals_t st;
    rbgrid_core_system_totals(&st, false);
    rbpower_home_unlock();
    int64_t t = esp_timer_get_time();
    portENTER_CRITICAL(&s_id_mux);
    if (base) { s_id.n = 0; s_id.gbase = s_id.gmn = s_id.gmx = (float)st.p_w; }
    for (int i = 0; i < n; i++) {
        float w = s_id_snap[i].p_w;
        int k = 0;
        while (k < s_id.n && s_id.uid[k] != s_id_snap[i].unit_id) k++;
        if (k == s_id.n) {                     /* new point (or the baseline pass) */
            if (s_id.n >= RBLLM_MAX_POINTS) continue;
            s_id.n++; s_id.uid[k] = s_id_snap[i].unit_id; s_id.base[k] = s_id.mn[k] = s_id.mx[k] = w;
        }
        s_id.role[k] = s_id_snap[i].role;
        if (w < s_id.mn[k]) s_id.mn[k] = w;
        if (w > s_id.mx[k]) s_id.mx[k] = w;
        s_id.now[k] = w;
    }
    float g = (float)st.p_w;
    if (g < s_id.gmn) s_id.gmn = g;
    if (g > s_id.gmx) s_id.gmx = g;
    s_id.gnow = g;
    s_id.samples++; s_id.last_us = t;
    bool done = (t - s_id.t0_us) >= (int64_t)s_id.win_s * 1000000LL;
    if (done) s_id.running = false;
    portEXIT_CRITICAL(&s_id_mux);
    if (done && s_id_timer) esp_timer_stop(s_id_timer);
    return true;
}

static void id_tick(void *arg) { (void)arg; if (s_id.running) id_sample(false); }

/* The signed step of a point: the larger excursion from its baseline (on = positive, off = negative). */
static float id_step(float base, float mn, float mx) { return (mx - base) >= (base - mn) ? mx - base : mn - base; }

static cJSON *t_identify_channel(const cJSON *a, rbllm_cx_t *cx)
{
    const char *act = cJSON_GetStringValue(cJSON_GetObjectItem(a, "action"));
    if (!act || (strcmp(act, "start") && strcmp(act, "result")))
        return fail(cx, "invalid_args", "action: \"start\" or \"result\" required", "Call start, ask the user to switch one appliance on or off, then call result.");
    if (!strcmp(act, "start")) {
        const cJSON *jw = cJSON_GetObjectItem(a, "window_s");
        uint32_t win = cJSON_IsNumber(jw) ? (uint32_t)(jw->valuedouble < 10 ? 10 : jw->valuedouble > ID_MAX_WIN_S ? ID_MAX_WIN_S : jw->valuedouble) : 60;
        if (!s_id_timer) {
            const esp_timer_create_args_t ta = { .callback = id_tick, .name = "llm_ident" };
            if (esp_timer_create(&ta, &s_id_timer) != ESP_OK) return fail(cx, "internal", "sampler not available", NULL);
        }
        esp_timer_stop(s_id_timer);                  /* a new start replaces the previous session */
        portENTER_CRITICAL(&s_id_mux);
        memset(&s_id, 0, sizeof(s_id));
        s_id.active = true; s_id.running = true; s_id.win_s = win; s_id.t0_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_id_mux);
        bool ok = false;
        for (int i = 0; i < 5 && !(ok = id_sample(true)); i++) vTaskDelay(pdMS_TO_TICKS(20));
        if (!ok) { s_id.active = s_id.running = false; return fail(cx, "busy", "the hub is busy; the baseline could not be taken", "Retry in a second."); }
        esp_timer_start_periodic(s_id_timer, 1000000);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "status", "sampling");
        cJSON_AddNumberToObject(r, "window_s", win);
        cJSON_AddNumberToObject(r, "points", s_id.n);
        char nx[320];
        snprintf(nx, sizeof(nx), "Ask the user to switch ONE appliance of at least about 100 W on (or off) now and leave it so until you call "
                 "result (the hub watches for %u s; 10 s is enough). Then call identify_channel with action \"result\". "
                 "Other big loads should stay as they are.", (unsigned)win);
        cJSON_AddStringToObject(r, "next", nx);
        cx->as_of = now_s(); cx->resolution = "live";
        return r;
    }
    /* result */
    id_state_t s;
    portENTER_CRITICAL(&s_id_mux);
    memcpy(&s, &s_id, sizeof(s));
    portEXIT_CRITICAL(&s_id_mux);
    if (!s.active) return fail(cx, "invalid_args", "no identification is running", "Call identify_channel with action \"start\" first.");
    int64_t el_us = esp_timer_get_time() - s.t0_us;
    rbgrid_point_snap_t *p = calloc(RBLLM_MAX_POINTS, sizeof(*p));
    if (!p) return fail(cx, "internal", "out of memory", NULL);
    int n = snapshot(p, RBLLM_MAX_POINTS);
    /* rank the non-main points by |step| */
    int ord[RBLLM_MAX_POINTS]; int m = 0;
    for (int k = 0; k < s.n; k++) if (s.role[k] != RBPOWER_ROLE_MAIN_SUPPLY) ord[m++] = k;
    for (int i = 1; i < m; i++) {                    /* insertion sort, m ≤ 24 */
        int v = ord[i], j = i;
        float sv = fabsf(id_step(s.base[v], s.mn[v], s.mx[v]));
        while (j > 0 && fabsf(id_step(s.base[ord[j - 1]], s.mn[ord[j - 1]], s.mx[ord[j - 1]])) < sv) { ord[j] = ord[j - 1]; j--; }
        ord[j] = v;
    }
    float gstep = id_step(s.gbase, s.gmn, s.gmx);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", s.running ? "sampling" : "finished");
    cJSON_AddNumberToObject(r, "elapsed_s", (double)(el_us / 1000000LL));
    cJSON_AddNumberToObject(r, "window_s", s.win_s);
    cJSON_AddNumberToObject(r, "samples", s.samples);
    cJSON *g = cJSON_AddObjectToObject(r, "grid");
    cJSON_AddNumberToObject(g, "baseline_w", r2(s.gbase));
    cJSON_AddNumberToObject(g, "step_w", r2(gstep));
    cJSON_AddNumberToObject(g, "now_w", r2(s.gnow));
    cJSON *arr = cJSON_AddArrayToObject(r, "candidates");
    for (int i = 0; i < m && i < 5; i++) {
        int k = ord[i];
        float st = id_step(s.base[k], s.mn[k], s.mx[k]);
        if (fabsf(st) < 5.0f) break;                 /* noise */
        cJSON *o = cJSON_CreateObject();
        char h[5]; uid_hex(s.uid[k], h);
        cJSON_AddStringToObject(o, "id", h);
        for (int q = 0; q < n; q++) if (p[q].unit_id == s.uid[k]) { add_name(o, p[q].name); break; }
        cJSON_AddStringToObject(o, "role", role_class(s.role[k]));
        cJSON_AddNumberToObject(o, "baseline_w", r2(s.base[k]));
        cJSON_AddNumberToObject(o, "step_w", r2(st));
        cJSON_AddNumberToObject(o, "now_w", r2(s.now[k]));
        cJSON_AddItemToArray(arr, o);
    }
    /* the verdict */
    const char *verdict = "no_step";
    bool nested_pick = false, same_wire = false;
    if (m > 1) {
        /* two similar steps where one point is measured inside the other: the appliance is on the inner one */
        int k0 = ord[0], k1 = ord[1];
        float a0 = fabsf(id_step(s.base[k0], s.mn[k0], s.mx[k0])), a1 = fabsf(id_step(s.base[k1], s.mn[k1], s.mx[k1]));
        if (a0 >= ID_MIN_STEP_W && a0 < 2.0f * a1) {
            uint16_t in0 = 0, in1 = 0;
            for (int q = 0; q < n; q++) { if (p[q].unit_id == s.uid[k0]) in0 = p[q].inside; if (p[q].unit_id == s.uid[k1]) in1 = p[q].inside; }
            if (in1 == s.uid[k0]) { ord[0] = k1; ord[1] = k0; nested_pick = true; }        /* k1 is inside k0 */
            else if (in0 == s.uid[k1]) nested_pick = true;                                  /* k0 already the inner */
            else if (fabsf(gstep) >= ID_MIN_STEP_W && a0 / fabsf(gstep) > 0.8f && a1 / fabsf(gstep) > 0.8f) same_wire = true;
        }
    }
    if (m > 0) {
        int k = ord[0];
        float s1 = id_step(s.base[k], s.mn[k], s.mx[k]);
        float s2 = m > 1 ? fabsf(id_step(s.base[ord[1]], s.mn[ord[1]], s.mx[ord[1]])) : 0.0f;
        if (fabsf(s1) >= ID_MIN_STEP_W) {
            verdict = (nested_pick || fabsf(s1) >= 2.0f * s2) ? "identified" : "ambiguous";
            if (nested_pick) note(cx, "The step shows on a channel and on the channel it is measured inside; the appliance is on the inner one (channel_id).");
            if (same_wire) note(cx, "Both channels saw the whole step: one circuit feeds the other (set inside on the inner channel), or two clamps sit on the same cable.");
            char h[5]; uid_hex(s.uid[k], h);
            if (!strcmp(verdict, "identified")) cJSON_AddStringToObject(r, "channel_id", h);
            /* the grid sees the same appliance: opposite signs on a load point = a clamp the wrong way round */
            if (fabsf(gstep) >= ID_MIN_STEP_W && s1 * gstep < 0 && !strcmp(role_class(s.role[k]), "load")) {
                cJSON_AddBoolToObject(r, "reversed_ct_suspected", true);
                note(cx, "The channel moved against the grid: its clamp is probably on the wrong way round (fix at the clamp, then check again).");
            }
            if (fabsf(gstep) >= ID_MIN_STEP_W) {
                float ratio = fabsf(s1) / fabsf(gstep);
                cJSON_AddNumberToObject(r, "step_vs_grid", round(ratio * 100.0) / 100.0);
                if (ratio < 0.6f || ratio > 1.6f) note(cx, "The channel's step differs from the grid's: another load changed at the same time, or the clamp's model or phase is set wrong.");
            } else if (s.n && fabsf(s1) >= ID_MIN_STEP_W) {
                note(cx, "The grid did not move with the channel: the channel may be on a circuit the grid point does not see, or the main clamp is missing.");
            }
        }
    }
    if (!strcmp(verdict, "no_step") && fabsf(gstep) >= ID_MIN_STEP_W) {
        verdict = "grid_only";
        char nb[200];
        snprintf(nb, sizeof(nb), "The grid saw a step of %.0f W but no channel did: the appliance is on a circuit without its own clamp (only the main supply measures it).", fabsf(gstep));
        note(cx, nb);
    }
    cJSON_AddStringToObject(r, "verdict", verdict);
    if (!strcmp(verdict, "no_step") && s.running)
        note(cx, "No clear step yet (the hub needs a change of about 60 W; an appliance of 100 W or more gives a clear one). Ask the user whether the appliance is on, then call result again (the window is still open).");
    else if (!strcmp(verdict, "no_step"))
        note(cx, "No clear step in the window (the hub needs a change of about 60 W). Start again with a bigger appliance (kettle, heater).");
    else if (!strcmp(verdict, "ambiguous") && !same_wire)   /* same cable: repeating won't separate them */
        note(cx, "Two channels moved by a similar amount: ask the user to switch the appliance off again and call start once more.");
    free(p);
    cx->as_of = now_s(); cx->resolution = "live";
    return r;
}

/* ---- module setup (scope config): the same path as the hub page's Add-a-module flow ---- */
static cJSON *i2c_fail(rbllm_cx_t *cx, cJSON *rep, int st)
{
    const char *m = rep ? cJSON_GetStringValue(cJSON_GetObjectItem(rep, "message")) : NULL;
    char msg[160]; snprintf(msg, sizeof(msg), "%s", m && m[0] ? m : "the I2C action failed");
    cJSON_Delete(rep);
    if (!s_i2c_action) return fail(cx, "internal", "module setup is not available on this hub", NULL);
    const char *code = st == 404 ? "module_not_found" : st == 409 ? "conflict" : st == 400 ? "invalid_args" : "internal";
    if (strstr(msg, "already taken") || strstr(msg, "configured for another module")) code = "address_taken";
    else if (strstr(msg, "not present")) code = "module_not_found";
    else if (strstr(msg, "not confirmed by UID") || strstr(msg, "may still answer")) code = "module_silent";
    else if (strstr(msg, "out of 0x08")) code = "address_out_of_range";
    if (strstr(msg, "is not running") || !strcmp(msg, "ESP_ERR_INVALID_STATE"))
        return fail(cx, "conflict", "I2C bus 1 is not running: it has no pins yet, so nothing on it can be scanned, added or read",
                    "Ask which GPIO pins the bus-1 connector uses (SDA, SCL), call set_bus_pins, then restart_hub after the user agrees, then scan_modules {bus: 1}.");
    return fail(cx, code, msg, st == 409 || !strcmp(code, "address_taken") ? "Call scan_modules to see what answers on the bus, then pick a free address." : NULL);
}
static void modules_view(cJSON *out, const cJSON *state)
{
    cJSON *mods = cJSON_AddArrayToObject(out, "modules");
    const cJSON *im;
    cJSON_ArrayForEach(im, cJSON_GetObjectItem(state, "modules")) {
        cJSON *o = cJSON_CreateObject();
        char ad[8]; snprintf(ad, sizeof(ad), "0x%02X", (int)cJSON_GetNumberValue(cJSON_GetObjectItem(im, "addr")));
        cJSON_AddNumberToObject(o, "module", cJSON_GetNumberValue(cJSON_GetObjectItem(im, "module")));
        cJSON_AddNumberToObject(o, "bus", cJSON_GetNumberValue(cJSON_GetObjectItem(im, "bus")));
        cJSON_AddStringToObject(o, "address", ad);
        const char *md = cJSON_GetStringValue(cJSON_GetObjectItem(im, "model"));
        if (md) cJSON_AddStringToObject(o, "model", md);
        cJSON_AddBoolToObject(o, "online", cJSON_IsTrue(cJSON_GetObjectItem(im, "present")));
        cJSON_AddItemToArray(mods, o);
    }
}
static cJSON *t_scan_modules(const cJSON *a, rbllm_cx_t *cx)
{
    const cJSON *ba = cJSON_GetObjectItem(a, "bus");
    int b0 = cJSON_IsNumber(ba) ? ba->valueint : 0, b1 = cJSON_IsNumber(ba) ? ba->valueint : 1;
    cJSON *r = cJSON_CreateObject();
    cJSON *bl = cJSON_AddArrayToObject(r, "buses");
    bool new50 = false, any = false;
    for (int bus = b0; bus <= b1; bus++) {
        int st = 0;
        cJSON *rep = i2c_do("scan", bus, -1, -1, &st);
        cJSON *bo = cJSON_CreateObject();
        cJSON_AddNumberToObject(bo, "bus", bus);
        if (st != 200 || !rep) {
            const char *m = rep ? cJSON_GetStringValue(cJSON_GetObjectItem(rep, "message")) : NULL;
            bool stopped = !m || !m[0] || !strcmp(m, "ESP_ERR_INVALID_STATE");   /* no pins: the bus was never started */
            cJSON_AddBoolToObject(bo, "running", false);
            cJSON_AddStringToObject(bo, "reason", stopped ? "no pins set: nothing on this bus can be scanned, added or read until set_bus_pins and a restart" : m);
            if (cJSON_IsNumber(ba)) {
                cJSON *rr = stopped ? NULL : rep; if (stopped) cJSON_Delete(rep);
                cJSON_Delete(bo); cJSON_Delete(r);
                if (stopped) { cJSON *fake = cJSON_CreateObject(); cJSON_AddStringToObject(fake, "message", "bus 1 is not running"); return i2c_fail(cx, fake, 409); }
                return i2c_fail(cx, rr, st);
            }
            cJSON_Delete(rep); cJSON_AddItemToArray(bl, bo);
            continue;
        }
        any = true;
        cJSON_AddBoolToObject(bo, "running", true);
        cJSON *dv = cJSON_AddArrayToObject(bo, "found");
        const cJSON *d;
        uint32_t used = 0;   /* 0x50..0x5F taken on this bus: answering now or configured for a module */
        cJSON_ArrayForEach(d, cJSON_GetObjectItem(rep, "devices")) { int x = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(d, "addr")); if (x >= 0x50 && x <= 0x5F) used |= 1u << (x - 0x50); }
        {
            int ist = 0; cJSON *is = i2c_do("state", -1, -1, -1, &ist); const cJSON *im;
            cJSON_ArrayForEach(im, cJSON_GetObjectItem(is, "modules"))
                if ((int)cJSON_GetNumberValue(cJSON_GetObjectItem(im, "bus")) == bus) { int x = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(im, "addr")); if (x >= 0x50 && x <= 0x5F) used |= 1u << (x - 0x50); }
            cJSON_Delete(is);
        }
        for (int x = 0x51; x <= 0x5F; x++) if (!(used & (1u << (x - 0x50)))) { char nf[8]; snprintf(nf, sizeof(nf), "0x%02X", x); cJSON_AddStringToObject(bo, "next_free_address", nf); break; }
        cJSON_ArrayForEach(d, cJSON_GetObjectItem(rep, "devices")) {
            cJSON *o = cJSON_CreateObject();
            int ad = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(d, "addr"));
            char ah[8]; snprintf(ah, sizeof(ah), "0x%02X", ad);
            cJSON_AddStringToObject(o, "address", ah);
            const char *md = cJSON_GetStringValue(cJSON_GetObjectItem(d, "model"));
            if (md) cJSON_AddStringToObject(o, "model", md);
            const char *fw = cJSON_GetStringValue(cJSON_GetObjectItem(d, "fw"));
            if (fw) cJSON_AddStringToObject(o, "fw", fw);
            const cJSON *mi = cJSON_GetObjectItem(d, "module");
            bool known = cJSON_IsNumber(mi) && mi->valueint >= 0;
            if (known) cJSON_AddNumberToObject(o, "module", mi->valueint); else cJSON_AddNullToObject(o, "module");
            cJSON_AddBoolToObject(o, "added", known);
            cJSON_AddBoolToObject(o, "factory_address", ad == 0x50);
            if (!known && ad == 0x50) new50 = true;
            cJSON_AddItemToArray(dv, o);
        }
        cJSON_Delete(rep);
        cJSON_AddItemToArray(bl, bo);
    }
    if (!any && !cJSON_IsNumber(ba)) { cJSON_Delete(r); return fail(cx, "internal", "no I2C bus is running", NULL); }
    if (new50) note(cx, "A new module answers at 0x50: add_module {bus, address 0x50}, then set_module_address to next_free_address before the user connects the next one.");
    note(cx, "An address is unique on its bus; the same address may be used once on each bus. Two modules on one address answer as one: the hub cannot tell them apart, so connect one at a time.");
    cx->as_of = now_s(); cx->resolution = "live";
    return r;
}
static int parse_addr(const cJSON *v)
{
    if (cJSON_IsNumber(v)) return v->valueint;
    const char *s = cJSON_GetStringValue(v);
    if (!s) return -1;
    char *e = NULL; long x = strtol(s, &e, 0);
    return (e && *e == 0) ? (int)x : -1;
}
static cJSON *t_add_module(const cJSON *a, rbllm_cx_t *cx)
{
    int bus = cJSON_GetObjectItem(a, "bus")->valueint, ad = parse_addr(cJSON_GetObjectItem(a, "address"));
    if (ad < 0x08 || ad > 0x77) return fail(cx, "invalid_args", "address: 0x08..0x77, e.g. \"0x50\"", NULL);
    int st = 0;
    cJSON *rep = i2c_do("add", bus, -1, ad, &st);
    if (st != 200 || !rep) return i2c_fail(cx, rep, st);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "added", true);
    modules_view(r, rep);
    cJSON_Delete(rep);
    if (ad == 0x50) note(cx, "The module is on the factory address 0x50: move it now with set_module_address, before the next module is connected on this bus.");
    note(cx, "The module measures nothing until its inputs carry points: list_module_inputs, then create_point for each current input in use.");
    cx->as_of = now_s();
    return r;
}
static cJSON *t_set_module_address(const cJSON *a, rbllm_cx_t *cx)
{
    int m = cJSON_GetObjectItem(a, "module")->valueint, ad = parse_addr(cJSON_GetObjectItem(a, "address"));
    if (ad < 0x08 || ad > 0x77) return fail(cx, "invalid_args", "address: 0x08..0x77, e.g. \"0x51\"", NULL);
    int st = 0;
    cJSON *rep = i2c_do("address", -1, m, ad, &st);
    if (st != 200 || !rep) return i2c_fail(cx, rep, st);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "moved", true);
    modules_view(r, rep);
    cJSON_Delete(rep);
    note(cx, "The address is stored in the module and checked by its unique ID. The next module can be connected now.");
    cx->as_of = now_s();
    return r;
}
static cJSON *t_forget_me(const cJSON *a, rbllm_cx_t *cx)
{
    (void)a;
    if (!cx->token_id || !cx->token_id[0]) return fail(cx, "invalid_args", "only a paired assistant can give its token back", NULL);
    bool hit = false;
    lk();
    for (int i = 0; i < RBLLM_MAX_TOKENS; i++)
        if (S.tok[i].id[0] && !strcmp(S.tok[i].id, cx->token_id)) { memset(&S.tok[i], 0, sizeof(S.tok[i])); hit = true; }
    if (hit) save_locked();
    ulk();
    if (!hit) return fail(cx, "not_found", "this token is not on the hub any more", NULL);
    rbpower_diag_printf("llm: an assistant gave its token back (%s)", cx->client ? cx->client : "?");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "revoked", true);
    note(cx, "This token no longer works; pair again to come back.");
    cx->as_of = now_s();
    return r;
}

/* ---- points on module inputs, bus pins, restart (scope config; operator 23.11: the whole setup over LLM) ---- */
static cJSON *sensors_list(void)
{
    char *sj = rbgrid_core_json_ctx() ? rbgrid_json_create_sensors_list(rbgrid_core_json_ctx()) : NULL;
    cJSON *sl = sj ? cJSON_Parse(sj) : NULL;
    free(sj);
    return sl;
}
static const cJSON *sl_module(const cJSON *sl, int m)
{
    const cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(sl, "modules")) if ((int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "module")) == m) return o;
    return NULL;
}
/* "selectable" (BASIC SCT-: the user picks the clamp model), "fixed" (clamps calibrated with the module, STD or CT-),
 * "uncalibrated" (BASIC without a calibration class). */
static const char *clamp_kind(const cJSON *mo)
{
    if (!mo) return "fixed";
    if (cJSON_IsTrue(cJSON_GetObjectItem(mo, "ct_settable"))) return "selectable";
    const char *ln = cJSON_GetStringValue(cJSON_GetObjectItem(mo, "line"));
    if (ln && !strcmp(ln, "basic") && cJSON_IsTrue(cJSON_GetObjectItem(mo, "uncalibrated"))) return "uncalibrated";
    return "fixed";
}
static const cJSON *sl_input(const cJSON *sl, int m, const char *label)
{
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(sl, "sensors")) {
        const char *ifc = cJSON_GetStringValue(cJSON_GetObjectItem(e, "iface")), *lb = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ch_label"));
        if (!ifc || strcmp(ifc, "i2c") || (int)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "module")) != m || !lb || !label) continue;
        if (!strcasecmp(lb, label)) return e;
        if (!strcmp(lb, "L1–L3") && (!strcasecmp(label, "L1-L3") || !strcasecmp(label, "L1–L3") || !strcasecmp(label, "phases"))) return e;
    }
    return NULL;
}
static int ct_code_of(const char *shortname)
{
    int code = -1;
    cJSON *cm = ct_models(); const cJSON *x;
    cJSON_ArrayForEach(x, cm) { char sh[24]; ct_short(x->valuestring, sh); if (shortname && !strcmp(sh, shortname)) code = atoi(x->string); }
    cJSON_Delete(cm);
    return code;
}
static void llm_source(const rbllm_cx_t *cx, char *src, size_t n)
{
    if (cx->internal && cx->client && !strcmp(cx->client, RBLLM_CLIENT_WEB)) strlcpy(src, "web", n);
    else snprintf(src, n, "llm:%.40s", cx->client && cx->client[0] ? cx->client : "client");
}

static cJSON *t_list_module_inputs(const cJSON *a, rbllm_cx_t *cx)
{
    const cJSON *fm = cJSON_GetObjectItem(a, "module");
    cJSON *sl = sensors_list();
    if (!sl) { cx->retry_ms = 5000; return fail(cx, "busy", "the module list is not ready yet", "Retry in a few seconds."); }
    int ist = 0;
    cJSON *st = i2c_do("state", -1, -1, -1, &ist);
    cJSON *r = cJSON_CreateObject();
    cJSON *mods = cJSON_AddArrayToObject(r, "modules");
    bool any_sel = false, any_free = false, hit = false;
    const cJSON *mo;
    cJSON_ArrayForEach(mo, cJSON_GetObjectItem(sl, "modules")) {
        int m = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(mo, "module"));
        if (cJSON_IsNumber(fm) && fm->valueint != m) continue;
        const cJSON *im = NULL, *x;
        cJSON_ArrayForEach(x, cJSON_GetObjectItem(st, "modules")) if ((int)cJSON_GetNumberValue(cJSON_GetObjectItem(x, "module")) == m) im = x;
        if (!im) continue;   /* a descriptor of a module no longer on the hub */
        hit = true;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "module", m);
        cJSON_AddNumberToObject(o, "bus", cJSON_GetNumberValue(cJSON_GetObjectItem(im, "bus")));
        char ad[8]; snprintf(ad, sizeof(ad), "0x%02X", (int)cJSON_GetNumberValue(cJSON_GetObjectItem(im, "addr")));
        cJSON_AddStringToObject(o, "address", ad);
        const char *md = cJSON_GetStringValue(cJSON_GetObjectItem(mo, "model"));
        cJSON_AddStringToObject(o, "model", md ? md : "");
        const char *ln = cJSON_GetStringValue(cJSON_GetObjectItem(mo, "line"));
        cJSON_AddStringToObject(o, "series", ln && !strcmp(ln, "std") ? "STD" : "BASIC");
        {   /* the module's clamp family: 1 = split-core SCT-013 clamps, 2 = wired CTs; null = not reported */
            rbpower_module_desc_t md2;
            if (rbpower_get_module_desc((uint8_t)m, &md2) && md2.sensor_class != 0 && md2.sensor_class != 0xFF) cJSON_AddNumberToObject(o, "sensor_class", md2.sensor_class);
            else cJSON_AddNullToObject(o, "sensor_class");
        }
        cJSON_AddBoolToObject(o, "online", cJSON_IsTrue(cJSON_GetObjectItem(mo, "present")));
        const char *ck = clamp_kind(mo);
        cJSON_AddStringToObject(o, "clamp", ck);
        if (!strcmp(ck, "selectable")) any_sel = true;
        cJSON *ins = cJSON_AddArrayToObject(o, "inputs");
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(sl, "sensors")) {
            const char *ifc = cJSON_GetStringValue(cJSON_GetObjectItem(e, "iface"));
            if (!ifc || strcmp(ifc, "i2c") || (int)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "module")) != m) continue;
            const char *lb = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ch_label"));
            cJSON *io = cJSON_CreateObject();
            cJSON_AddStringToObject(io, "input", lb ? lb : "?");
            bool volt = lb && !strcmp(lb, "V");
            cJSON_AddStringToObject(io, "kind", volt ? "voltage" : lb && !strcmp(lb, "L1–L3") ? "three_phase" : "current");
            const char *uid = cJSON_GetStringValue(cJSON_GetObjectItem(e, "unit_id"));
            /* a voltage input feeds the hub's voltage reference (power = V x I for this module's channels), not a point of its
             * own: list_channels does not list it, so no point id is shown for it */
            if (volt) { cJSON_AddNullToObject(io, "point"); cJSON_AddBoolToObject(io, "voltage_reference", uid != NULL); }
            else if (uid) cJSON_AddStringToObject(io, "point", uid); else { cJSON_AddNullToObject(io, "point"); any_free = true; }
            const char *cn = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ct_name"));
            if (cn && !volt) { char sh[24]; ct_short(cn, sh); cJSON_AddStringToObject(io, "ct_model", sh); }
            double ra = cJSON_GetNumberValue(cJSON_GetObjectItem(e, "ct_rated_a"));
            if (ra > 0 && !volt) cJSON_AddNumberToObject(io, "ct_rating_a", ra);
            cJSON_AddItemToArray(ins, io);
        }
        cJSON_AddItemToArray(mods, o);
    }
    cJSON_Delete(st);
    cJSON_Delete(sl);
    if (cJSON_IsNumber(fm) && !hit) { cJSON_Delete(r); return fail(cx, "not_found", "no such module on the hub", "get_health lists the modules."); }
    if (any_sel) {
        cJSON *cl = cJSON_AddArrayToObject(r, "ct_models");
        cJSON *cm = ct_models(); const cJSON *x;
        cJSON_ArrayForEach(x, cm) { char sh[24]; ct_short(x->valuestring, sh); cJSON_AddItemToArray(cl, cJSON_CreateString(sh)); }
        cJSON_Delete(cm);
        note(cx, "clamp=selectable: ask which clamp is on each current input (the model is printed on it) and pass it as ct_model.");
    }
    note(cx, "clamp=fixed: the clamps are calibrated together with the module; there is no model to choose, never offer one.");
    if (any_free) note(cx, "An input with point=null measures nothing yet: create_point puts a point on it.");
    cx->as_of = now_s(); cx->resolution = "live";
    return r;
}

static cJSON *t_create_point(const cJSON *a, rbllm_cx_t *cx)
{
    int m = cJSON_GetObjectItem(a, "module")->valueint;
    const char *in = cJSON_GetStringValue(cJSON_GetObjectItem(a, "input"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(a, "name"));
    const char *rs = cJSON_GetStringValue(cJSON_GetObjectItem(a, "role"));
    const char *ctm = cJSON_GetStringValue(cJSON_GetObjectItem(a, "ct_model"));
    if (!valid_name(name)) return fail(cx, "invalid_args", "name: 1..31 bytes of UTF-8 text, no control characters", NULL);
    int role = rs ? role_from_name(rs) : RBPOWER_ROLE_DEVICE_LOAD;
    if (role < 0) return fail(cx, "invalid_args", "role: one of the roles in get_config_schema {section: channels}", NULL);
    if (role == RBPOWER_ROLE_MAIN_SUPPLY || role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY)
        return fail(cx, "invalid_args", "the supply point already exists on the hub; it is not created again",
                    "Put the existing main_supply point on the input with the grid clamp: move_point.");
    cJSON *sl = sensors_list();
    if (!sl) { cx->retry_ms = 5000; return fail(cx, "busy", "the module list is not ready yet", "Retry in a few seconds."); }
    const cJSON *e = sl_input(sl, m, in), *mo = sl_module(sl, m);
    char msg[160];
    if (!e) { snprintf(msg, sizeof(msg), "module %d has no input '%.8s'", m, in ? in : ""); cJSON_Delete(sl); return fail(cx, "not_found", msg, "list_module_inputs shows the inputs."); }
    const char *lb = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ch_label"));
    if (lb && !strcmp(lb, "V")) { cJSON_Delete(sl); return fail(cx, "invalid_args", "the voltage input feeds the hub's voltage; points go on current inputs", NULL); }
    const char *bound = cJSON_GetStringValue(cJSON_GetObjectItem(e, "unit_id"));
    if (bound) { snprintf(msg, sizeof(msg), "input %s of module %d already carries point %s", lb, m, bound); cJSON_Delete(sl); return fail(cx, "conflict", msg, "Rename that point with propose_config, or pick a free input."); }
    int code = -1;
    if (!ctm && !strcmp(clamp_kind(mo), "selectable")) {   /* a guessed rating would mis-book every watt from now on, with no recount */
        cJSON_Delete(sl);
        return fail(cx, "invalid_args", "ct_model: this module takes clamps of several ratings; the model printed on the clamp is needed",
                    "Ask the user to read the model on the clamp (e.g. SCT-013-030) and pass it as ct_model; list_module_inputs lists ct_models.");
    }
    if (ctm) {
        if (strcmp(clamp_kind(mo), "selectable")) { cJSON_Delete(sl); return fail(cx, "invalid_args", "this module's clamps are calibrated with it: no clamp model to choose", "Leave ct_model out."); }
        code = ct_code_of(ctm);
        if (code < 0) { cJSON_Delete(sl); return fail(cx, "invalid_args", "ct_model: one of ct_models from list_module_inputs (e.g. SCT-013-030)", NULL); }
    }
    cJSON *req = cJSON_CreateObject();
    cJSON *sn = cJSON_AddObjectToObject(req, "sensor");
    cJSON_AddStringToObject(sn, "iface", "i2c");
    cJSON_AddNumberToObject(sn, "bus", cJSON_GetNumberValue(cJSON_GetObjectItem(e, "bus")));
    cJSON_AddNumberToObject(sn, "module", m);
    cJSON_AddNumberToObject(sn, "ch", cJSON_GetNumberValue(cJSON_GetObjectItem(e, "ch")));
    cJSON_AddStringToObject(req, "name", name);
    char up[24]; strlcpy(up, role_name((uint8_t)role), sizeof(up)); for (char *q = up; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
    cJSON_AddStringToObject(req, "role", up);
    cJSON_Delete(sl);
    char *js = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    int st = 0;
    char *rep = js ? rbgrid_units_create(js, &st) : NULL;
    free(js);
    cJSON *rj = rep ? cJSON_Parse(rep) : NULL;
    free(rep);
    if (st != 201) {
        const char *em = rj ? cJSON_GetStringValue(cJSON_GetObjectItem(rj, "message")) : NULL;
        snprintf(msg, sizeof(msg), "the hub refused the point (%d: %.100s)", st, em ? em : "?");
        cJSON_Delete(rj);
        return fail(cx, st == 409 ? "conflict" : st == 507 ? "conflict" : "invalid_args", msg, st == 507 ? "The hub holds no more points." : NULL);
    }
    char id[8]; strlcpy(id, cJSON_GetStringValue(cJSON_GetObjectItem(rj, "unit_id")) ? cJSON_GetStringValue(cJSON_GetObjectItem(rj, "unit_id")) : "", sizeof(id));
    cJSON_Delete(rj);
    bool ct_ok = true;
    if (code >= 0) {
        unsigned uid = 0; sscanf(id, "%4x", &uid);
        char b[32]; snprintf(b, sizeof(b), "{\"ct_code\":%d}", code);
        int s2 = 0; char *r2 = rbgrid_units_mutate((rbpower_unit_id_t)uid, "", b, &s2);
        free(r2);
        ct_ok = s2 == 200;
    }
    char src[48]; llm_source(cx, src, sizeof(src));
    char sm[120]; snprintf(sm, sizeof(sm), "point %s created: %.40s (module %d %s)", id, name, m, lb ? lb : "");
    int v = ver_sync(src, sm);
    s_cfg_changed = true;
    rbpower_diag_printf("llm: %s by '%s'", sm, cx->client ? cx->client : "?");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "created", true);
    cJSON *c = cJSON_AddObjectToObject(r, "channel");
    cJSON_AddStringToObject(c, "id", id);
    cJSON_AddStringToObject(c, "name", name);
    {   /* the slug the hub derived from the name (MQTT topics, HA entity ids); set_point_slug changes it */
        unsigned u2 = 0; const rbpower_unit_definition_t *d2 = sscanf(id, "%4x", &u2) == 1 ? rbpower_home_get_unit_definition((rbpower_unit_id_t)u2) : NULL;
        cJSON_AddStringToObject(c, "point", d2 ? d2->point : "");
    }
    cJSON_AddStringToObject(c, "role", role_name((uint8_t)role));
    cJSON_AddNumberToObject(c, "module", m);
    cJSON_AddStringToObject(c, "input", lb ? lb : "");
    if (code >= 0 && ct_ok) cJSON_AddStringToObject(c, "ct_model", ctm);
    cJSON_AddNumberToObject(r, "version", v);
    if (!ct_ok) note(cx, "The point was created, but the module did not take the clamp model: set it with propose_config {channels:{<id>:{ct_model}}}.");
    else if (code >= 0) note(cx, "The module restarts to take the clamp model: a few seconds without data on it.");
    note(cx, "Live values appear within a few seconds (read_live); hours fill as they close.");
    cx->as_of = now_s();
    return r;
}

static cJSON *t_move_point(const cJSON *a, rbllm_cx_t *cx)
{
    const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(a, "channel"));
    int m = cJSON_GetObjectItem(a, "module")->valueint;
    const char *in = cJSON_GetStringValue(cJSON_GetObjectItem(a, "input"));
    unsigned uid = 0;
    if (!cid || sscanf(cid, "%4x", &uid) != 1 || !uid) return fail(cx, "invalid_args", "channel: a channel id from list_channels", NULL);
    cJSON *sl = sensors_list();
    if (!sl) { cx->retry_ms = 5000; return fail(cx, "busy", "the module list is not ready yet", "Retry in a few seconds."); }
    const cJSON *e = sl_input(sl, m, in);
    char msg[160];
    if (!e) { snprintf(msg, sizeof(msg), "module %d has no input '%.8s'", m, in ? in : ""); cJSON_Delete(sl); return fail(cx, "not_found", msg, "list_module_inputs shows the inputs."); }
    const char *lb = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ch_label"));
    if (lb && !strcmp(lb, "V")) { cJSON_Delete(sl); return fail(cx, "invalid_args", "a point goes on a current input, not on V", NULL); }
    const char *bound = cJSON_GetStringValue(cJSON_GetObjectItem(e, "unit_id"));
    if (bound) { snprintf(msg, sizeof(msg), "input %s of module %d already carries point %s", lb, m, bound); cJSON_Delete(sl); return fail(cx, "conflict", msg, NULL); }
    char b[48]; snprintf(b, sizeof(b), "{\"module\":%d,\"ch\":%d}", m, (int)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "ch")));
    char lbs[12]; strlcpy(lbs, lb ? lb : "", sizeof(lbs));
    cJSON_Delete(sl);
    int st = 0;
    char *rep = rbgrid_units_mutate((rbpower_unit_id_t)uid, "rebind", b, &st);
    cJSON *rj = rep ? cJSON_Parse(rep) : NULL;
    free(rep);
    if (st != 200) {
        const char *em = rj ? cJSON_GetStringValue(cJSON_GetObjectItem(rj, "message")) : NULL;
        snprintf(msg, sizeof(msg), "the hub refused the move (%d: %.100s)", st, em ? em : "?");
        cJSON_Delete(rj);
        return fail(cx, st == 404 ? "not_found" : st == 409 ? "conflict" : "invalid_args", msg, NULL);
    }
    bool reb = cJSON_IsTrue(cJSON_GetObjectItem(rj, "requires_reboot"));
    cJSON_Delete(rj);
    char src[48]; llm_source(cx, src, sizeof(src));
    char sm[120]; snprintf(sm, sizeof(sm), "point %04X moved to module %d %s", uid, m, lbs);
    ver_sync(src, sm);
    s_cfg_changed = true;
    rbpower_diag_printf("llm: %s by '%s'", sm, cx->client ? cx->client : "?");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "moved", true);
    cJSON *c = cJSON_AddObjectToObject(r, "channel");
    char h[5]; snprintf(h, sizeof(h), "%04X", uid);
    cJSON_AddStringToObject(c, "id", h);
    cJSON_AddNumberToObject(c, "module", m);
    cJSON_AddStringToObject(c, "input", lbs);
    cJSON_AddBoolToObject(r, "restart_required", reb);
    note(cx, "The point keeps its id, name, role and history; from now on it measures the new input.");
    cx->as_of = now_s();
    return r;
}

static cJSON *t_set_bus_pins(const cJSON *a, rbllm_cx_t *cx)
{
    if (!s_i2c_action) return fail(cx, "internal", "module setup is not available on this hub", NULL);
    int bus = cJSON_GetObjectItem(a, "bus")->valueint;
    bool off = cJSON_IsTrue(cJSON_GetObjectItem(a, "off"));
    const cJSON *sda = cJSON_GetObjectItem(a, "sda"), *scl = cJSON_GetObjectItem(a, "scl");
    if (bus != 1) return fail(cx, "invalid_args", "bus: 1 (bus 0 is fixed by the board)", NULL);
    if (!off && (!cJSON_IsNumber(sda) || !cJSON_IsNumber(scl))) return fail(cx, "invalid_args", "sda and scl: GPIO numbers from pins_ok in get_health, or off:true", NULL);
    cJSON *q = cJSON_CreateObject();
    cJSON_AddStringToObject(q, "op", "pins");
    cJSON_AddNumberToObject(q, "bus", bus);
    if (off) cJSON_AddTrueToObject(q, "off");
    else { cJSON_AddNumberToObject(q, "sda", sda->valueint); cJSON_AddNumberToObject(q, "scl", scl->valueint); }
    int st = 500;
    char *out = s_i2c_action(q, &st);
    cJSON_Delete(q);
    cJSON *rep = out ? cJSON_Parse(out) : NULL;
    free(out);
    if (st != 200 || !rep) return i2c_fail(cx, rep, st);
    cJSON_Delete(rep);
    rbpower_diag_printf("llm: bus %d pins %s by '%s'", bus, off ? "off" : "set", cx->client ? cx->client : "?");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "saved", true);
    cJSON_AddNumberToObject(r, "bus", bus);
    if (!off) { cJSON_AddNumberToObject(r, "sda", sda->valueint); cJSON_AddNumberToObject(r, "scl", scl->valueint); }
    cJSON_AddBoolToObject(r, "restart_required", true);
    note(cx, "Saved; the bus starts after a restart. Ask the user, then call restart_hub. After it, scan_modules {bus: 1}.");
    cx->as_of = now_s();
    return r;
}

static void llm_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));   /* the reply is on its way first */
    rbgrid_system_reboot(500);
    vTaskDelete(NULL);
}
static cJSON *t_restart_hub(const cJSON *a, rbllm_cx_t *cx)
{
    (void)a;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t os;
    if (run && esp_ota_get_state_partition(run, &os) == ESP_OK && os == ESP_OTA_IMG_PENDING_VERIFY) {
        cx->retry_ms = 60000;
        return fail(cx, "busy", "the hub is still confirming the firmware installed less than a minute ago", "Retry in about a minute.");
    }
    /* 6144: the orderly reboot (open-hour flush, NVS persist, deinit, esp_restart) overflowed 3072 and ended in a panic reset */
    if (xTaskCreate(llm_restart_task, "llm_restart", 6144, NULL, tskIDLE_PRIORITY + 6, NULL) != pdPASS)
        return fail(cx, "internal", "the restart could not be scheduled", "Restart the hub from its page.");
    rbpower_diag_printf("llm: restart requested by '%s'", cx->client ? cx->client : "?");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "restarting", true);
    cJSON_AddNumberToObject(r, "back_in_s", 30);
    note(cx, "The hub restarts in about 2 s and answers again in about 30 s; the hour in progress is kept.");
    cx->as_of = now_s();
    return r;
}

static cJSON *t_delete_point(const cJSON *a, rbllm_cx_t *cx)
{
    const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(a, "channel"));
    unsigned uid = 0;
    if (!cid || sscanf(cid, "%4x", &uid) != 1 || !uid) return fail(cx, "invalid_args", "channel: a channel id from list_channels", NULL);
    const rbpower_unit_definition_t *def = rbpower_home_get_unit_definition((rbpower_unit_id_t)uid);
    if (!def) return fail(cx, "not_found", "no such channel", "Call list_channels for the ids.");
    if (def->role == RBPOWER_ROLE_MAIN_SUPPLY || def->role == RBPOWER_ROLE_ALTERNATIVE_SUPPLY)
        return fail(cx, "invalid_args", "the supply point is not deleted: the grid figure and the bill depend on it", "Move it to the right input with move_point.");
    char nm[48]; utf8_clean(def->name, nm, sizeof(nm));
    int st = 0;
    char *rep = rbgrid_units_delete((rbpower_unit_id_t)uid, &st);
    cJSON *rj = rep ? cJSON_Parse(rep) : NULL;
    free(rep);
    if (st != 200) {
        const char *em = rj ? cJSON_GetStringValue(cJSON_GetObjectItem(rj, "message")) : NULL;
        char msg[160]; snprintf(msg, sizeof(msg), "the hub refused (%d: %.100s)", st, em ? em : "?");
        cJSON_Delete(rj);
        return fail(cx, st == 404 ? "not_found" : "conflict", msg, st == 409 ? "Points measured inside it go first (propose_config inside), or delete them." : NULL);
    }
    cJSON_Delete(rj);
    char src[48]; llm_source(cx, src, sizeof(src));
    char sm[120]; snprintf(sm, sizeof(sm), "point %04X deleted (%.40s)", uid, nm);
    ver_sync(src, sm);
    s_cfg_changed = true;
    rbpower_diag_printf("llm: %s by '%s'", sm, cx->client ? cx->client : "?");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "deleted", true);
    char h[5]; snprintf(h, sizeof(h), "%04X", uid);
    cJSON_AddStringToObject(r, "channel", h);
    note(cx, "The input is free again (create_point). The point's stored history stays in its slot until the slot is reused; its id is not given to a new point meanwhile.");
    cx->as_of = now_s();
    return r;
}

static cJSON *t_set_module_bus(const cJSON *a, rbllm_cx_t *cx)
{
    int m = cJSON_GetObjectItem(a, "module")->valueint, bus = cJSON_GetObjectItem(a, "bus")->valueint;
    if (!s_i2c_action) return fail(cx, "internal", "module setup is not available on this hub", NULL);
    cJSON *q = cJSON_CreateObject();
    cJSON_AddStringToObject(q, "op", "bus");
    cJSON_AddNumberToObject(q, "module", m);
    cJSON_AddNumberToObject(q, "bus", bus);
    int st = 500;
    char *out = s_i2c_action(q, &st);
    cJSON_Delete(q);
    cJSON *rep = out ? cJSON_Parse(out) : NULL;
    free(out);
    if (st != 200 || !rep) return i2c_fail(cx, rep, st);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "moved", true);
    modules_view(r, rep);
    cJSON_Delete(rep);
    rbpower_diag_printf("llm: module %d set to bus %d by '%s'", m, bus, cx->client ? cx->client : "?");
    note(cx, "The hub now looks for the module on that bus at the same address; its points and history stay.");
    cx->as_of = now_s();
    return r;
}

static cJSON *t_list_nodes(const cJSON *a, rbllm_cx_t *cx)
{
    (void)a;
    cJSON *st = core_cmd("get", "espnow", NULL);
    const cJSON *d = cJSON_GetObjectItem(st, "data");
    if (!st || (d && cJSON_GetStringValue(cJSON_GetObjectItem(d, "result")) && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(d, "result")), "ok"))) {
        cJSON_Delete(st);
        return fail(cx, "not_found", "remote nodes (ESP-NOW) are not part of this hub's firmware", NULL);
    }
    cJSON_DeleteItemFromObject(st, "id");
    note(cx, "Remote nodes are battery or mains boards that measure far from the hub and send over ESP-NOW; their channels show in list_channels like any other.");
    cx->as_of = now_s(); cx->resolution = "live";
    return st;
}

static cJSON *t_node_action(const cJSON *a, rbllm_cx_t *cx)
{
    const char *act = cJSON_GetStringValue(cJSON_GetObjectItem(a, "action"));
    static const char *const OK[] = { "hub_on", "hub_off", "pair", "pair_cancel", "remove", "disable", "enable", "configure" };
    bool known = false;
    for (size_t i = 0; i < sizeof(OK) / sizeof(OK[0]); i++) if (act && !strcmp(act, OK[i])) known = true;
    if (!known) return fail(cx, "invalid_args", "action: hub_on, hub_off, pair, pair_cancel, remove, disable, enable or configure", NULL);
    bool needs_mac = !strcmp(act, "remove") || !strcmp(act, "disable") || !strcmp(act, "enable") || !strcmp(act, "configure");
    if (needs_mac && !cJSON_GetStringValue(cJSON_GetObjectItem(a, "mac"))) return fail(cx, "invalid_args", "mac: the node's MAC from list_nodes", NULL);
    cJSON *ack = core_cmd("set", "espnow", a);
    char m[160];
    bool ok = core_ok(ack, m, sizeof(m));
    cJSON *rep = cJSON_DetachItemFromObject(cJSON_GetObjectItem(ack, "data"), "reply");
    cJSON_Delete(ack);
    if (!ok) { cJSON_Delete(rep); return fail(cx, strstr(m, "switched off") ? "conflict" : "invalid_args", m[0] ? m : "the hub refused the action", NULL); }
    rbpower_diag_printf("llm: node action %s by '%s'", act, cx->client ? cx->client : "?");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "done", true);
    cJSON_AddStringToObject(r, "action", act);
    if (rep) cJSON_AddItemToObject(r, "reply", rep);
    if (!strcmp(act, "pair")) note(cx, "The hub accepts a new node while the window is open: ask the user to power the node now; list_nodes shows it once paired.");
    cx->as_of = now_s();
    return r;
}

static cJSON *t_set_prices(const cJSON *a, rbllm_cx_t *cx)
{
    bool clear = cJSON_IsTrue(cJSON_GetObjectItem(a, "clear"));
    const cJSON *pr = cJSON_GetObjectItem(a, "prices");
    if (!clear && (!cJSON_IsArray(pr) || !cJSON_GetArraySize(pr))) return fail(cx, "invalid_args", "prices: a list of {buy, sell?, from, until}, or clear:true", NULL);
    if (cJSON_GetArraySize(pr) > 192) return fail(cx, "invalid_args", "prices: at most 192 entries per call (two days of 15-min periods)", "Send the rest in another call.");
    cJSON *d = cJSON_CreateObject();
    if (clear) cJSON_AddTrueToObject(d, "clear");
    else {
        cJSON *arr = cJSON_AddArrayToObject(d, "prices");
        const cJSON *e; int k = 0;
        cJSON_ArrayForEach(e, pr) {
            uint32_t f = 0, u = 0;
            const cJSON *jf = cJSON_GetObjectItem(e, "from"), *ju = cJSON_GetObjectItem(e, "until");
            bool okf = cJSON_IsString(jf) ? parse_iso(jf->valuestring, &f) : false, oku = cJSON_IsString(ju) ? parse_iso(ju->valuestring, &u) : false;
            if (!okf || !oku) {
                char m[96]; snprintf(m, sizeof(m), "prices[%d]: from and until as \"YYYY-MM-DDTHH:MM\" (hub time, or with an offset)", k);
                cJSON_Delete(d); return fail(cx, "invalid_args", m, NULL);
            }
            cJSON *o = cJSON_CreateObject();
            const cJSON *jb = cJSON_GetObjectItem(e, "buy"), *js = cJSON_GetObjectItem(e, "sell");
            if (jb) cJSON_AddItemToObject(o, "buy", cJSON_Duplicate(jb, true));
            if (js) cJSON_AddItemToObject(o, "sell", cJSON_Duplicate(js, true));
            cJSON_AddNumberToObject(o, "from", f); cJSON_AddNumberToObject(o, "until", u);
            cJSON_AddItemToArray(arr, o);
            k++;
        }
    }
    cJSON *ack = core_cmd("set", "price", d);
    cJSON_Delete(d);
    char m[160];
    bool ok = core_ok(ack, m, sizeof(m));
    cJSON_Delete(ack);
    if (!ok) return fail(cx, "invalid_args", m[0] ? m : "the hub refused the prices", "Prices are per kWh (-100..100), before fees and VAT; a price per MWh must be divided by 1000.");
    rbpower_diag_printf("llm: prices %s by '%s'", clear ? "cleared" : "stored", cx->client ? cx->client : "?");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "stored", !clear);
    cJSON_AddBoolToObject(r, "cleared", clear);
    if (!clear) cJSON_AddNumberToObject(r, "count", cJSON_GetArraySize(pr));
    cJSON_AddStringToObject(r, "message", m);
    note(cx, clear ? "The tariff zones' prices apply again." : "These prices replace the zone price for the 15-min periods they cover; fees and VAT are added on top. A dynamic price provider, if set, may overwrite them on its next fetch.");
    cx->as_of = now_s();
    return r;
}

static cJSON *t_set_clock(const cJSON *a, rbllm_cx_t *cx)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(a, "time"));
    bool sync = cJSON_IsTrue(cJSON_GetObjectItem(a, "sync"));
    if (!t == !sync) return fail(cx, "invalid_args", "either time (the user's clock, with its offset) or sync:true", NULL);
    uint32_t before = now_s();
    if (t) {
        uint32_t ts = 0;
        if (strlen(t) < 16 || (!strchr(t + 10, '+') && !strchr(t + 10, '-') && !strchr(t, 'Z'))) return fail(cx, "invalid_args", "time: with its UTC offset, e.g. 2026-11-24T14:05:00+01:00", "Ask the user for the time on their clock and their offset; do not use your own idea of the time.");
        if (!parse_iso(t, &ts) || ts < 1700000000u) return fail(cx, "invalid_args", "time: \"YYYY-MM-DDTHH:MM[:SS]±HH:MM\"", NULL);
        if (rbtime_set_manual_time(ts) != RBPOWER_OK) return fail(cx, "internal", "the hub did not take the time", NULL);
    }
    rbpower_err_t se = rbtime_force_sync(RBTIME_SOURCE_SNTP, 5000);   /* an Internet time source refines it when there is one */
    if (!t && se != RBPOWER_OK)
        return fail(cx, "conflict", "no Internet time server answered; the clock is unchanged",
                    "Check the hub's internet access, or ask the user for the time on their clock and call set_clock {time} with its offset.");
    uint32_t after = now_s();
    rbpower_diag_printf("llm: clock %s by '%s' (%+ld s)", t ? "set" : "synced", cx->client ? cx->client : "?", (long)((int64_t)after - before));
    cJSON *r = cJSON_CreateObject();
    add_time(r, "hub_time", after);
    cJSON_AddNumberToObject(r, "changed_by_s", (double)((int64_t)after - (int64_t)before));
    cJSON_AddBoolToObject(r, "internet_time", se == RBPOWER_OK);
    note(cx, se == RBPOWER_OK ? "The hub's clock follows the Internet time source." : "No Internet time source answered; the hub keeps the time given until one does.");
    cx->as_of = after;
    return r;
}

static cJSON *t_set_module_drdy(const cJSON *a, rbllm_cx_t *cx)
{
    if (!s_i2c_action) return fail(cx, "internal", "module setup is not available on this hub", NULL);
    int m = cJSON_GetObjectItem(a, "module")->valueint, g = cJSON_GetObjectItem(a, "gpio")->valueint;
    cJSON *q = cJSON_CreateObject();
    cJSON_AddStringToObject(q, "op", "drdy");
    cJSON_AddNumberToObject(q, "module", m);
    cJSON_AddNumberToObject(q, "gpio", g);
    int st = 500;
    char *out = s_i2c_action(q, &st);
    cJSON_Delete(q);
    cJSON *rep = out ? cJSON_Parse(out) : NULL;
    free(out);
    if (st != 200 || !rep) return i2c_fail(cx, rep, st);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "saved", true);
    modules_view(r, rep);
    cJSON_Delete(rep);
    rbpower_diag_printf("llm: module %d drdy %d by '%s'", m, g, cx->client ? cx->client : "?");
    note(cx, g < 0 ? "The module uses the shared ready line again." : "The module signals 'data ready' on its own GPIO from now on.");
    cx->as_of = now_s();
    return r;
}

static cJSON *t_set_point_slug(const cJSON *a, rbllm_cx_t *cx)
{
    const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(a, "channel"));
    const char *slug = cJSON_GetStringValue(cJSON_GetObjectItem(a, "point"));
    bool reuse = cJSON_IsTrue(cJSON_GetObjectItem(a, "reuse"));
    unsigned uid = 0;
    if (!cid || sscanf(cid, "%4x", &uid) != 1 || !uid) return fail(cx, "invalid_args", "channel: a channel id from list_channels", NULL);
    if (!rbpower_home_get_unit_definition((rbpower_unit_id_t)uid)) return fail(cx, "not_found", "no such channel", "Call list_channels for the ids.");
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "point", slug ? slug : "");
    if (reuse) cJSON_AddTrueToObject(b, "reuse");
    char *bs = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    int st = 0;
    char *rep = bs ? rbgrid_units_mutate((rbpower_unit_id_t)uid, "rename", bs, &st) : NULL;
    free(bs);
    cJSON *rj = rep ? cJSON_Parse(rep) : NULL;
    free(rep);
    if (st != 200) {
        const char *code = rj ? cJSON_GetStringValue(cJSON_GetObjectItem(rj, "code")) : NULL;
        const char *em = rj ? cJSON_GetStringValue(cJSON_GetObjectItem(rj, "message")) : NULL;
        char msg[160]; snprintf(msg, sizeof(msg), "%.120s", em ? em : "the hub refused the slug");
        const char *hint = code && !strcmp(code, "POINT_TOMBSTONED") ? "This slug is retired: a deleted point had it, or a point had it before a rename. reuse:true gives it (and its Home Assistant history) to this point - ask the user first." : NULL;
        cJSON_Delete(rj);
        return fail(cx, st == 409 ? "conflict" : st == 404 ? "not_found" : "invalid_args", msg, hint);
    }
    cJSON_Delete(rj);
    char src[48]; llm_source(cx, src, sizeof(src));
    char sm[120]; snprintf(sm, sizeof(sm), "point %04X slug -> %.40s", uid, slug);
    ver_sync(src, sm);
    s_cfg_changed = true;
    rbpower_diag_printf("llm: %s by '%s'", sm, cx->client ? cx->client : "?");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "renamed", true);
    char h[5]; snprintf(h, sizeof(h), "%04X", uid);
    cJSON_AddStringToObject(r, "channel", h);
    cJSON_AddStringToObject(r, "point", slug);
    note(cx, "The slug names the point in MQTT topics and Home Assistant entity ids; the hub re-announces the entities. Automations that used the old entity id must be updated by the user.");
    cx->as_of = now_s();
    return r;
}

static const rbllm_tool_t TOOLS[] = {
    { "get_hub_info", RBLLM_SCOPE_READ,
      "When to call: first, to learn the hub's time zone, currency, firmware and what it can do. What it returns: hub id, name, firmware, "
      "protocol version, board, hub time with offset, time zone, currency, uptime, capabilities, the installed web app kit (ui_kit) and "
      "its language (ui_language), history (hourly_days, daily_days kept; from = where the oldest stored records start, null if none), "
      "and hub_page_only: what is done on the hub's own page.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"hub_id\":{\"type\":\"string\"},\"fw\":{\"type\":\"string\"},\"timezone\":{\"type\":\"string\"},\"currency\":{\"type\":\"string\"},\"hub_time\":{\"type\":\"string\"},\"ui_kit\":{\"type\":[\"string\",\"null\"]},\"ui_language\":{\"type\":[\"string\",\"null\"]},\"history\":{\"type\":\"object\",\"properties\":{\"hourly_days\":{\"type\":\"integer\"},\"daily_days\":{\"type\":\"integer\"},\"from\":{\"type\":[\"string\",\"null\"]}}}}}",
      t_get_hub_info },
    { "list_channels", RBLLM_SCOPE_READ,
      "When to call: before any per-channel question, to map names to ids. What it returns: every measuring point with id, the user's name, "
      "role (main = grid supply, load, generation, bidirectional), whether its source is present, whether it has a history slot (the hour in progress fills every 15 min as its 15-min periods close; the hour's record is stored when the hour closes), and history_from: where its stored "
      "records start (null = none yet). Before history_from the hub answers null: no data, not zero.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"channels\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"role\":{\"type\":\"string\"},\"present\":{\"type\":\"boolean\"},\"has_history\":{\"type\":\"boolean\"},\"history_from\":{\"type\":[\"string\",\"null\"]}}}}}}",
      t_list_channels },
    { "read_live", RBLLM_SCOPE_READ,
      "When to call: for what is happening right now. What it returns: the grid power (positive = import) and per channel the power in W, "
      "current in A and voltage in V, from the hub's 3-second data. A load channel reading negative power has its clamp facing the wrong "
      "way (on a BASIC module that energy is not booked at all, so the load looks small).",
      "{\"type\":\"object\",\"properties\":{\"channels\":" S_CHANNELS "},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"grid\":{\"type\":\"object\"},\"channels\":{\"type\":\"array\"}}}",
      t_read_live },
    { "get_health", RBLLM_SCOPE_READ,
      "When to call: when data looks wrong or missing, or the user asks whether the hub is fine. What it returns: uptime, last reset reason, "
      "clock sync, free memory, the metering modules with model, firmware and online/missing status, channels present.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"uptime_s\":{\"type\":\"number\"},\"modules\":{\"type\":\"array\"},\"modules_online\":{\"type\":\"integer\"}}}",
      t_get_health },
    { "identify_channel", RBLLM_SCOPE_READ,
      "When to call: to find which channel an appliance is on (setting up, naming channels, checking a clamp). Two steps: action start "
      "(takes the baseline and samples every second for window_s), then the user switches one appliance on or off, then action result. "
      "What it returns: verdict identified | ambiguous | no_step, channel_id, candidates with their power step, the grid's step, and "
      "reversed_ct_suspected when a load channel moved against the grid. Read-only: it measures, it changes nothing.",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"result\"]},"
      "\"window_s\":{\"type\":\"integer\",\"minimum\":10,\"maximum\":120,\"description\":\"start only; default 60\"}},\"required\":[\"action\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"},\"verdict\":{\"type\":\"string\"},\"channel_id\":{\"type\":\"string\"},\"candidates\":{\"type\":\"array\"},\"grid\":{\"type\":\"object\"}}}",
      t_identify_channel },
    { "get_events", RBLLM_SCOPE_READ,
      "When to call: to explain a gap, a missing module, a restart or a warning, or to see what changed since you last looked. What it "
      "returns: typed events kept across restarts (reboot, config_changed, module_lost, module_back, clock_unsynced, price_missing), oldest "
      "first, with a cursor (next) to pass as since next time. source \"log\" gives the hub's raw log lines of this boot instead.",
      "{\"type\":\"object\",\"properties\":{\"since\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"next from the previous call; omit for the oldest kept\"},"
      "\"source\":{\"type\":\"string\",\"enum\":[\"events\",\"log\"],\"description\":\"default events\"},"
      "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":50}},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"events\":{\"type\":\"array\"},\"next\":{\"type\":\"integer\"},\"more\":{\"type\":\"boolean\"}}}",
      t_get_events },
    { "get_energy_summary", RBLLM_SCOPE_READ,
      "When to call: for how much energy was used or exported in a period, per channel, as a table. What it returns: per channel rows of "
      "{from, energy_kwh (import/consumption), export_kwh} at the resolution used, and totals. Hourly data is kept 60 days, daily 400 days.",
      "{\"type\":\"object\",\"properties\":{\"period\":" S_PERIOD ",\"resolution\":{\"type\":\"string\",\"enum\":[\"hour\",\"day\",\"week\",\"month\",\"year\",\"total\"]},"
      "\"channels\":" S_CHANNELS "},\"required\":[\"period\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"resolution\":{\"type\":\"string\"},\"channels\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"rows\":{\"type\":\"array\"},\"total\":{\"type\":\"object\"}}}}}}",
      t_get_energy_summary },
    { "get_cost_breakdown", RBLLM_SCOPE_READ,
      "When to call: for what electricity cost in a period. What it returns: total_cost = the grid import cost of the main supply (the bill "
      "figure, gross, no fixed charges), export revenue, net cost, the currency, cost per other channel (attribution, does not add up to the "
      "bill) and the price in force now.",
      "{\"type\":\"object\",\"properties\":{\"period\":" S_PERIOD ",\"channels\":" S_CHANNELS "},\"required\":[\"period\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"total_cost\":{\"type\":[\"number\",\"null\"]},\"currency\":{\"type\":\"string\"},\"by_channel\":{\"type\":\"array\"}}}",
      t_get_cost_breakdown },
    { "compare_periods", RBLLM_SCOPE_READ,
      "When to call: for 'more or less than before'. What it returns: per channel the energy of period a and period b, the difference in kWh "
      "and percent, and the cost difference; and the other direction (a_export_kwh, b_export_kwh, delta_export_kwh, delta_revenue): what a "
      "generation point produced, or the grid feed exported. Give b, or a baseline relative to a.",
      "{\"type\":\"object\",\"properties\":{\"a\":" S_PERIOD ",\"b\":" S_PERIOD ",\"baseline\":{\"type\":\"string\",\"enum\":[\"previous_period\",\"previous_month\",\"same_month_last_year\"],\"description\":\"previous_period: as long as a, just before it; previous_month: the same calendar dates one month earlier; same_month_last_year: the same dates a year earlier\"},"
      "\"channels\":" S_CHANNELS "},\"required\":[\"a\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"channels\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"delta_kwh\":{\"type\":\"number\"},\"delta_pct\":{\"type\":[\"number\",\"null\"]},\"delta_export_kwh\":{\"type\":[\"number\",\"null\"]},\"delta_revenue\":{\"type\":[\"number\",\"null\"]}}}}}}",
      t_compare_periods },
    { "get_load_profile", RBLLM_SCOPE_READ,
      "When to call: for the shape of a period on one channel (which hours or days used more, a standby floor). What it returns: rows "
      "of {from, avg_w, min_w, max_w} per stored hour or day. avg_w is the average power of the step; min_w/max_w are null on points fed "
      "by measuring modules (their records carry no extremes), so judge peaks and standby from avg_w.",
      "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"string\",\"description\":\"Channel id from list_channels.\"},\"period\":" S_PERIOD ","
      "\"resolution\":{\"type\":\"string\",\"enum\":[\"hour\",\"day\"]}},\"required\":[\"channel\",\"period\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"resolution\":{\"type\":\"string\"},\"rows\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"string\"},\"avg_w\":{\"type\":\"number\"},\"min_w\":{\"type\":[\"number\",\"null\"]},\"max_w\":{\"type\":[\"number\",\"null\"]}}}}}}",
      t_get_load_profile },
    { "get_import_export_balance", RBLLM_SCOPE_READ,
      "When to call: for how much came from the grid, went back to it, and (with solar or an inverter) how much of the production was used "
      "at home. What it returns: import, export, generation and self-consumption in kWh, and the self-consumption and self-sufficiency "
      "shares (0..1); null where the needed point is not configured.",
      "{\"type\":\"object\",\"properties\":{\"period\":" S_PERIOD "},\"required\":[\"period\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"import_kwh\":{\"type\":[\"number\",\"null\"]},\"export_kwh\":{\"type\":[\"number\",\"null\"]},\"generation_kwh\":{\"type\":[\"number\",\"null\"]}}}",
      t_get_import_export_balance },
    { "check_installation", RBLLM_SCOPE_READ,
      "When to call: after setup, or when numbers look wrong. What it returns: findings {type, severity, channel, evidence, fix_hint}: "
      "no_main_supply, channel_absent, channel_no_history, module_missing, module_without_points, module_uncalibrated, "
      "module_on_factory_address, no_voltage, ct_reversed, ct_rating_mismatch, sum_mismatch, generation_not_configured, generation_reversed, clock_unsynced, "
      "timezone_not_set, timezone_implausible, tariff_not_set, ap_password_default, unexpected_restart. Detects only; a reversed clamp is fixed at the clamp.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"findings\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\"},\"severity\":{\"type\":\"string\"},\"fix_hint\":{\"type\":\"string\"}}}}}}",
      t_check_installation },
    { "detect_anomalies", RBLLM_SCOPE_READ,
      "When to call: for 'anything unusual?' or 'why is my bill higher'. What it returns: heuristic findings {type: night_baseline_rise | "
      "unusual_day, channel, evidence, confidence} from the stored hours and days. They are hints, not measured faults.",
      "{\"type\":\"object\",\"properties\":{\"channels\":" S_CHANNELS "},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"findings\":{\"type\":\"array\"},\"checks_with_enough_data\":{\"type\":\"integer\"}}}",
      t_detect_anomalies },
    { "get_regional_context", RBLLM_SCOPE_READ,
      "When to call: when the user asks about dynamic or spot prices for their country. What it returns: the dynamic price sources the "
      "hub can use for that country (zones, currency, whether a token is needed), the hub currency and the price state now.",
      "{\"type\":\"object\",\"properties\":{\"country\":{\"type\":\"string\",\"description\":\"ISO 3166-1 alpha-2, e.g. DE; omit for the list of countries\"}},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"dynamic_price_sources\":{\"type\":\"array\"},\"hub_currency\":{\"type\":\"string\"}}}",
      t_get_regional_context },
    { "get_config", RBLLM_SCOPE_CONFIG,
      "When to call: to explain how the hub is set up (points with their clamps, short names, phases, ratings and EV thresholds, tariff, price rules, time zone, hub name, MQTT, dynamic price) before advising a change. "
      "What it returns: the chosen section(s) as a compact view plus the config version number; passwords masked. dynamic_price only when asked by section.",
      "{\"type\":\"object\",\"properties\":{\"section\":{\"type\":\"string\",\"enum\":[\"channels\",\"tariff\",\"notifications\",\"region\",\"hub\",\"integrations\",\"dynamic_price\"]}},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"version\":{\"type\":\"integer\"},\"channels\":{\"type\":\"array\"},\"tariff\":{\"type\":\"object\"}}}",
      t_get_config },
    { "get_config_schema", RBLLM_SCOPE_CONFIG,
      "When to call: before propose_config, to learn which fields of a section can be changed and their allowed values (roles, clamp "
      "models). What it returns: the writable fields with type, limits, enums and descriptions.",
      "{\"type\":\"object\",\"properties\":{\"section\":{\"type\":\"string\",\"enum\":[\"channels\",\"notifications\",\"tariff\",\"region\",\"hub\",\"integrations\",\"dynamic_price\"]}},\"required\":[\"section\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"writable\":{\"type\":\"object\"}}}",
      t_get_config_schema },
    { "propose_config", RBLLM_SCOPE_CONFIG,
      "When to call: for any settings change: channel names, roles, parents, clamp models and circuit data; the tariff (prices, VAT, fees, zones with "
      "windows and weekdays, default zone); price rules and notifications; the time zone and mains voltage/frequency; the hub's name and "
      "supply type; the MQTT broker and telemetry format; the dynamic price provider; or to undo a change. What it returns: the diff, warnings, restart_required and a one-time confirm_token (10 min); no_change true and no token when the hub already has those values. Changes "
      "nothing; show the diff and ask the user.",
      "{\"type\":\"object\",\"properties\":{\"patch\":{\"type\":\"object\",\"description\":\"{channels:{<id>:{name?,short_name?,role?,inside?,ct_model?,phase?,rated_power_w?,max_current_a?,charge_threshold_w?,idle_timeout_min?}}, "
      "tariff:{plan_name?,provider?,currency?,vat_percent?,energy_tax_per_kwh?,grid_fee_per_kwh?,levy_per_kwh?,default_zone?,prices_from_bill?,zones?:{<zone>:{buy_per_kwh?,sell_per_kwh?,window?,days?,priority?,enabled?}}}, "
      "peaks_cycle_day?, peaks_reset?, price_rules?:[...], price_signal?:{...}, region?:{timezone?,nominal_voltage?,nominal_frequency?}, hub?:{name?,location?,grid_system?}, mqtt?:{broker_uri}, telemetry?:{profile?,frame?}, dynamic_price?:{...}}; get_config_schema per section\"},"
      "\"rollback_to\":{\"type\":\"integer\",\"description\":\"a version from list_config_versions\"}},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"diff\":{\"type\":\"array\"},\"warnings\":{\"type\":\"array\"},\"restart_required\":{\"type\":\"boolean\"},\"confirm_token\":{\"type\":\"string\"},\"expires_in\":{\"type\":\"integer\"},\"base_version\":{\"type\":\"integer\"}}}",
      t_propose_config },
    { "apply_config", RBLLM_SCOPE_CONFIG,
      "When to call: only after the user said yes to a propose_config diff. What it returns: the new version number, or version null with "
      "versioned false when nothing versioned changed (then no rollback undoes it; the note names what). Fails with conflict "
      "if the settings changed since the proposal; nothing is half applied.",
      "{\"type\":\"object\",\"properties\":{\"confirm_token\":{\"type\":\"string\"}},\"required\":[\"confirm_token\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"version\":{\"type\":[\"integer\",\"null\"]},\"versioned\":{\"type\":\"boolean\"},\"applied\":{\"type\":\"boolean\"},\"restart_required\":{\"type\":\"boolean\"}}}",
      t_apply_config },
    { "list_config_versions", RBLLM_SCOPE_CONFIG,
      "When to call: when the user asks what changed, or before a rollback. What it returns: the last 5 versions, newest first, with time, "
      "source (hub, llm:<client>) and a summary.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"versions\":{\"type\":\"array\"}}}",
      t_list_config_versions },
    { "rollback_config", RBLLM_SCOPE_CONFIG,
      "When to call: to finish an undo the user confirmed. Takes the confirm_token from propose_config {rollback_to}. What it returns: "
      "as apply_config. It restores names, roles, inside, clamp models, tariff and peaks cycle day; modules, addresses, points and bus pins stay as they are.",
      "{\"type\":\"object\",\"properties\":{\"version\":{\"type\":\"integer\"},\"confirm_token\":{\"type\":\"string\"}},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"version\":{\"type\":\"integer\"},\"applied\":{\"type\":\"boolean\"}}}",
      t_rollback_config },
    { "scan_modules", RBLLM_SCOPE_READ,
      "When to call: while setting up hardware, after the user connects a metering module, to see what answers on an I2C bus. "
      "What it returns: every address that answers on the bus, the model, and whether it is already added (module number) or new.",
      "{\"type\":\"object\",\"properties\":{\"bus\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1,\"description\":\"one I2C bus; omit for both\"}},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"buses\":{\"type\":\"array\"}}}",
      t_scan_modules },
    { "add_module", RBLLM_SCOPE_CONFIG,
      "When to call: when scan_modules shows a new module (added=false). What it does: adds it to the hub on that bus and address "
      "(its inputs then show in list_module_inputs, without points). New modules arrive at 0x50: move it with set_module_address before the next one is connected.",
      "{\"type\":\"object\",\"properties\":{\"bus\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1},\"address\":{\"type\":\"string\",\"description\":\"hex, e.g. 0x50\"}},\"required\":[\"bus\",\"address\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"added\":{\"type\":\"boolean\"},\"modules\":{\"type\":\"array\"}}}",
      t_add_module },
    { "set_module_address", RBLLM_SCOPE_CONFIG,
      "When to call: right after adding a module that sits at 0x50, or to resolve an address clash. What it does: writes a new I2C "
      "address into the module (stored in the module, checked by its unique ID). The address must be free on that bus.",
      "{\"type\":\"object\",\"properties\":{\"module\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":7},\"address\":{\"type\":\"string\",\"description\":\"hex 0x08..0x77, e.g. 0x51\"}},\"required\":[\"module\",\"address\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"moved\":{\"type\":\"boolean\"},\"modules\":{\"type\":\"array\"}}}",
      t_set_module_address },
    { "list_module_inputs", RBLLM_SCOPE_READ,
      "When to call: after add_module, and before create_point or a clamp-model change. What it returns: per module its series, model, "
      "bus and address, the clamp kind (selectable = the user picks the clamp model; fixed = calibrated with the module, nothing to pick) "
      "and every input as marked on the module (I0, I1 ..., V) with the point on it or null, plus the clamp models to choose from. A V input "
      "is the voltage reference for the module's channels (voltage_reference true when in use), never a point.",
      "{\"type\":\"object\",\"properties\":{\"module\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":7,\"description\":\"one module; omit for all\"}},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"modules\":{\"type\":\"array\"},\"ct_models\":{\"type\":\"array\"}}}",
      t_list_module_inputs },
    { "create_point", RBLLM_SCOPE_CONFIG,
      "When to call: for each current input in use that has no point (list_module_inputs: point=null), after asking the user what it "
      "measures. What it does: creates a metering point on that input with a name, a role (default device_load) and, for clamp=selectable, "
      "the clamp model. The supply point already exists: put it on the right input with move_point.",
      "{\"type\":\"object\",\"properties\":{\"module\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":7},\"input\":{\"type\":\"string\",\"description\":\"as list_module_inputs shows it, e.g. I0\"},"
      "\"name\":{\"type\":\"string\",\"maxLength\":31},\"role\":{\"type\":\"string\",\"enum\":[\"circuit_breaker\",\"device_load\",\"zone_load\",\"renewable_source\",\"storage_system\",\"ev_charging\",\"heating_cooling\",\"smart_appliance\"]},"
      "\"ct_model\":{\"type\":\"string\",\"description\":\"only for clamp=selectable, one of ct_models\"}},\"required\":[\"module\",\"input\",\"name\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"created\":{\"type\":\"boolean\"},\"channel\":{\"type\":\"object\"},\"version\":{\"type\":\"integer\"}}}",
      t_create_point },
    { "move_point", RBLLM_SCOPE_CONFIG,
      "When to call: when a point sits on the wrong input, typically the supply (main_supply) point, which the hub starts with on module 0 "
      "input I0: ask the user which input carries the grid clamp. What it does: moves the point to a free input; id, name, role and history stay.",
      "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"string\",\"description\":\"id from list_channels\"},\"module\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":7},"
      "\"input\":{\"type\":\"string\",\"description\":\"e.g. I1\"}},\"required\":[\"channel\",\"module\",\"input\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"moved\":{\"type\":\"boolean\"},\"channel\":{\"type\":\"object\"},\"restart_required\":{\"type\":\"boolean\"}}}",
      t_move_point },
    { "set_bus_pins", RBLLM_SCOPE_CONFIG,
      "When to call: to start I2C bus 1 (get_health: running=false), after the user says which GPIO pins carry its SDA and SCL. What it "
      "does: saves the pins (checked against the board's free pins, pins_ok in get_health); the bus starts after a restart, so tell the "
      "user before calling. off:true stops bus 1 (no module may sit on it).",
      "{\"type\":\"object\",\"properties\":{\"bus\":{\"type\":\"integer\",\"enum\":[1]},\"sda\":{\"type\":\"integer\"},\"scl\":{\"type\":\"integer\"},\"off\":{\"type\":\"boolean\"}},\"required\":[\"bus\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"saved\":{\"type\":\"boolean\"},\"restart_required\":{\"type\":\"boolean\"}}}",
      t_set_bus_pins },
    { "restart_hub", RBLLM_SCOPE_CONFIG,
      "When to call: when a change answered restart_required=true and the user agreed to a restart now. What it does: restarts the hub "
      "(about 30 s without data; the hour in progress is kept). Refused for a minute after a firmware update.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"restarting\":{\"type\":\"boolean\"},\"back_in_s\":{\"type\":\"integer\"}}}",
      t_restart_hub },
    { "delete_point", RBLLM_SCOPE_CONFIG,
      "When to call: when the user wants a point gone (a clamp removed, or a point made on the wrong input). What it does: deletes the "
      "point; its input becomes free. The supply point is not deleted (move it with move_point); a point with points inside it goes last.",
      "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"string\",\"description\":\"id from list_channels\"}},\"required\":[\"channel\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"deleted\":{\"type\":\"boolean\"},\"channel\":{\"type\":\"string\"}}}",
      t_delete_point },
    { "set_module_bus", RBLLM_SCOPE_CONFIG,
      "When to call: after the user moved a module's cable to the other I2C bus. What it does: tells the hub to look for that module on "
      "the other bus at the same address; points and history stay. The address must be free on the new bus.",
      "{\"type\":\"object\",\"properties\":{\"module\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":7},\"bus\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1}},\"required\":[\"module\",\"bus\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"moved\":{\"type\":\"boolean\"},\"modules\":{\"type\":\"array\"}}}",
      t_set_module_bus },
    { "list_nodes", RBLLM_SCOPE_READ,
      "When to call: when the user has remote metering nodes (ESP-NOW boards far from the hub) or wants to add one. What it returns: "
      "whether remote nodes are on, the pairing window, and each node with its MAC, state, link quality and channels.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      "{\"type\":\"object\"}",
      t_list_nodes },
    { "node_action", RBLLM_SCOPE_CONFIG,
      "When to call: to add or manage a remote node. What it does: hub_on / hub_off switch remote nodes on or off; pair opens a pairing "
      "window (seconds) for a new node, pair_cancel closes it; remove, disable, enable act on one node (mac); configure sets a node's "
      "channels and period_s.",
      "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"hub_on\",\"hub_off\",\"pair\",\"pair_cancel\",\"remove\",\"disable\",\"enable\",\"configure\"]},"
      "\"mac\":{\"type\":\"string\"},\"seconds\":{\"type\":\"integer\",\"minimum\":10,\"maximum\":600},\"channels\":{\"type\":\"array\"},\"period_s\":{\"type\":\"integer\"}},\"required\":[\"action\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"done\":{\"type\":\"boolean\"},\"reply\":{\"type\":\"object\"}}}",
      t_node_action },
    { "set_prices", RBLLM_SCOPE_CONFIG,
      "When to call: when the user gives energy prices for coming hours (a supplier's day-ahead list, a special offer) and no dynamic price "
      "provider fetches them. What it does: stores the prices for the periods given; they replace the zone price there. clear:true removes all "
      "stored prices.",
      "{\"type\":\"object\",\"properties\":{\"prices\":{\"type\":\"array\",\"maxItems\":192,\"items\":{\"type\":\"object\",\"properties\":{\"buy\":{\"type\":\"number\",\"description\":\"per kWh, before fees and VAT\"},"
      "\"sell\":{\"type\":\"number\"},\"from\":{\"type\":\"string\",\"description\":\"YYYY-MM-DDTHH:MM, hub time\"},\"until\":{\"type\":\"string\"}},\"required\":[\"buy\",\"from\",\"until\"]}},"
      "\"clear\":{\"type\":\"boolean\"}},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"stored\":{\"type\":\"boolean\"},\"cleared\":{\"type\":\"boolean\"},\"count\":{\"type\":\"integer\"}}}",
      t_set_prices },
    { "set_clock", RBLLM_SCOPE_CONFIG,
      "When to call: when get_health or check_installation says the hub's clock is not set or wrong and the user tells you the time on their "
      "clock. What it does: sets the hub's clock (time with its UTC offset), or sync:true asks the Internet time source again.",
      "{\"type\":\"object\",\"properties\":{\"time\":{\"type\":\"string\",\"description\":\"e.g. 2026-11-24T14:05:00+01:00, from the user\"},\"sync\":{\"type\":\"boolean\"}},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"hub_time\":{\"type\":\"string\"},\"changed_by_s\":{\"type\":\"number\"},\"internet_time\":{\"type\":\"boolean\"}}}",
      t_set_clock },
    { "set_module_drdy", RBLLM_SCOPE_CONFIG,
      "When to call: after the user wired a module's DRDY (data ready) pin to its own GPIO, or to go back to the shared line. What it does: "
      "saves which GPIO the module's ready signal arrives on; -1 = the shared line.",
      "{\"type\":\"object\",\"properties\":{\"module\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":7},\"gpio\":{\"type\":\"integer\",\"minimum\":-1,\"maximum\":48}},\"required\":[\"module\",\"gpio\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"saved\":{\"type\":\"boolean\"},\"modules\":{\"type\":\"array\"}}}",
      t_set_module_drdy },
    { "set_point_slug", RBLLM_SCOPE_CONFIG,
      "When to call: when the user wants a point's machine name changed (the 'point' slug in list_channels, used in MQTT topics and Home "
      "Assistant entity ids). The display name is changed with propose_config instead. What it does: renames the slug. The old slug is "
      "retired, so it cannot be given back, even to this point, without reuse:true.",
      "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"string\",\"description\":\"id from list_channels\"},\"point\":{\"type\":\"string\",\"pattern\":\"^[a-z0-9_.-]{1,32}$\"},"
      "\"reuse\":{\"type\":\"boolean\",\"description\":\"take over a retired slug (a deleted point's, or one renamed away)\"}},\"required\":[\"channel\",\"point\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"renamed\":{\"type\":\"boolean\"},\"channel\":{\"type\":\"string\"},\"point\":{\"type\":\"string\"}}}",
      t_set_point_slug },
    { "forget_me", RBLLM_SCOPE_READ,
      "When to call: after pairing again with a wider scope, to give back the previous token, or when the user wants this assistant "
      "disconnected. What it does: revokes the token this call is made with. Then pair again to come back.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"revoked\":{\"type\":\"boolean\"}}}",
      t_forget_me },
    { "get_analysis_playbook", RBLLM_SCOPE_READ,
      "When to call: when the user asks for a report or a check and you want the hub's recipe. What it returns: the steps (tools with argument "
      "hints), questions to ask the user and rules for the answer.",
      "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"enum\":[\"monthly_report\",\"installation_check\",\"first_setup\"]}},\"required\":[\"id\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"steps\":{\"type\":\"array\"},\"rules\":{\"type\":\"array\"}}}",
      t_get_analysis_playbook },
};
#define N_TOOLS (sizeof(TOOLS) / sizeof(TOOLS[0]))

static const char *const PLAYBOOKS[] = {
    "{\"id\":\"monthly_report\",\"title\":\"Monthly report\",\"steps\":[{\"tool\":\"get_hub_info\"},{\"tool\":\"list_channels\"},"
    "{\"tool\":\"get_energy_summary\",\"args_hint\":{\"period\":{\"preset\":\"last_month\"},\"resolution\":\"day\"}},"
    "{\"tool\":\"get_cost_breakdown\",\"args_hint\":{\"period\":{\"preset\":\"last_month\"}}},"
    "{\"tool\":\"compare_periods\",\"args_hint\":{\"a\":{\"preset\":\"last_month\"},\"baseline\":\"previous_period\"}}],"
    "\"ask_user\":[\"Which month?\"],\"rules\":[\"Show the numbers and the period behind every conclusion.\",\"Answer in the user's language.\","
    "\"Say when meta.gaps or meta.estimated is true.\"]}",
    "{\"id\":\"installation_check\",\"title\":\"Installation check\",\"steps\":[{\"tool\":\"check_installation\"},{\"tool\":\"get_health\"},{\"tool\":\"list_channels\"},"
    "{\"tool\":\"read_live\"},{\"tool\":\"get_events\",\"args_hint\":{\"limit\":30}}],"
    "\"ask_user\":[\"Which appliances are running now?\"],\"rules\":[\"A missing module or a channel that is not present comes first.\","
    "\"A reversed clamp is fixed at the clamp; never suggest a software sign flip.\",\"Answer in the user's language.\"]}",
    "{\"id\":\"first_setup\",\"title\":\"First setup of a new hub\",\"steps\":[{\"tool\":\"get_hub_info\"},{\"tool\":\"get_health\"},"
    "{\"tool\":\"set_bus_pins\",\"when\":\"a module is connected to bus 1 and get_health shows bus 1 running=false: set its pins first, then restart_hub after the user agrees\"},"
    "{\"tool\":\"scan_modules\",\"args_hint\":{\"bus\":0},\"repeat\":\"after each module the user connects\"},"
    "{\"tool\":\"add_module\",\"args_hint\":{\"bus\":0,\"address\":\"0x50\"},\"when\":\"scan shows a module with added=false\"},"
    "{\"tool\":\"set_module_address\",\"args_hint\":{\"module\":0,\"address\":\"0x51\"},\"when\":\"the module sits at 0x50; use the bus's next_free_address\"},"
    "{\"tool\":\"list_module_inputs\",\"when\":\"the module is added and moved\"},"
    "{\"tool\":\"create_point\",\"args_hint\":{\"module\":1,\"input\":\"I0\",\"name\":\"Kitchen\"},\"when\":\"for each current input in use without a point; ct_model only when clamp=selectable\"},"
    "{\"tool\":\"list_channels\"},{\"tool\":\"move_point\",\"when\":\"the main_supply point is not on the input with the grid clamp\"},"
    "{\"tool\":\"identify_channel\",\"args_hint\":{\"action\":\"start\"},\"when\":\"to tell which clamp is which\"},"
    "{\"tool\":\"propose_config\",\"when\":\"rename points, change roles or clamp models, enter the tariff\"},{\"tool\":\"check_installation\"}],"
    "\"ask_user\":[\"Connect ONE module now (cable to the hub, module powered) and tell me when it is done.\",\"Which inputs of this module have a clamp, and what does each one measure?\","
    "\"Which clamp model is printed on it?\",\"Which clamp sits on the grid feed (the main supply)?\"],"
    "\"rules\":[\"Every new module arrives at I2C address 0x50. Connect modules one at a time: add it, move it off 0x50 (next_free_address), only then ask for the next one.\","
    "\"An address is unique on its bus; the same address may be used once on each bus.\","
    "\"A module measures nothing until its inputs carry points: create_point for each input in use.\","
    "\"clamp=fixed: the clamps are calibrated with the module; never offer a clamp model. clamp=selectable: ask for the model printed on the clamp.\","
    "\"The hub starts with one main_supply point; without it on the grid clamp there is no grid figure and no bill: ask which input carries it and use move_point.\","
    "\"Bus pins and restarts: tell the user before set_bus_pins and ask before restart_hub.\","
    "\"Up to the hub's max_modules (get_health) on one bus or both.\",\"Set the time zone (propose_config region.timezone), the hub name and the tariff before reading money or days.\","
    "\"Names, roles, clamp models and tariff go through propose_config and the user's OK before apply_config.\",\"Answer in the user's language.\"]}",
};

static cJSON *t_get_analysis_playbook(const cJSON *a, rbllm_cx_t *cx)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(a, "id"));
    for (size_t i = 0; id && i < sizeof(PLAYBOOKS) / sizeof(PLAYBOOKS[0]); i++) {
        cJSON *pb = cJSON_Parse(PLAYBOOKS[i]);
        if (pb && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(pb, "id")), id)) { cx->as_of = now_s(); return pb; }
        cJSON_Delete(pb);
    }
    return fail(cx, "not_found", "unknown playbook", "Use monthly_report or installation_check.");
}

/* Manifest hash: sha256 over the tool names, scopes, schemas and playbooks (stable per firmware). */
static void manifest_hash(char out[72])
{
    mbedtls_sha256_context c;
    uint8_t h[32];
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, (const unsigned char *)RBLLM_PROTO, strlen(RBLLM_PROTO));
    for (size_t i = 0; i < N_TOOLS; i++) {
        mbedtls_sha256_update(&c, (const unsigned char *)TOOLS[i].name, strlen(TOOLS[i].name));
        mbedtls_sha256_update(&c, &TOOLS[i].scope, 1);
        mbedtls_sha256_update(&c, (const unsigned char *)TOOLS[i].desc, strlen(TOOLS[i].desc));
        mbedtls_sha256_update(&c, (const unsigned char *)TOOLS[i].input, strlen(TOOLS[i].input));
        mbedtls_sha256_update(&c, (const unsigned char *)TOOLS[i].output, strlen(TOOLS[i].output));
    }
    for (size_t i = 0; i < sizeof(PLAYBOOKS) / sizeof(PLAYBOOKS[0]); i++)
        mbedtls_sha256_update(&c, (const unsigned char *)PLAYBOOKS[i], strlen(PLAYBOOKS[i]));
    mbedtls_sha256_finish(&c, h);
    mbedtls_sha256_free(&c);
    strcpy(out, "sha256:");
    hex_of(h, 32, out + 7);
}

char *rbllm_manifest_json(void)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) return NULL;
    cJSON_AddStringToObject(r, "proto", RBLLM_PROTO);
    cJSON *hub = cJSON_AddObjectToObject(r, "hub");
    char id[13]; rbllm_hub_id(id);
    cJSON_AddStringToObject(hub, "id", id);
    { char hn[48]; hub_display_name(hn, sizeof(hn)); cJSON_AddStringToObject(hub, "name", hn); }
    const esp_app_desc_t *ad = esp_app_get_description();
    cJSON_AddStringToObject(hub, "fw", ad ? ad->version : "");
    { char eh[17] = ""; esp_app_get_elf_sha256(eh, sizeof(eh)); cJSON_AddStringToObject(hub, "build", eh); }
#if CONFIG_IDF_TARGET_ESP32S3
    cJSON_AddStringToObject(hub, "board", "esp32s3");
#else
    cJSON_AddStringToObject(hub, "board", "esp32");
#endif
    char mh[72]; manifest_hash(mh);
    cJSON_AddStringToObject(r, "manifest_hash", mh);
    cJSON *cap = cJSON_AddArrayToObject(r, "capabilities");
    cJSON_AddItemToArray(cap, cJSON_CreateString("read"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("events"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("playbooks"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("config_read"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("config"));
    cJSON *lim = cJSON_AddObjectToObject(r, "limits");
    cJSON_AddNumberToObject(lim, "max_response_bytes", RBLLM_MAX_RESPONSE_BYTES);
    cJSON_AddNumberToObject(lim, "max_manifest_bytes", RBLLM_MAX_MANIFEST_BYTES);
    cJSON_AddNumberToObject(lim, "max_concurrent", RBLLM_MAX_CONCURRENT);
    cJSON_AddNumberToObject(lim, "call_timeout_ms", RBLLM_CALL_TIMEOUT_MS);
    cJSON *loc = cJSON_AddObjectToObject(r, "locale");
    cJSON_AddStringToObject(loc, "currency", rbpower_tariff_currency());
    rbtime_config_t tc; memset(&tc, 0, sizeof(tc));
    rbtime_get_config(&tc);
    if (tc.timezone[0]) cJSON_AddStringToObject(loc, "timezone", tc.timezone);
    cJSON *tools = cJSON_AddArrayToObject(r, "tools");
    for (size_t i = 0; i < N_TOOLS; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", TOOLS[i].name);
        cJSON_AddStringToObject(t, "description", TOOLS[i].desc);
        cJSON_AddStringToObject(t, "scope", TOOLS[i].scope == RBLLM_SCOPE_CONFIG ? "config" : "read");
        cJSON_AddItemToObject(t, "input_schema", cJSON_Parse(TOOLS[i].input));
        cJSON_AddItemToObject(t, "output_schema", cJSON_Parse(TOOLS[i].output));
        cJSON *an = cJSON_AddObjectToObject(t, "annotations");
        bool fm = !strcmp(TOOLS[i].name, "forget_me");      /* read scope, but it revokes the caller's token */
        /* writes into a module or the board that the client should confirm with the user first (MCP clients ask on this hint) */
        bool ds = fm || !strcmp(TOOLS[i].name, "add_module") || !strcmp(TOOLS[i].name, "set_module_address") ||
                  !strcmp(TOOLS[i].name, "set_bus_pins") || !strcmp(TOOLS[i].name, "restart_hub") || !strcmp(TOOLS[i].name, "delete_point") ||
                  !strcmp(TOOLS[i].name, "set_module_bus") || !strcmp(TOOLS[i].name, "node_action") ||
                  !strcmp(TOOLS[i].name, "set_clock") || !strcmp(TOOLS[i].name, "set_module_drdy") || !strcmp(TOOLS[i].name, "set_point_slug");
        bool ro = !fm && (TOOLS[i].scope == RBLLM_SCOPE_READ || !strncmp(TOOLS[i].name, "get_", 4) || !strcmp(TOOLS[i].name, "propose_config") ||
                  !strcmp(TOOLS[i].name, "list_config_versions"));   /* config scope but no change */
        cJSON_AddBoolToObject(an, "readOnlyHint", ro);
        cJSON_AddBoolToObject(an, "destructiveHint", ds);
        cJSON_AddBoolToObject(an, "idempotentHint", ro);
        cJSON_AddItemToArray(tools, t);
    }
    cJSON *pbs = cJSON_AddArrayToObject(r, "playbooks");
    for (size_t i = 0; i < sizeof(PLAYBOOKS) / sizeof(PLAYBOOKS[0]); i++) cJSON_AddItemToArray(pbs, cJSON_Parse(PLAYBOOKS[i]));
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    return s;
}

/* ============================================================================================ envelope */

char *rbllm_error_json(const char *request_id, const char *code, const char *message, const char *hint, int retry_after_ms)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) return NULL;
    cJSON_AddBoolToObject(r, "ok", false);
    if (request_id) cJSON_AddStringToObject(r, "request_id", request_id); else cJSON_AddNullToObject(r, "request_id");
    char id[13]; rbllm_hub_id(id);
    cJSON_AddStringToObject(r, "hub_id", id);
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message ? message : "");
    if (hint && hint[0]) cJSON_AddStringToObject(e, "hint", hint); else cJSON_AddNullToObject(e, "hint");
    if (retry_after_ms > 0) cJSON_AddNumberToObject(e, "retry_after_ms", retry_after_ms); else cJSON_AddNullToObject(e, "retry_after_ms");
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    return s;
}

/* Top-level argument check against the tool's schema: unknown keys (additionalProperties false), required keys,
 * and the basic type of each known key. Deeper checks are the tool's. */
static bool check_args(rbllm_cx_t *cx, const rbllm_tool_t *t, const cJSON *args)
{
    cJSON *sch = cJSON_Parse(t->input);
    if (!sch) { fail(cx, "internal", "schema parse", NULL); return false; }
    const cJSON *props = cJSON_GetObjectItem(sch, "properties");
    bool ok = true;
    const cJSON *a;
    cJSON_ArrayForEach(a, args) {
        const cJSON *pd = cJSON_GetObjectItem(props, a->string);
        char m[96];
        if (!pd) { snprintf(m, sizeof(m), "unknown argument '%.40s'", a->string); fail(cx, "invalid_args", m, "Check the tool's input_schema in the manifest."); ok = false; break; }
        const char *ty = cJSON_GetStringValue(cJSON_GetObjectItem(pd, "type"));
        bool tok = !ty || (!strcmp(ty, "object") && cJSON_IsObject(a)) || (!strcmp(ty, "array") && cJSON_IsArray(a)) ||
                   (!strcmp(ty, "string") && cJSON_IsString(a)) || (!strcmp(ty, "integer") && cJSON_IsNumber(a)) ||
                   (!strcmp(ty, "number") && cJSON_IsNumber(a)) || (!strcmp(ty, "boolean") && cJSON_IsBool(a));
        if (!tok) { snprintf(m, sizeof(m), "argument '%.40s' must be %s", a->string, ty); fail(cx, "invalid_args", m, NULL); ok = false; break; }
    }
    const cJSON *rq;
    cJSON_ArrayForEach(rq, cJSON_GetObjectItem(sch, "required")) {
        if (!ok) break;
        if (!cJSON_GetObjectItem(args, cJSON_GetStringValue(rq))) {
            char m[96]; snprintf(m, sizeof(m), "missing argument '%.40s'", cJSON_GetStringValue(rq));
            fail(cx, "invalid_args", m, "Check the tool's input_schema in the manifest."); ok = false;
        }
    }
    cJSON_Delete(sch);
    return ok;
}

static void log_call(const char *client, const char *tool, const char *code, int64_t us, size_t bytes)
{
    lk();
    if (S.log) {
        rbllm_log_t *l = &S.log[S.log_head];
        l->ts = rbtime_is_time_valid() ? now_s() : 0;   /* 0 = before the clock synced */
        /* Both come from the request (an unknown tool name is the caller's raw text): keep only well-formed UTF-8,
         * or one hostile call makes the admin view's JSON unreadable. */
        utf8_clean(client, l->client, sizeof(l->client));
        utf8_clean(tool, l->tool, sizeof(l->tool));
        strlcpy(l->code, code, sizeof(l->code));
        l->ms = (uint16_t)(us / 1000 > 65535 ? 65535 : us / 1000);
        l->bytes = (uint16_t)(bytes > 65535 ? 65535 : bytes);
        l->cfg = s_cfg_changed; s_cfg_changed = false;
        S.log_head = (uint16_t)((S.log_head + 1) % RBLLM_LOG_N);
        if (S.log_n < RBLLM_LOG_N) S.log_n++;
    }
    ulk();
}

char *rbllm_call(const char *body, size_t len, const rbllm_caller_t *caller, int64_t deadline_us)
{
    int64_t t0 = esp_timer_get_time();
    rbllm_cx_t cx = { .deadline_us = deadline_us ? deadline_us : t0 + (int64_t)RBLLM_CALL_TIMEOUT_MS * 1000,
                      .scopes = caller ? caller->scopes : 0, .client = caller ? caller->client : "",
                      .internal = caller && !caller->token_id[0] && !strcmp(caller->client, RBLLM_CLIENT_WEB),
                      .token_id = caller ? caller->token_id : "" };
    cJSON *req = (len && len <= RBLLM_MAX_REQUEST_BYTES) ? cJSON_ParseWithLength(body, len) : NULL;
    const char *rid = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "request_id")) : NULL;
    char ridb[65] = {0};
    if (rid) utf8_clean(rid, ridb, sizeof(ridb));   /* echoed back: well-formed UTF-8 only, never cut inside a character */
    const char *tname = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "tool")) : NULL;
    char tnb[28] = {0};
    if (tname) utf8_clean(tname, tnb, sizeof(tnb));
    const rbllm_tool_t *t = NULL;
    cJSON *res = NULL;
    if (!req || !cJSON_IsObject(req)) fail(&cx, "invalid_args", "body: a well-formed JSON object {tool, args, request_id} required (max 4 KB, nesting depth limited)", NULL);
    else if (!tname) fail(&cx, "invalid_args", "missing 'tool'", NULL);
    else {
        for (size_t i = 0; i < N_TOOLS; i++) if (!strcmp(TOOLS[i].name, tname)) t = &TOOLS[i];
        const cJSON *args = cJSON_GetObjectItem(req, "args");
        cJSON *empty = NULL;
        if (!args || cJSON_IsNull(args)) args = empty = cJSON_CreateObject();
        if (!t) fail(&cx, "unknown_tool", "no such tool", "Refresh the manifest.");
        else if (!(cx.scopes & t->scope)) {
            char m[64]; snprintf(m, sizeof(m), "the tool needs scope '%s'", t->scope == RBLLM_SCOPE_CONFIG ? "config" : "read");
            fail(&cx, "forbidden_scope", m, "Pair again with that scope on the hub's AI / LLM page.");
        } else if (S.read_only && !cx.internal && t->scope == RBLLM_SCOPE_CONFIG &&   /* every writer; reads and the preview stay */
                   strcmp(t->name, "get_config") && strcmp(t->name, "get_config_schema") && strcmp(t->name, "list_config_versions") && strcmp(t->name, "propose_config"))
            fail(&cx, "read_only_mode", "the hub is in read-only mode for LLM clients: the user switched changes by assistants off",
                 "Only the user can switch read-only off, on the hub's AI / LLM page (get_hub_info.hub_page_only: read_only). propose_config still checks a change and shows its diff.");
        else if (!cJSON_IsObject(args)) fail(&cx, "invalid_args", "'args' must be an object", NULL);
        else if (check_args(&cx, t, args)) res = t->fn(args, &cx);
        cJSON_Delete(empty);
    }
    cJSON_Delete(req);

    char *out = NULL;
    if (res) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", true);
        if (ridb[0]) cJSON_AddStringToObject(r, "request_id", ridb); else cJSON_AddNullToObject(r, "request_id");
        char id[13]; rbllm_hub_id(id);
        cJSON_AddStringToObject(r, "hub_id", id);
        cJSON_AddItemToObject(r, "result", res);
        cJSON *m = cJSON_AddObjectToObject(r, "meta");
        if (cx.as_of > RBGRID_TS_SANE_MIN) add_time(m, "as_of", cx.as_of); else cJSON_AddNullToObject(m, "as_of");
        cJSON_AddBoolToObject(m, "truncated", cx.truncated);
        cJSON_AddBoolToObject(m, "gaps", cx.gaps);
        cJSON_AddBoolToObject(m, "estimated", cx.estimated);
        if (cx.resolution) cJSON_AddStringToObject(m, "resolution", cx.resolution);
        if (!rbtime_is_time_valid()) cJSON_AddTrueToObject(m, "clock_unsynced");   /* every time in this answer is the hub's unsynced clock */
        if (cx.open_hour) {
            if (!cx.notes) cx.notes = cJSON_CreateArray();
            if (cx.open_cost_exact)
                cJSON_AddItemToArray(cx.notes, cJSON_CreateString("The current hour is included up to its last closed 15-min period; the main supply's cost for it is the hub's own booked figure."));
            else {
                cJSON_AddItemToArray(cx.notes, cJSON_CreateString("The current hour is included up to its last closed 15-min period; its cost is estimated at the price in force now."));
                cJSON_ReplaceItemInObject(m, "estimated", cJSON_CreateBool(true));
            }
        }
        if (cx.notes) { cJSON_AddItemToObject(m, "notes", cx.notes); cx.notes = NULL; }
        out = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        if (out && strlen(out) > RBLLM_MAX_RESPONSE_BYTES) {
            /* the tools budget their rows; reaching this means a budget estimate is off — say so, don't cut JSON */
            rbpower_diag_printf("FAIL llm: %s answer %u B > cap %u B", tnb, (unsigned)strlen(out), (unsigned)RBLLM_MAX_RESPONSE_BYTES);
            free(out);
            out = NULL;
            fail(&cx, "range_too_large", "the answer would exceed max_response_bytes", "Narrow the period, use a coarser resolution or fewer channels.");
        }
    }
    cJSON_Delete(cx.notes);
    if (!out) out = rbllm_error_json(ridb[0] ? ridb : NULL, cx.err_code ? cx.err_code : "internal",
                                     cx.err_code ? cx.err_msg : "out of memory", cx.err_hint, cx.retry_ms);
    log_call(cx.client, tnb[0] ? tnb : "?", cx.err_code ? cx.err_code : "ok", esp_timer_get_time() - t0, out ? strlen(out) : 0);
    return out;
}

/* ============================================================================================ worker */

typedef struct {
    char *body; size_t len; rbllm_caller_t caller;
    void (*done)(char *, void *); void *ctx; int64_t deadline_us;
} rbllm_job_t;

#define RBLLM_STACK 12288   /* apply_config runs rbgrid_units_mutate, which keeps fleet config copies on the stack */
/* The stack is internal heap (the tools read flash with the cache off — no PSRAM stack), taken after the boot
 * window rather than as static RAM; see rbllm_init. */
static TaskHandle_t s_task;
static QueueHandle_t s_q;
static StaticQueue_t s_q_buf;
static uint8_t s_q_store[(RBLLM_MAX_CONCURRENT - 1 > 0 ? RBLLM_MAX_CONCURRENT - 1 : 1) * sizeof(rbllm_job_t)];
static volatile bool s_busy;
TaskHandle_t rbllm_worker_handle(void) { return s_task; }

static void worker(void *arg)
{
    (void)arg;
    rbllm_job_t j;
    for (;;) {
        bool got = xQueueReceive(s_q, &j, pdMS_TO_TICKS(10000)) == pdTRUE;
        ev_watch();                       /* rate-limited inside: about every 10 s, busy or idle */
        if (!got) continue;
        s_busy = true;
        char *rep = (esp_timer_get_time() > j.deadline_us)
                        ? rbllm_error_json(NULL, "timeout", "the call waited too long in the queue", "Retry.", 0)
                        : rbllm_call(j.body, j.len, &j.caller, j.deadline_us);
        free(j.body);
        j.done(rep, j.ctx);
        s_busy = false;
    }
}

static void ensure_task(void)
{
    static volatile int s_creating;
    if (s_task || __atomic_exchange_n(&s_creating, 1, __ATOMIC_ACQ_REL)) return;   /* timer task vs httpd */
    if (!s_q) s_q = xQueueCreateStatic(sizeof(s_q_store) / sizeof(rbllm_job_t), sizeof(rbllm_job_t), s_q_store, &s_q_buf);
    TaskHandle_t h = NULL;
    if (xTaskCreate(worker, "rbllm", RBLLM_STACK, NULL, 2, &h) == pdPASS) s_task = h;
    else ESP_LOGW(TAG, "worker not started yet (internal free %u, largest %u) — retry in 30 s",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    __atomic_store_n(&s_creating, 0, __ATOMIC_RELEASE);
}

static void task_timer_cb(void *arg)
{
    (void)arg;
    ensure_task();
    if (s_task) esp_timer_stop(s_task_timer);
}

bool rbllm_submit(char *body, size_t len, const rbllm_caller_t *caller, void (*done)(char *, void *), void *ctx)
{
    if (!s_task) {
        if (!s_mx) rbllm_init();
        ensure_task();
    }
    if (!s_task || !s_q) return false;   /* worker not started (no internal heap for its stack): caller answers busy */
    rbllm_job_t j = { .body = body, .len = len, .done = done, .ctx = ctx,
                      .deadline_us = esp_timer_get_time() + (int64_t)RBLLM_CALL_TIMEOUT_MS * 1000 };
    if (caller) j.caller = *caller;
    return xQueueSend(s_q, &j, 0) == pdTRUE;
}

/* ============================================================================================ pairing */

static void pair_gc_locked(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < RBLLM_MAX_PAIRS; i++) if (S.pair[i].pair_id[0] && now > S.pair[i].exp_us) memset(&S.pair[i], 0, sizeof(S.pair[i]));
}

static char *jerr(int *status, int st, const char *code, const char *msg)
{
    *status = st;
    return rbllm_error_json(NULL, code, msg, NULL, 0);
}
static char *jerr_h(int *status, int st, const char *code, const char *msg, const char *hint)
{
    *status = st;
    return rbllm_error_json(NULL, code, msg, hint, 0);
}

char *rbllm_pair_start(const char *body, int *status)
{
    if (!S.enabled) return jerr(status, 403, "llm_disabled", "the LLM API is off; switch it on on the hub's AI / LLM page");
    cJSON *b = body ? cJSON_Parse(body) : NULL;
    const char *nm = b ? cJSON_GetStringValue(cJSON_GetObjectItem(b, "client_name")) : NULL;
    uint8_t sc = b ? scopes_from_json(cJSON_GetObjectItem(b, "scopes")) : 0;
    if (!nm || !nm[0] || strlen(nm) > 32 || !sc) { cJSON_Delete(b); return jerr(status, 400, "invalid_args", "client_name (1..32 chars) and scopes [read|config] required"); }
    for (const char *c = nm; *c; c++) if ((unsigned char)*c < 0x20) { cJSON_Delete(b); return jerr(status, 400, "invalid_args", "client_name: no control characters"); }
    if (!strcasecmp(nm, RBLLM_CLIENT_WEB)) { cJSON_Delete(b); return jerr_h(status, 400, "invalid_args", "client_name: \"web\" is reserved for the hub page", "Pair under the assistant's own name, e.g. \"Claude Desktop\"."); }
    lk();
    pair_gc_locked();
    if (esp_timer_get_time() < S.pair_block_until_us) { ulk(); cJSON_Delete(b); return jerr(status, 429, "busy", "pairing is blocked for 10 minutes after too many wrong codes"); }
    int slot = -1;
    for (int i = 0; i < RBLLM_MAX_PAIRS; i++) if (!S.pair[i].pair_id[0]) { slot = i; break; }
    if (slot < 0) { ulk(); cJSON_Delete(b); return jerr(status, 429, "busy", "two pairings are already waiting; finish or let them expire (10 min)"); }
    rbllm_pair_t *p = &S.pair[slot];
    rand_hex(p->pair_id, 8);
    utf8_clean(nm, p->client, sizeof(p->client));
    p->scopes = sc;
    snprintf(p->code, sizeof(p->code), "%06lu", (unsigned long)(esp_random() % 1000000u));
    p->exp_us = esp_timer_get_time() + (int64_t)RBLLM_PAIR_TTL_S * 1000000;
    p->fails = 0;
    char pid[17]; strlcpy(pid, p->pair_id, sizeof(pid));
    char cname[33]; strlcpy(cname, p->client, sizeof(cname));   /* nm points into b, freed below */
    ulk();
    cJSON_Delete(b);
    rbpower_diag_printf("llm: pairing started for '%s' — the code is on the hub's AI / LLM page", cname);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "pair_id", pid);
    cJSON_AddNumberToObject(r, "expires_in", RBLLM_PAIR_TTL_S);
    cJSON_AddStringToObject(r, "next", "Ask the user for the 6-digit code shown on the hub's AI / LLM page, then call pair/complete.");
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    *status = 200;
    return s;
}

char *rbllm_pair_complete(const char *body, int *status)
{
    if (!S.enabled) return jerr(status, 403, "llm_disabled", "the LLM API is off");
    cJSON *b = body ? cJSON_Parse(body) : NULL;
    const char *pid = b ? cJSON_GetStringValue(cJSON_GetObjectItem(b, "pair_id")) : NULL;
    const char *code = b ? cJSON_GetStringValue(cJSON_GetObjectItem(b, "code")) : NULL;
    if (!pid || !code) { cJSON_Delete(b); return jerr(status, 400, "invalid_args", "pair_id and code required"); }
    lk();
    pair_gc_locked();
    int64_t now = esp_timer_get_time();
    if (now < S.pair_block_until_us) { ulk(); cJSON_Delete(b); return jerr(status, 429, "busy", "pairing is blocked for 10 minutes"); }
    rbllm_pair_t *p = NULL;
    for (int i = 0; i < RBLLM_MAX_PAIRS; i++) if (S.pair[i].pair_id[0] && !strcmp(S.pair[i].pair_id, pid)) p = &S.pair[i];
    if (!p) { ulk(); cJSON_Delete(b); return jerr(status, 404, "not_found", "unknown or expired pair_id; start again"); }
    if (strcmp(p->code, code) != 0) {
        if (now - S.pair_fail_win_us > 600LL * 1000000) { S.pair_fail_win_us = now; S.pair_fail_n = 0; }
        if (++S.pair_fail_n >= 20) { S.pair_block_until_us = now + 600LL * 1000000; memset(S.pair, 0, sizeof(S.pair)); }
        if (++p->fails >= 5) memset(p, 0, sizeof(*p));
        ulk(); cJSON_Delete(b);
        return jerr(status, 401, "unauthorized", "wrong code");
    }
    int slot = -1;
    for (int i = 0; i < RBLLM_MAX_TOKENS; i++) if (!S.tok[i].id[0]) { slot = i; break; }
    if (slot < 0) { ulk(); cJSON_Delete(b); return jerr(status, 507, "internal", "8 LLM clients are paired; revoke one on the hub's AI / LLM page"); }
    char tok[48] = "rbl_";
    rand_hex(tok + 4, 20);
    rbllm_token_t *t = &S.tok[slot];
    memset(t, 0, sizeof(*t));
    rand_hex(t->id, 4);
    strlcpy(t->name, p->client, sizeof(t->name));
    t->scopes = p->scopes;
    sha256_hex(tok, t->hash);
    t->created = now_s() > RBGRID_TS_SANE_MIN ? now_s() : 0;
    uint8_t sc = p->scopes;
    char nm[33]; strlcpy(nm, p->client, sizeof(nm));
    memset(p, 0, sizeof(*p));
    esp_err_t se = save_locked();
    ulk();
    cJSON_Delete(b);
    if (se != ESP_OK) { memset(tok, 0, sizeof(tok)); return jerr(status, 500, "internal", "the token could not be saved"); }
    rbpower_diag_printf("llm: client '%s' paired", nm);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "token", tok);
    scopes_to_json(r, "scopes", sc);
    char id[13]; rbllm_hub_id(id);
    cJSON_AddStringToObject(r, "hub_id", id);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    memset(tok, 0, sizeof(tok));
    *status = 200;
    return s;
}

/* ============================================================================================ admin */

char *rbllm_admin_json(void)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) return NULL;
    lk();
    pair_gc_locked();
    cJSON_AddBoolToObject(r, "enabled", S.enabled);
    cJSON_AddBoolToObject(r, "read_only", S.read_only);
    cJSON_AddStringToObject(r, "proto", RBLLM_PROTO);
    cJSON *pa = cJSON_AddArrayToObject(r, "pairing");
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < RBLLM_MAX_PAIRS; i++) {
        rbllm_pair_t *p = &S.pair[i];
        if (!p->pair_id[0]) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "pair_id", p->pair_id);
        cJSON_AddStringToObject(o, "client_name", p->client);
        scopes_to_json(o, "scopes", p->scopes);
        cJSON_AddStringToObject(o, "code", p->code);
        cJSON_AddNumberToObject(o, "expires_in", (double)((p->exp_us - now) / 1000000));
        cJSON_AddItemToArray(pa, o);
    }
    cJSON *ta = cJSON_AddArrayToObject(r, "tokens");
    for (int i = 0; i < RBLLM_MAX_TOKENS; i++) {
        rbllm_token_t *t = &S.tok[i];
        if (!t->id[0]) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", t->id);
        cJSON_AddStringToObject(o, "client_name", t->name);
        scopes_to_json(o, "scopes", t->scopes);
        if (t->created) cJSON_AddNumberToObject(o, "created", t->created); else cJSON_AddNullToObject(o, "created");
        if (t->last_used) cJSON_AddNumberToObject(o, "last_used", t->last_used); else cJSON_AddNullToObject(o, "last_used");
        cJSON_AddItemToArray(ta, o);
    }
    cJSON *la = cJSON_AddArrayToObject(r, "log");
    for (int k = 0; S.log && k < S.log_n && k < 50; k++) {   /* newest 50 */
        const rbllm_log_t *l = &S.log[(S.log_head + RBLLM_LOG_N - 1 - k) % RBLLM_LOG_N];
        cJSON *o = cJSON_CreateObject();
        if (l->ts > RBGRID_TS_SANE_MIN) cJSON_AddNumberToObject(o, "ts", l->ts); else cJSON_AddNumberToObject(o, "ts", 0);
        cJSON_AddStringToObject(o, "client", l->client);
        cJSON_AddStringToObject(o, "tool", l->tool);
        cJSON_AddStringToObject(o, "result", l->code);
        cJSON_AddNumberToObject(o, "ms", l->ms);
        cJSON_AddNumberToObject(o, "bytes", l->bytes);
        cJSON_AddBoolToObject(o, "config_changed", l->cfg);
        cJSON_AddItemToArray(la, o);
    }
    cJSON_AddNumberToObject(r, "log_total", S.log_n);
    ulk();
    /* what the page needs to write a client's config snippet (CONNECT-UX §2.2) */
    cJSON *cn = cJSON_AddObjectToObject(r, "connect");
    char id[13]; rbllm_hub_id(id);
    cJSON_AddStringToObject(cn, "hub_id", id);
    char nm[48]; hub_display_name(nm, sizeof(nm));
    cJSON_AddStringToObject(cn, "name", nm);
    uint8_t mac[6] = {0}; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char host[32]; snprintf(host, sizeof(host), "rbgrid-%02x%02x.local", mac[4], mac[5]);   /* = rbgrid_app's mDNS host */
    cJSON_AddStringToObject(cn, "mdns_host", host);
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) {
        char ips[16]; snprintf(ips, sizeof(ips), IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(cn, "ip", ips);
        char u[40]; snprintf(u, sizeof(u), "http://%s/", ips);
        cJSON_AddStringToObject(cn, "url", u);
    } else { cJSON_AddNullToObject(cn, "ip"); cJSON_AddNullToObject(cn, "url"); }
    cJSON_AddStringToObject(cn, "proto", RBLLM_PROTO);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    return s;
}

char *rbllm_admin_set(const char *body, int *status)
{
    cJSON *b = body ? cJSON_Parse(body) : NULL;
    if (!cJSON_IsObject(b)) { cJSON_Delete(b); return jerr(status, 400, "invalid_args", "JSON object {enabled?, read_only?, revoke?} required"); }
    const cJSON *en = cJSON_GetObjectItem(b, "enabled"), *ro = cJSON_GetObjectItem(b, "read_only"), *rv = cJSON_GetObjectItem(b, "revoke");
    const cJSON *pc = cJSON_GetObjectItem(b, "pair_cancel");
    if ((en && !cJSON_IsBool(en)) || (ro && !cJSON_IsBool(ro)) || (rv && !cJSON_IsString(rv)) || (pc && !cJSON_IsString(pc))) {
        cJSON_Delete(b); return jerr(status, 400, "invalid_args", "enabled/read_only: boolean; revoke: token id; pair_cancel: pair id");
    }
    lk();
    if (pc) {   /* Deny on the page: the pending pairing ends now; its code stops working */
        bool hit = false;
        for (int i = 0; i < RBLLM_MAX_PAIRS; i++)
            if (S.pair[i].pair_id[0] && !strcmp(S.pair[i].pair_id, pc->valuestring)) { memset(&S.pair[i], 0, sizeof(S.pair[i])); hit = true; }
        if (!hit) { ulk(); cJSON_Delete(b); return jerr(status, 404, "not_found", "no pending pairing with that id"); }
    }
    bool found = true;
    if (rv) {
        found = false;
        for (int i = 0; i < RBLLM_MAX_TOKENS; i++)
            if (S.tok[i].id[0] && !strcmp(S.tok[i].id, rv->valuestring)) { memset(&S.tok[i], 0, sizeof(S.tok[i])); found = true; }
    }
    if (!found) { ulk(); cJSON_Delete(b); return jerr(status, 404, "not_found", "no token with that id"); }
    if (en) { S.enabled = cJSON_IsTrue(en); if (!S.enabled) memset(S.pair, 0, sizeof(S.pair)); }
    if (ro) S.read_only = cJSON_IsTrue(ro);
    esp_err_t se = save_locked();
    ulk();
    cJSON_Delete(b);
    if (se != ESP_OK) return jerr(status, 500, "internal", "not saved");
    rbpower_diag_printf("llm: API %s, read-only %s%s", S.enabled ? "on" : "off", S.read_only ? "on" : "off", rv ? ", a token revoked" : "");
    *status = 200;
    return rbllm_admin_json();
}
