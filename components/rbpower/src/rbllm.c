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
    char     client[24];
    char     tool[28];
    char     code[20];     /* "ok" or the error code */
    uint16_t ms;
    uint16_t bytes;
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

static void sum_emit(const rbpower_period_statistics_t *s, void *v, bool is_hour)
{
    sum_ctx_t *c = (sum_ctx_t *)v;
    if (c->timed_out) return;
    uint32_t ts = (uint32_t)(s->period_start_us / 1000000LL);
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
    if (ts + dur > c->newest_end) c->newest_end = ts + dur;
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
    rbgrid_foreach_valid_record_ex(u, RBPOWER_STAT_PERIOD_1HOUR, RBPOWER_PERIOD_HOUR, a, b - 1, true,
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
        rbgrid_foreach_valid_record_ex(u, RBPOWER_STAT_PERIOD_DAILY, RBPOWER_PERIOD_DAY, d0, d1 - 1, true,
                                       c->cx->deadline_us, sum_emit_d, c);
        hour_seg(c, u, from, d0, hmin);   /* partial first day */
        hour_seg(c, u, d1, to, hmin);     /* partial last day / today */
    } else {
        hour_seg(c, u, from, to, hmin);
    }
    if (late(c->cx)) c->timed_out = true;
}

static double r4(double v) { return round(v * 10000.0) / 10000.0; }
static double r2(double v) { return round(v * 100.0) / 100.0; }

/* Parse `period` {from,to} | {preset}. Clips `to` to now. */
static bool parse_period(rbllm_cx_t *cx, const cJSON *p, uint32_t *from, uint32_t *to)
{
    uint32_t now = now_s();
    if (now < RBGRID_TS_SANE_MIN) { fail(cx, "internal", "the hub clock is not set", "Wait until the hub has synced its clock."); return false; }
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

static cJSON *t_get_events(const cJSON *a, rbllm_cx_t *cx)
{
    if (!s_events_reader) return fail(cx, "internal", "event log not available in this build", NULL);
    const cJSON *js = cJSON_GetObjectItem(a, "since"), *jl = cJSON_GetObjectItem(a, "limit");
    uint32_t since = cJSON_IsNumber(js) && js->valuedouble > 0 ? (uint32_t)js->valuedouble : 0;
    uint32_t lim = cJSON_IsNumber(jl) ? (uint32_t)jl->valuedouble : 30;
    if (lim < 1) lim = 1;
    if (lim > 50) lim = 50;
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
        cJSON_AddStringToObject(o, "severity", lv && !strcmp(lv, "fail") ? "warning" : "info");
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
    if (u == 'h' && now_s() - from > HOUR_DEPTH_S)
        return fail(cx, "out_of_history", "hourly history is kept for 60 days", "Use resolution=day for older ranges.");

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
        cJSON_AddNumberToObject(tot, "energy_kwh", r4(ti / 1000.0));
        cJSON_AddNumberToObject(tot, "export_kwh", r4(te / 1000.0));
        if (empty) { cx->gaps = true; note(cx, "Rows with null have no stored record (hub off, point added later, or beyond the history depth)."); }
        if (!c.hour_secs && !c.day_secs) cx->gaps = true;
        if (c.newest_end > newest) newest = c.newest_end;
        cJSON_AddItemToArray(arr, o);
        free(b);
    }
    free(p);
    cx->as_of = newest;
    if (newest && to > newest + 60) note(cx, "Data runs up to as_of: the open hour is booked when it closes.");
    cx->resolution = res_str(u);
    return r;
}

/* Totals per selected point over [from,to): fills the out arrays. Returns false on timeout. */
typedef struct { double e_in, e_out, cost, rev; uint32_t newest; bool any; } tot_t;
static bool totals(rbllm_cx_t *cx, const rbgrid_point_snap_t *p, int n, const bool sel[], uint32_t from, uint32_t to, tot_t *t)
{
    for (int i = 0; i < n; i++) {
        memset(&t[i], 0, sizeof(t[i]));
        if (!sel[i]) continue;
        bucket_t b = { .from = from };
        sum_ctx_t c = { .b = &b, .nb = 1, .unit = 't', .cx = cx };
        sum_range(&c, p[i].unit_id, from, to);
        if (c.timed_out) return false;
        t[i] = (tot_t){ b.e_in, b.e_out, b.cost, b.rev, c.newest_end, b.recs > 0 };
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
    if (mains) {
        cJSON_AddNumberToObject(r, "total_cost", r4(cost));
        cJSON_AddNumberToObject(r, "import_kwh", r4(imp / 1000.0));
        cJSON_AddNumberToObject(r, "export_revenue", r4(rev));
        cJSON_AddNumberToObject(r, "export_kwh", r4(exp / 1000.0));
        cJSON_AddNumberToObject(r, "net_cost", r4(cost - rev));
        if (!any) { cx->gaps = true; note(cx, "No stored record of the main supply in this period."); }
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
        cJSON_AddNumberToObject(o, "cost", r4(t[i].cost));
        cJSON_AddNumberToObject(o, "energy_kwh", r4(t[i].e_in / 1000.0));
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
        cJSON_AddNumberToObject(o, "a_energy_kwh", r4(ta[i].e_in / 1000.0));
        cJSON_AddNumberToObject(o, "b_energy_kwh", r4(tb[i].e_in / 1000.0));
        double d = (ta[i].e_in - tb[i].e_in) / 1000.0;
        cJSON_AddNumberToObject(o, "delta_kwh", r4(d));
        if (tb[i].e_in > 1.0) cJSON_AddNumberToObject(o, "delta_pct", round(1000.0 * d * 1000.0 / tb[i].e_in) / 10.0);
        else cJSON_AddNullToObject(o, "delta_pct");
        cJSON_AddNumberToObject(o, "delta_cost", r4(ta[i].cost - tb[i].cost));
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
    { "get_events", RBLLM_SCOPE_READ,
      "When to call: to explain a gap, a missing module or a warning. What it returns: the hub's event and warning lines of this boot, newest "
      "last, with a cursor (next) for the following call.",
      "{\"type\":\"object\",\"properties\":{\"since\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"next from the previous call; omit for the oldest kept\"},"
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
    "{\"id\":\"installation_check\",\"title\":\"Installation check\",\"steps\":[{\"tool\":\"get_health\"},{\"tool\":\"list_channels\"},"
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
        bool ro = TOOLS[i].scope == RBLLM_SCOPE_READ;
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
        l->ts = now_s();
        strlcpy(l->client, client ? client : "", sizeof(l->client));
        strlcpy(l->tool, tool ? tool : "", sizeof(l->tool));
        strlcpy(l->code, code, sizeof(l->code));
        l->ms = (uint16_t)(us / 1000 > 65535 ? 65535 : us / 1000);
        l->bytes = (uint16_t)(bytes > 65535 ? 65535 : bytes);
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
    if (rid) strlcpy(ridb, rid, sizeof(ridb));
    const char *tname = req ? cJSON_GetStringValue(cJSON_GetObjectItem(req, "tool")) : NULL;
    char tnb[28] = {0};
    if (tname) strlcpy(tnb, tname, sizeof(tnb));
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
        } else if (t->scope == RBLLM_SCOPE_CONFIG && S.read_only) fail(&cx, "read_only_mode", "the hub is in read-only mode for LLM clients", NULL);
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
                                     cx.err_code ? cx.err_msg : "out of memory", cx.err_hint, 0);
    log_call(cx.client, tnb[0] ? tnb : "?", cx.err_code ? cx.err_code : "ok", esp_timer_get_time() - t0, out ? strlen(out) : 0);
    return out;
}

/* ============================================================================================ worker */

typedef struct {
    char *body; size_t len; rbllm_caller_t caller;
    void (*done)(char *, void *); void *ctx; int64_t deadline_us;
} rbllm_job_t;

#define RBLLM_STACK 6144
static StaticTask_t s_task_buf;
static StackType_t  s_task_stack[RBLLM_STACK];   /* internal RAM: the tools read flash (cache off) — no PSRAM stack */
static TaskHandle_t s_task;
static QueueHandle_t s_q;
static StaticQueue_t s_q_buf;
static uint8_t s_q_store[(RBLLM_MAX_CONCURRENT - 1 > 0 ? RBLLM_MAX_CONCURRENT - 1 : 1) * sizeof(rbllm_job_t)];
static volatile bool s_busy;

static void worker(void *arg)
{
    (void)arg;
    rbllm_job_t j;
    for (;;) {
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE) continue;
        s_busy = true;
        char *rep = (esp_timer_get_time() > j.deadline_us)
                        ? rbllm_error_json(NULL, "timeout", "the call waited too long in the queue", "Retry.", 0)
                        : rbllm_call(j.body, j.len, &j.caller, j.deadline_us);
        free(j.body);
        j.done(rep, j.ctx);
        s_busy = false;
    }
}

bool rbllm_submit(char *body, size_t len, const rbllm_caller_t *caller, void (*done)(char *, void *), void *ctx)
{
    if (!s_task) {
        if (!s_mx) rbllm_init();
        s_q = xQueueCreateStatic(sizeof(s_q_store) / sizeof(rbllm_job_t), sizeof(rbllm_job_t), s_q_store, &s_q_buf);
        s_task = xTaskCreateStatic(worker, "rbllm", RBLLM_STACK, NULL, 2, s_task_stack, &s_task_buf);
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
        cJSON_AddNumberToObject(o, "ts", l->ts);
        cJSON_AddStringToObject(o, "client", l->client);
        cJSON_AddStringToObject(o, "tool", l->tool);
        cJSON_AddStringToObject(o, "result", l->code);
        cJSON_AddNumberToObject(o, "ms", l->ms);
        cJSON_AddNumberToObject(o, "bytes", l->bytes);
        cJSON_AddBoolToObject(o, "config_changed", false);
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
