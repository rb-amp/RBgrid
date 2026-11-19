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
#include "esp_mac.h"
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

static void lk(void)  { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); }
static void ulk(void) { if (s_mx) xSemaphoreGive(s_mx); }
static void ev_boot(void);
static void ev_watch(void);
static void ev_add(uint8_t type, uint8_t sev, uint16_t chan, const char *info);
static void ensure_task(void);

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
    ensure_task();            /* the worker also runs the event watcher, so it starts with the hub */
}

bool rbllm_enabled(void) { return S.enabled; }

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

/* ============================================================================================ call context */

typedef struct {
    int64_t  deadline_us;
    uint8_t  scopes;
    const char *client;
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
} sum_ctx_t;

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

static double r4(double v) { return round(v * 10000.0) / 10000.0; }
static double r2(double v) { return round(v * 100.0) / 100.0; }

/* Parse `period` {from,to} | {preset}. Clips `to` to now. */
static bool parse_period(rbllm_cx_t *cx, const cJSON *p, uint32_t *from, uint32_t *to)
{
    uint32_t now = now_s();
    if (now < RBGRID_TS_SANE_MIN) { fail(cx, "clock_unsynced", "the hub clock is not set yet (just restarted, or no internet for NTP)", "Retry in a few seconds; if it persists, check the hub's internet access."); cx->retry_ms = 5000; return false; }
    if (!cJSON_IsObject(p)) { fail(cx, "invalid_args", "period: object {from,to} or {preset} required", "Pass period:{preset:\"yesterday\"}."); return false; }
    const char *pre = cJSON_GetStringValue(cJSON_GetObjectItem(p, "preset"));
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
    if (*from >= *to) { fail(cx, "invalid_args", "period: from must be before to (and before now)", NULL); return false; }
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

static cJSON *t_get_hub_info(const cJSON *a, rbllm_cx_t *cx)
{
    (void)a;
    cJSON *r = cJSON_CreateObject();
    const esp_app_desc_t *ad = esp_app_get_description();
    char id[13]; rbllm_hub_id(id);
    cJSON_AddStringToObject(r, "hub_id", id);
    rbgrid_json_context_t *jc = rbgrid_core_json_ctx();
    cJSON_AddStringToObject(r, "name", jc && jc->device_info.device_name[0] ? jc->device_info.device_name : "RBgrid");
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
    cJSON *cap = cJSON_AddArrayToObject(r, "capabilities");
    cJSON_AddItemToArray(cap, cJSON_CreateString("read"));
    cJSON_AddItemToArray(cap, cJSON_CreateString("events"));
    cJSON_AddBoolToObject(r, "read_only_mode", S.read_only);
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
        cJSON_AddItemToArray(arr, o);
    }
    free(p);
    note(cx, "Energy direction is reported, not set: a reversed clamp is fixed at the clamp, not in software.");
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
    cJSON_AddNumberToObject(sys, "power_w", r2(st.p_w));
    cJSON_AddStringToObject(sys, "sign", "positive = import from the grid");
    cJSON *arr = cJSON_AddArrayToObject(r, "channels");
    for (int i = 0; i < n; i++) {
        if (!sel[i]) continue;
        cJSON *o = cJSON_CreateObject();
        char h[5]; uid_hex(p[i].unit_id, h);
        cJSON_AddStringToObject(o, "id", h);
        add_name(o, p[i].name);
        cJSON_AddBoolToObject(o, "present", p[i].present);
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
    cJSON *hp = cJSON_AddObjectToObject(r, "heap");
    cJSON_AddNumberToObject(hp, "internal_free_b", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(hp, "internal_min_b", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(hp, "psram_free_b", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    extern TaskHandle_t rbllm_worker_handle(void);
    TaskHandle_t wh = rbllm_worker_handle();
    if (wh) cJSON_AddNumberToObject(hp, "llm_stack_free_b", (double)uxTaskGetStackHighWaterMark(wh) * sizeof(StackType_t));
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
            if (bar) { *bar = 0; cJSON_AddNumberToObject(d, "version", atoi(b)); cJSON_AddStringToObject(d, "source", bar + 1); }
            snprintf(msg, sizeof(msg), "Configuration version %s saved (%s)", b, bar ? bar + 1 : "?");
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
typedef struct { double e_in, e_out, cost, rev; uint32_t newest; bool any; double open_cost, open_rev; } tot_t;
static bool totals(rbllm_cx_t *cx, const rbgrid_point_snap_t *p, int n, const bool sel[], uint32_t from, uint32_t to, tot_t *t)
{
    for (int i = 0; i < n; i++) {
        memset(&t[i], 0, sizeof(t[i]));
        if (!sel[i]) continue;
        bucket_t b = { .from = from };
        sum_ctx_t c = { .b = &b, .nb = 1, .unit = 't', .cx = cx };
        sum_range(&c, p[i].unit_id, from, to);
        if (c.timed_out) return false;
        t[i] = (tot_t){ b.e_in, b.e_out, b.cost, b.rev, c.newest_end, b.recs > 0, c.open_cost, c.open_rev };
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
    cJSON_AddStringToObject(tj, "price_source", tn.external ? "dynamic" : "plan");
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
    if (!parse_period(cx, cJSON_GetObjectItem(a, "a"), &af, &at)) return NULL;
    if (bl) {
        if (!strcmp(bl, "previous_period")) { bt = af; bf = af - (at - af); }
        else if (!strcmp(bl, "previous_month")) { bf = local_add(local_floor(af, 'm'), 'm', -1); bt = bf + (at - af); }
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
    rbgrid_tariff_now_t tn; rbgrid_core_tariff_now(&tn);
    if (tn.placeholder) finding(f, "tariff_not_set", "info", NULL, "No tariff plan is saved; money is computed off a demo plan.", "Enter the tariff on the hub's tariff page.");
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
    cJSON_AddStringToObject(r, "price_state_now", cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "state")) ? cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "state")) : "");
    cJSON_Delete(cfg);
    note(cx, "spot = wholesale day-ahead price without fees and VAT; all_in = the price a supplier charges. Set the source on the hub's dynamic price page.");
    note(cx, "Tariff templates and local terms are not built in yet; ask the user for the supplier's price sheet.");
    cx->as_of = now_s();
    return r;
}

/* ---- get_config: compact, LLM-facing projection of the settings (never the raw files, never a token) ---- */
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
            if (ctn) cJSON_AddStringToObject(src, "ct_model", ctn);
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
    static const char *ZN[RBPOWER_TARIFF_ZONE_COUNT] = { "night", "morning_peak", "day", "evening_peak", "late_evening", "weekend", "holiday", "critical_peak" };
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
    cJSON *zs = cJSON_AddArrayToObject(t, "zones");
    for (int z = 0; z < RBPOWER_TARIFF_ZONE_COUNT; z++) {
        const rbpower_tariff_zone_t *zo = &tp->zones[z];
        if (!zo->is_enabled) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "zone", ZN[z]);
        utf8_clean(zo->zone_name, b, sizeof(b)); cJSON_AddStringToObject(o, "name", b);
        cJSON_AddNumberToObject(o, "buy_per_kwh", r4(zo->rate_buying_per_kwh));
        cJSON_AddNumberToObject(o, "sell_per_kwh", r4(zo->rate_selling_per_kwh));
        cJSON_AddItemToArray(zs, o);
    }
    note(cx, "Zone rates are before fees and VAT; the zone time windows are shown on the hub's tariff page.");
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
    if (sec && strcmp(sec, "channels") && strcmp(sec, "tariff") && strcmp(sec, "notifications") && strcmp(sec, "region"))
        return fail(cx, "invalid_args", "section: channels, tariff, notifications or region", NULL);
    cJSON *r = cJSON_CreateObject();
    char ver[9]; rbgrid_core_config_version(ver);
    cJSON_AddStringToObject(r, "version", ver);
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
    }
    note(cx, "Read-only view. Changing settings over LLM comes with propose_config / apply_config; until then the user changes them on the hub page.");
    cx->as_of = now_s();
    return r;
}

/* ============================================================================================ config writes (stage 2) */
/* Writable over LLM (v1): per channel name / role (load-class roles only; supply roles are topology, not a patch) /
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
    static const char *ZN[RBPOWER_TARIFF_ZONE_COUNT] = { "night", "morning_peak", "day", "evening_peak", "late_evening", "weekend", "holiday", "critical_peak" };
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
        for (int z = 0; z < RBPOWER_TARIFF_ZONE_COUNT; z++) {
            if (!tp->zones[z].is_enabled) continue;
            cJSON *o = cJSON_AddObjectToObject(zs, ZN[z]);
            cJSON_AddNumberToObject(o, "buy_per_kwh", r4(tp->zones[z].rate_buying_per_kwh));
            cJSON_AddNumberToObject(o, "sell_per_kwh", r4(tp->zones[z].rate_selling_per_kwh));
        }
    }
    return r;
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
    if (!lh || strcmp(lh, h)) {
        int next = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(v, "next"));
        if (next < 1) next = 1;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "version", next);
        cJSON_AddNumberToObject(e, "ts", now_s());
        cJSON_AddStringToObject(e, "source", source);
        cJSON_AddStringToObject(e, "summary", summary ? summary : (last ? "changed outside the LLM API" : "first recorded state"));
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

/* Validate a channels patch against the current state; fill diff/warnings; return a normalized patch or NULL (fail set). */
static cJSON *check_patch(rbllm_cx_t *cx, const cJSON *patch, const cJSON *cur, cJSON *diff, cJSON *warn)
{
    const cJSON *pch = cJSON_GetObjectItem(patch, "channels"), *pcd = cJSON_GetObjectItem(patch, "peaks_cycle_day"),
                *ptf = cJSON_GetObjectItem(patch, "tariff");
    if (!pch && !pcd && !ptf) return fail(cx, "invalid_args", "patch: {channels: {<id>: {...}}}, {tariff: {...}} and/or {peaks_cycle_day}", NULL);
    {   /* unknown top-level keys */
        const cJSON *k;
        cJSON_ArrayForEach(k, patch)
            if (strcmp(k->string, "channels") && strcmp(k->string, "peaks_cycle_day") && strcmp(k->string, "tariff")) {
                char mm[96]; snprintf(mm, sizeof(mm), "patch.%.30s: not writable over LLM (channels, tariff, peaks_cycle_day)", k->string);
                return fail(cx, "invalid_args", mm, NULL);
            }
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
                if (r < 0) { snprintf(m, sizeof(m), "channels.%s.role: one of device_load, zone_load, circuit_breaker, ev_charging, heating_cooling, smart_appliance, renewable_source, storage_system", id); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
                int cr = role_from_name(was);
                bool cs = cr == RBPOWER_ROLE_MAIN_SUPPLY || cr == RBPOWER_ROLE_ALTERNATIVE_SUPPLY, ns = r == RBPOWER_ROLE_MAIN_SUPPLY || r == RBPOWER_ROLE_ALTERNATIVE_SUPPLY;
                if ((cs || ns) && r != cr) { snprintf(m, sizeof(m), "channels.%s.role: a supply role (main / alternative supply) is the hub's topology and is changed on the hub's points page", id); cJSON_Delete(norm); return fail(cx, "invalid_args", m, "Ask the user to change it on the hub page."); }
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
            } else { snprintf(m, sizeof(m), "channels.%s.%.20s: not writable over LLM (name, role, inside, ct_model)", id, k); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
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
                cJSON_ArrayForEach(z, f) {
                    const cJSON *cz = cJSON_GetObjectItem(cJSON_GetObjectItem(ct, "zones"), z->string);
                    if (!cz) { snprintf(m, sizeof(m), "tariff.zones.%.20s: not a zone of the plan (see get_config tariff)", z->string); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, "Zone time windows are set on the hub's tariff page."); }
                    const cJSON *r;
                    cJSON *oz = NULL;
                    cJSON_ArrayForEach(r, z) {
                        if (strcmp(r->string, "buy_per_kwh") && strcmp(r->string, "sell_per_kwh")) { snprintf(m, sizeof(m), "tariff.zones.%s.%.20s: buy_per_kwh or sell_per_kwh", z->string, r->string); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
                        if (!cJSON_IsNumber(r) || r->valuedouble < 0 || r->valuedouble > 100) { snprintf(m, sizeof(m), "tariff.zones.%s.%s: 0..100 per kWh (before fees and VAT)", z->string, r->string); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, "A price per MWh must be divided by 1000."); }
                        double was = cJSON_GetNumberValue(cJSON_GetObjectItem(cz, r->string));
                        if (fabs(was - r->valuedouble) < 1e-6) continue;
                        if (!oz) oz = cJSON_AddObjectToObject(nz, z->string);
                        cJSON_AddNumberToObject(oz, r->string, r->valuedouble);
                        cJSON *d = cJSON_CreateObject(); snprintf(path, sizeof(path), "tariff.zones.%s.%s", z->string, r->string);
                        cJSON_AddStringToObject(d, "path", path); cJSON_AddNumberToObject(d, "from", was); cJSON_AddNumberToObject(d, "to", r->valuedouble);
                        cJSON_AddItemToArray(diff, d);
                    }
                }
            } else {
                size_t q; bool hit = false;
                for (q = 0; q < sizeof(NUMS) / sizeof(NUMS[0]); q++) if (!strcmp(k, NUMS[q].k)) { hit = true; break; }
                if (!hit) { snprintf(m, sizeof(m), "tariff.%.24s: not writable over LLM (plan_name, provider, currency, vat_percent, energy_tax_per_kwh, grid_fee_per_kwh, levy_per_kwh, zones)", k); cJSON_Delete(nt); cJSON_Delete(norm); return fail(cx, "invalid_args", m, NULL); }
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
    if (!cJSON_GetArraySize(diff)) { cJSON_Delete(norm); return fail(cx, "invalid_args", "the patch changes nothing", "Read get_config first."); }
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
    cJSON_ArrayForEach(z, cJSON_GetObjectItem(nt, "zones")) {
        char up[24]; strlcpy(up, z->string, sizeof(up)); for (char *q = up; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
        cJSON *dz = NULL, *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(plan, "zones")) { const char *zid = cJSON_GetStringValue(cJSON_GetObjectItem(e, "zone_id")); if (zid && !strcmp(zid, up)) dz = e; }
        if (!dz) { cJSON_Delete(d); snprintf(m, mlen, "tariff.zones.%s: zone not in the plan document", z->string); return false; }
        const cJSON *b = cJSON_GetObjectItem(z, "buy_per_kwh"), *sl = cJSON_GetObjectItem(z, "sell_per_kwh");
        if (b) cJSON_ReplaceItemInObject(dz, "rate_buying_per_kwh", cJSON_CreateNumber(b->valuedouble));
        if (sl) cJSON_ReplaceItemInObject(dz, "rate_selling_per_kwh", cJSON_CreateNumber(sl->valuedouble));
    }
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
            if (!cz) continue;
            cJSON *one = NULL;
            const char *ks[] = { "buy_per_kwh", "sell_per_kwh" };
            for (int q = 0; q < 2; q++) {
                double x = cJSON_GetNumberValue(cJSON_GetObjectItem(z, ks[q])), y = cJSON_GetNumberValue(cJSON_GetObjectItem(cz, ks[q]));
                if (fabs(x - y) > 1e-6) { if (!one) one = cJSON_AddObjectToObject(zo, z->string); cJSON_AddNumberToObject(one, ks[q], x); }
            }
        }
        if (cJSON_GetArraySize(zo)) cJSON_AddItemToObject(o, "zones", zo); else cJSON_Delete(zo);
        if (cJSON_GetArraySize(o)) cJSON_AddItemToObject(p, "tariff", o); else cJSON_Delete(o);
    }
    return p;
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
    cJSON *norm = check_patch(cx, patch, cur, diff, warn);
    cJSON_Delete(own); cJSON_Delete(cur);
    if (!norm) { cJSON_Delete(diff); cJSON_Delete(warn); return NULL; }
    /* dedupe warnings */
    cJSON *w2 = cJSON_CreateArray(); const cJSON *w;
    cJSON_ArrayForEach(w, warn) { bool dup = false; const cJSON *x; cJSON_ArrayForEach(x, w2) if (!strcmp(x->valuestring, w->valuestring)) dup = true;
                                  if (!dup) cJSON_AddItemToArray(w2, cJSON_CreateString(w->valuestring)); }
    cJSON_Delete(warn);
    if (!summary[0]) snprintf(summary, sizeof(summary), "%d change(s)", cJSON_GetArraySize(diff));
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
    cJSON_AddBoolToObject(r, "restart_required", false);
    cJSON_AddStringToObject(r, "confirm_token", tok);
    cJSON_AddNumberToObject(r, "expires_in", PROP_TTL_S);
    cJSON_AddStringToObject(r, "base_version", base);
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
            } else cJSON_AddStringToObject(body, f->string, f->valuestring);
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
    ver_sync("hub", NULL);   /* the state before, if not yet recorded */
    bool ok = apply_patch(cx, p.patch, cur);
    cJSON_Delete(cur); cJSON_Delete(p.patch);
    if (!ok) return NULL;
    char src[48]; snprintf(src, sizeof(src), "llm:%.40s", p.client[0] ? p.client : "client");
    int v = ver_sync(src, p.summary);
    rbpower_diag_printf("llm: config version %d applied by '%s' (%s)", v, p.client, p.summary);
    s_cfg_changed = true;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "version", v);
    cJSON_AddBoolToObject(r, "applied", true);
    cJSON_AddBoolToObject(r, "restart_required", false);
    cJSON_AddStringToObject(r, "summary", p.summary);
    note(cx, "Applied. list_config_versions shows it; propose_config {rollback_to} undoes it.");
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
    note(cx, "Versions cover what can be changed over LLM: channel names, roles, inside, clamp models, the tariff prices/VAT/fees and the peaks cycle day.");
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
    if (!sec || (strcmp(sec, "channels") && strcmp(sec, "notifications") && strcmp(sec, "tariff") && strcmp(sec, "region")))
        return fail(cx, "invalid_args", "section: channels, notifications, tariff or region", NULL);
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
        for (int q = 0; q < 10; q++) if (q != RBPOWER_ROLE_MAIN_SUPPLY && q != RBPOWER_ROLE_ALTERNATIVE_SUPPLY) cJSON_AddItemToArray(en, cJSON_CreateString(role_name((uint8_t)q)));
        cJSON_AddStringToObject(f, "description", "What the point measures. main_supply / alternative_supply are set on the hub page, not here.");
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
    } else if (!strcmp(sec, "notifications")) {
        cJSON *f = cJSON_AddObjectToObject(w, "peaks_cycle_day");
        cJSON_AddStringToObject(f, "type", "integer"); cJSON_AddNumberToObject(f, "minimum", 1); cJSON_AddNumberToObject(f, "maximum", 28);
        cJSON_AddStringToObject(f, "description", "Day of the month the import-peak cycle starts (the grid operator's billing day).");
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
        cJSON_AddStringToObject(f, "description", "{<zone>: {buy_per_kwh?, sell_per_kwh?}} for the zones get_config lists; 0..100 per kWh, before fees and VAT. Zone time windows are set on the hub page.");
        note(cx, "A flat tariff on a two-zone plan: give every zone the same price.");
    } else {
        note(cx, "Nothing in this section is writable over LLM yet; the user changes it on the hub page.");
    }
    cJSON_AddStringToObject(r, "how", "propose_config {patch: {channels: {<id>: {field: value}}}}, {patch: {tariff: {field: value}}} or {patch: {peaks_cycle_day: n}}");
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
        cJSON_AddStringToObject(r, "next", "Ask the user to switch ONE appliance of at least about 100 W on (or off) now and leave it for 10 s. "
                                           "Then call identify_channel with action \"result\". Other big loads should stay as they are.");
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
    free(p);
    /* the verdict */
    const char *verdict = "no_step";
    if (m > 0) {
        int k = ord[0];
        float s1 = id_step(s.base[k], s.mn[k], s.mx[k]);
        float s2 = m > 1 ? fabsf(id_step(s.base[ord[1]], s.mn[ord[1]], s.mx[ord[1]])) : 0.0f;
        if (fabsf(s1) >= ID_MIN_STEP_W) {
            verdict = (fabsf(s1) >= 2.0f * s2) ? "identified" : "ambiguous";
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
    cJSON_AddStringToObject(r, "verdict", verdict);
    if (!strcmp(verdict, "no_step") && s.running)
        note(cx, "No step of 60 W or more yet. Ask the user whether the appliance is on, then call result again (the window is still open).");
    else if (!strcmp(verdict, "no_step"))
        note(cx, "No step of 60 W or more in the window. Start again with a bigger appliance (kettle, heater).");
    else if (!strcmp(verdict, "ambiguous"))
        note(cx, "Two channels moved by a similar amount: ask the user to switch the appliance off again and call start once more.");
    cx->as_of = now_s(); cx->resolution = "live";
    return r;
}

static const rbllm_tool_t TOOLS[] = {
    { "get_hub_info", RBLLM_SCOPE_READ,
      "When to call: first, to learn the hub's time zone, currency, firmware and what it can do. What it returns: hub id, name, firmware, "
      "protocol version, board, hub time with offset, time zone, currency, uptime, capabilities.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"hub_id\":{\"type\":\"string\"},\"fw\":{\"type\":\"string\"},\"timezone\":{\"type\":\"string\"},\"currency\":{\"type\":\"string\"},\"hub_time\":{\"type\":\"string\"}}}",
      t_get_hub_info },
    { "list_channels", RBLLM_SCOPE_READ,
      "When to call: before any per-channel question, to map names to ids. What it returns: every measuring point with id, the user's name, "
      "role (main = grid supply, load, generation, bidirectional), whether its source is present and whether it has stored history.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"channels\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"role\":{\"type\":\"string\"},\"present\":{\"type\":\"boolean\"},\"has_history\":{\"type\":\"boolean\"}}}}}}",
      t_list_channels },
    { "read_live", RBLLM_SCOPE_READ,
      "When to call: for what is happening right now. What it returns: the grid power (positive = import) and per channel the power in W, "
      "current in A and voltage in V, from the hub's 3-second data.",
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
      "and percent, and the cost difference. Give b, or a baseline relative to a.",
      "{\"type\":\"object\",\"properties\":{\"a\":" S_PERIOD ",\"b\":" S_PERIOD ",\"baseline\":{\"type\":\"string\",\"enum\":[\"previous_period\",\"previous_month\",\"same_month_last_year\"]},"
      "\"channels\":" S_CHANNELS "},\"required\":[\"a\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"channels\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"delta_kwh\":{\"type\":\"number\"},\"delta_pct\":{\"type\":[\"number\",\"null\"]}}}}}}",
      t_compare_periods },
    { "get_load_profile", RBLLM_SCOPE_READ,
      "When to call: for when power was high or low over a period on one channel (shape of the day, peaks, standby). What it returns: rows "
      "of {from, avg_w, min_w, max_w} per stored hour or day; min/max are null where the record has no extremes.",
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
      "no_main_supply, channel_absent, module_missing, no_voltage, ct_reversed, ct_rating_mismatch, sum_mismatch, clock_unsynced, "
      "tariff_not_set. Detects only; a reversed clamp is fixed at the clamp.",
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
      "When to call: to explain how the hub is set up (points and their clamps, tariff, price rules, time zone) before advising a change. "
      "What it returns: the chosen section(s) as a compact view plus the config version; no secrets.",
      "{\"type\":\"object\",\"properties\":{\"section\":{\"type\":\"string\",\"enum\":[\"channels\",\"tariff\",\"notifications\",\"region\"]}},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"version\":{\"type\":\"string\"},\"channels\":{\"type\":\"array\"},\"tariff\":{\"type\":\"object\"}}}",
      t_get_config },
    { "get_config_schema", RBLLM_SCOPE_CONFIG,
      "When to call: before propose_config, to learn which fields of a section can be changed and their allowed values (roles, clamp "
      "models). What it returns: the writable fields with type, limits, enums and descriptions.",
      "{\"type\":\"object\",\"properties\":{\"section\":{\"type\":\"string\",\"enum\":[\"channels\",\"notifications\",\"tariff\",\"region\"]}},\"required\":[\"section\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"writable\":{\"type\":\"object\"}}}",
      t_get_config_schema },
    { "propose_config", RBLLM_SCOPE_CONFIG,
      "When to call: when the user wants to rename a channel, change its role, parent or clamp model, enter their tariff (prices, VAT, fees), set the peaks cycle day, or undo a change. "
      "What it returns: the diff, warnings and a one-time confirm_token (10 min). Changes nothing; show the diff and ask the user.",
      "{\"type\":\"object\",\"properties\":{\"patch\":{\"type\":\"object\",\"description\":\"{channels:{<id>:{name?,role?,inside?,ct_model?}}, tariff:{plan_name?,provider?,currency?,vat_percent?,energy_tax_per_kwh?,grid_fee_per_kwh?,levy_per_kwh?,zones?:{<zone>:{buy_per_kwh?,sell_per_kwh?}}}, peaks_cycle_day?}\"},"
      "\"rollback_to\":{\"type\":\"integer\",\"description\":\"a version from list_config_versions\"}},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"diff\":{\"type\":\"array\"},\"warnings\":{\"type\":\"array\"},\"confirm_token\":{\"type\":\"string\"},\"expires_in\":{\"type\":\"integer\"}}}",
      t_propose_config },
    { "apply_config", RBLLM_SCOPE_CONFIG,
      "When to call: only after the user said yes to a propose_config diff. What it returns: the new version number. Fails with conflict "
      "if the settings changed since the proposal; nothing is half applied.",
      "{\"type\":\"object\",\"properties\":{\"confirm_token\":{\"type\":\"string\"}},\"required\":[\"confirm_token\"],\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"version\":{\"type\":\"integer\"},\"applied\":{\"type\":\"boolean\"},\"restart_required\":{\"type\":\"boolean\"}}}",
      t_apply_config },
    { "list_config_versions", RBLLM_SCOPE_CONFIG,
      "When to call: when the user asks what changed, or before a rollback. What it returns: the last 5 versions, newest first, with time, "
      "source (hub, llm:<client>) and a summary.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"versions\":{\"type\":\"array\"}}}",
      t_list_config_versions },
    { "rollback_config", RBLLM_SCOPE_CONFIG,
      "When to call: to finish an undo the user confirmed. Takes the confirm_token from propose_config {rollback_to}. What it returns: "
      "as apply_config.",
      "{\"type\":\"object\",\"properties\":{\"version\":{\"type\":\"integer\"},\"confirm_token\":{\"type\":\"string\"}},\"additionalProperties\":false}",
      "{\"type\":\"object\",\"properties\":{\"version\":{\"type\":\"integer\"},\"applied\":{\"type\":\"boolean\"}}}",
      t_rollback_config },
    { "get_analysis_playbook", RBLLM_SCOPE_READ,
      "When to call: when the user asks for a report or a check and you want the hub's recipe. What it returns: the steps (tools with argument "
      "hints), questions to ask the user and rules for the answer.",
      "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"enum\":[\"monthly_report\",\"installation_check\"]}},\"required\":[\"id\"],\"additionalProperties\":false}",
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
    rbgrid_json_context_t *jc = rbgrid_core_json_ctx();
    cJSON_AddStringToObject(hub, "name", jc && jc->device_info.device_name[0] ? jc->device_info.device_name : "RBgrid");
    const esp_app_desc_t *ad = esp_app_get_description();
    cJSON_AddStringToObject(hub, "fw", ad ? ad->version : "");
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
        bool ro = TOOLS[i].scope == RBLLM_SCOPE_READ || !strncmp(TOOLS[i].name, "get_", 4) || !strcmp(TOOLS[i].name, "propose_config") ||
                  !strcmp(TOOLS[i].name, "list_config_versions");   /* config scope but no change */
        cJSON_AddBoolToObject(an, "readOnlyHint", ro);
        cJSON_AddBoolToObject(an, "destructiveHint", false);
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
                      .scopes = caller ? caller->scopes : 0, .client = caller ? caller->client : "" };
    cJSON *req = (len && len <= RBLLM_MAX_REQUEST_BYTES) ? cJSON_ParseWithLength(body, len) : NULL;
    const char *rid = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "request_id")) : NULL;
    char ridb[65] = {0};
    if (rid) utf8_clean(rid, ridb, sizeof(ridb));   /* echoed back: well-formed UTF-8 only, never cut inside a character */
    const char *tname = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "tool")) : NULL;
    char tnb[28] = {0};
    if (tname) utf8_clean(tname, tnb, sizeof(tnb));
    const rbllm_tool_t *t = NULL;
    cJSON *res = NULL;
    if (!req || !cJSON_IsObject(req)) fail(&cx, "invalid_args", "body: JSON object {tool, args, request_id} required (max 4 KB)", NULL);
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
        } else if (S.read_only && (!strcmp(t->name, "apply_config") || !strcmp(t->name, "rollback_config") || !strcmp(t->name, "propose_config")))
            fail(&cx, "read_only_mode", "the hub is in read-only mode for LLM clients", "The user can switch read-only off on the hub's AI / LLM page.");
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
static StaticTask_t s_task_buf;
static StackType_t  s_task_stack[RBLLM_STACK];   /* internal RAM: the tools read flash (cache off) — no PSRAM stack */
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
    if (s_task) return;
    s_q = xQueueCreateStatic(sizeof(s_q_store) / sizeof(rbllm_job_t), sizeof(rbllm_job_t), s_q_store, &s_q_buf);
    s_task = xTaskCreateStatic(worker, "rbllm", RBLLM_STACK, NULL, 2, s_task_stack, &s_task_buf);
}

bool rbllm_submit(char *body, size_t len, const rbllm_caller_t *caller, void (*done)(char *, void *), void *ctx)
{
    if (!s_task) {
        if (!s_mx) rbllm_init();
        ensure_task();
    }
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

char *rbllm_pair_start(const char *body, int *status)
{
    if (!S.enabled) return jerr(status, 403, "llm_disabled", "the LLM API is off; switch it on on the hub's AI / LLM page");
    cJSON *b = body ? cJSON_Parse(body) : NULL;
    const char *nm = b ? cJSON_GetStringValue(cJSON_GetObjectItem(b, "client_name")) : NULL;
    uint8_t sc = b ? scopes_from_json(cJSON_GetObjectItem(b, "scopes")) : 0;
    if (!nm || !nm[0] || strlen(nm) > 32 || !sc) { cJSON_Delete(b); return jerr(status, 400, "invalid_args", "client_name (1..32 chars) and scopes [read|config] required"); }
    for (const char *c = nm; *c; c++) if ((unsigned char)*c < 0x20) { cJSON_Delete(b); return jerr(status, 400, "invalid_args", "client_name: no control characters"); }
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
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    return s;
}

char *rbllm_admin_set(const char *body, int *status)
{
    cJSON *b = body ? cJSON_Parse(body) : NULL;
    if (!cJSON_IsObject(b)) { cJSON_Delete(b); return jerr(status, 400, "invalid_args", "JSON object {enabled?, read_only?, revoke?} required"); }
    const cJSON *en = cJSON_GetObjectItem(b, "enabled"), *ro = cJSON_GetObjectItem(b, "read_only"), *rv = cJSON_GetObjectItem(b, "revoke");
    if ((en && !cJSON_IsBool(en)) || (ro && !cJSON_IsBool(ro)) || (rv && !cJSON_IsString(rv))) {
        cJSON_Delete(b); return jerr(status, 400, "invalid_args", "enabled/read_only: boolean; revoke: token id");
    }
    lk();
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
