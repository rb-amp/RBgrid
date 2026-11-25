/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbled.c
 * @brief Hub status LEDs (green / red): state registry, patterns, GET/POST /api/v1/led.
 *
 * Registry (documents/architecture/LED_INDICATION.md):
 *   green  G1 lamp test · G2 booting · G6 storage preparing 3x · G5 pairing window 2x · G4 Wi-Fi connecting FAST ·
 *          G3 setup (no Wi-Fi saved) SLOW · G7 normal BEAT
 *   red    R1 no module answers 1x · R4 statistics storage failed 4x · R2 a module is missing 2x ·
 *          R8 firmware update rolled back 8x · R7 unexpected restart 7x · R5 Wi-Fi unreachable > 5 min 5x ·
 *          R6 MQTT broker unreachable > 5 min 6x · R3 clock not set 3x
 *   both   P1 factory reset ON · P2 firmware update ALT-F · P3 identify BOTH-F · P4 module update ALT-S
 * Mode: full · quiet (no heartbeat) · off (only P1, P2, P3 and R1).
 *
 * One static task (no heap stack, boot-RAM rule): GPIO every 50 ms, registry every 500 ms. Active high.
 */
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"
#include "rbled.h"
#include "rbgrid_core.h"
#include "rbpower.h"            /* rbtime_is_time_valid */
#include "rbstats_migrate.h"
#include "rbstorage_media.h"
#include "rbllm.h"
#if CONFIG_RBGRID_IF_WIFI
#include "rbwifi.h"
#endif
#if CONFIG_RBGRID_IF_MQTT
#include "rbmqtt.h"
#endif
#if CONFIG_RBGRID_IF_HTTP
#include "esp_http_server.h"
#include "rbwebserver.h"
extern void add_cors_headers(httpd_req_t *req);
#endif

static const char *TAG = "rbled";

#define TICK_MS        50
#define EVAL_EVERY     10            /* ticks: 500 ms */
#define LAMP_MS        500
#define EVENT_SHOW_US  (24LL * 3600 * 1000000)
#define NET_GRACE_US   (300LL * 1000000)   /* R5 / R6 after 5 min */
#define MOD_GRACE_US   (30LL * 1000000)    /* R1 / R2 not during the first 30 s */
#define TIME_GRACE_US  (120LL * 1000000)   /* R3 */

typedef enum { PAT_OFF, PAT_ON, PAT_SLOW, PAT_FAST, PAT_BEAT, PAT_N, PAT_ALT_F, PAT_ALT_S, PAT_BOTH_F } pat_t;
static const char *const PAT_NAME[] = { "off", "on", "slow", "fast", "heartbeat", "flashes", "alternate_fast", "alternate_slow", "together_fast" };

typedef struct { const char *id; const char *state; pat_t pat; uint8_t n; } entry_t;

static const entry_t G_LAMP    = { "G1", "lamp test",                         PAT_ON,   0 };
static const entry_t G_BOOT    = { "G2", "booting",                           PAT_ON,   0 };
static const entry_t G_SETUP   = { "G3", "setup: no Wi-Fi saved",             PAT_SLOW, 0 };
static const entry_t G_CONN    = { "G4", "connecting to Wi-Fi",               PAT_FAST, 0 };
static const entry_t G_PAIR    = { "G5", "pairing window open",               PAT_N,    2 };
static const entry_t G_STORE   = { "G6", "statistics storage preparing",      PAT_N,    3 };
static const entry_t G_NORMAL  = { "G7", "normal operation",                  PAT_BEAT, 0 };
static const entry_t R_NOMOD   = { "R1", "no module answers",                 PAT_N,    1 };
static const entry_t R_MISSING = { "R2", "a module is missing",               PAT_N,    2 };
static const entry_t R_CLOCK   = { "R3", "clock not set",                     PAT_N,    3 };
static const entry_t R_STORE   = { "R4", "statistics storage failed",         PAT_N,    4 };
static const entry_t R_WIFI    = { "R5", "Wi-Fi unreachable",                 PAT_N,    5 };
static const entry_t R_MQTT    = { "R6", "MQTT broker unreachable",           PAT_N,    6 };
static const entry_t R_RESTART = { "R7", "unexpected restart",                PAT_N,    7 };
static const entry_t R_ROLLBK  = { "R8", "firmware update rolled back",       PAT_N,    8 };
static const entry_t P_FACTORY = { "P1", "factory reset",                     PAT_ON,   0 };
static const entry_t P_OTA     = { "P2", "firmware update",                   PAT_ALT_F, 0 };
static const entry_t P_IDENT   = { "P3", "identify",                          PAT_BOTH_F, 0 };
static const entry_t P_MODOTA  = { "P4", "module firmware update",            PAT_ALT_S, 0 };

static int s_gpio_g = -1, s_gpio_r = -1;
static rbled_probe_t s_probe;
static volatile bool s_started, s_boot_done, s_proc_factory, s_proc_ota, s_rollback, s_acked;
static bool s_unexpected_reset;
static volatile int64_t s_ident_until_us;
static volatile rbled_mode_t s_mode = RBLED_MODE_FULL;
static int64_t s_start_us, s_wifi_down_since_us, s_mqtt_down_since_us;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static const entry_t *s_g, *s_r, *s_p;   /* current picks (under s_mux) */

static StaticTask_t s_tcb;
static StackType_t  s_stack[3072 / sizeof(StackType_t)];

static bool level(pat_t p, uint8_t n, uint32_t t, bool second)
{
    switch (p) {
        case PAT_ON:     return true;
        case PAT_SLOW:   return (t % 1000) < 500;
        case PAT_FAST:   return (t % 200) < 100;
        case PAT_BEAT:   return (t % 3000) < TICK_MS;
        case PAT_N: { uint32_t cyc = n * 500u + 2000u, pos = t % cyc; return pos < n * 500u && (pos % 500u) < 200u; }
        case PAT_ALT_F:  return ((t % 250) < 125) != second;
        case PAT_ALT_S:  return ((t % 1000) < 500) != second;
        case PAT_BOTH_F: return (t % 200) < 100;
        default:         return false;
    }
}

static void evaluate(void)
{
    const int64_t now = esp_timer_get_time(), up = now - s_start_us;
    rbled_app_state_t a = { 0 };
    if (s_probe) s_probe(&a);
    const bool off = s_mode == RBLED_MODE_OFF, quiet = s_mode == RBLED_MODE_QUIET;

    /* processes (both LEDs) */
    const entry_t *p = NULL;
    if (s_proc_factory) p = &P_FACTORY;
    else if (s_proc_ota) p = &P_OTA;
    else if (s_ident_until_us && now < s_ident_until_us) p = &P_IDENT;
    else if (a.module_ota && !off) p = &P_MODOTA;

    /* network state */
    bool creds = true, wifi_up = true;
#if CONFIG_RBGRID_IF_WIFI
    creds = rbwifi_nvs_has_credentials();
    wifi_up = rbwifi_is_connected();
#endif
    if (wifi_up || !creds) s_wifi_down_since_us = 0;
    else if (!s_wifi_down_since_us) s_wifi_down_since_us = now;
    bool mqtt_down_long = false;
#if CONFIG_RBGRID_IF_MQTT
    if (!rbmqtt_is_started() || rbmqtt_is_connected()) s_mqtt_down_since_us = 0;
    else if (!s_mqtt_down_since_us) s_mqtt_down_since_us = now;
    mqtt_down_long = s_mqtt_down_since_us && now - s_mqtt_down_since_us > NET_GRACE_US;
#endif
    const char *st = rbstats_store_state_str();

    /* green layer */
    const entry_t *g;
    if (up < LAMP_MS * 1000LL) g = &G_LAMP;
    else if (!s_boot_done) g = &G_BOOT;
    else if (!strcmp(st, "migrating")) g = &G_STORE;
    else if (a.espnow_pairing || rbllm_pair_pending() > 0) g = &G_PAIR;
    else if (!creds) g = &G_SETUP;
    else if (!wifi_up) g = &G_CONN;
    else g = (quiet || off) ? NULL : &G_NORMAL;
    if (off && g != &G_LAMP) g = NULL;

    /* red layer */
    const entry_t *r = NULL;
    const bool ev_live = !s_acked && up < EVENT_SHOW_US;
    if (up < LAMP_MS * 1000LL) r = &G_LAMP;
    else if (s_boot_done && up > MOD_GRACE_US && a.online == 0) r = &R_NOMOD;
    else if (rbmedia_mode() == RBMEDIA_NONE || !strcmp(st, "incompatible")) r = &R_STORE;
    else if (s_boot_done && up > MOD_GRACE_US && a.missing > 0) r = &R_MISSING;
    else if (s_rollback && ev_live) r = &R_ROLLBK;
    else if (s_unexpected_reset && ev_live) r = &R_RESTART;
    else if (s_wifi_down_since_us && now - s_wifi_down_since_us > NET_GRACE_US) r = &R_WIFI;
    else if (mqtt_down_long) r = &R_MQTT;
    else if (creds && up > TIME_GRACE_US && !rbtime_is_time_valid()) r = &R_CLOCK;
    if (off && r != &R_NOMOD && r != &G_LAMP) r = NULL;

    portENTER_CRITICAL(&s_mux);
    s_g = g; s_r = r; s_p = p;
    portEXIT_CRITICAL(&s_mux);
}

static void led_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    for (;;) {
        if (tick % EVAL_EVERY == 0) evaluate();
        const uint32_t t = tick * TICK_MS;
        portENTER_CRITICAL(&s_mux);
        const entry_t *g = s_g, *r = s_r, *p = s_p;
        portEXIT_CRITICAL(&s_mux);
        bool lg, lr;
        if (p) { lg = level(p->pat, p->n, t, false); lr = level(p->pat, p->n, t, true); }
        else   { lg = g ? level(g->pat, g->n, t, false) : false; lr = r ? level(r->pat, r->n, t, false) : false; }
        if (s_gpio_g >= 0) gpio_set_level((gpio_num_t)s_gpio_g, lg);
        if (s_gpio_r >= 0) gpio_set_level((gpio_num_t)s_gpio_r, lr);
        tick++;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

static void mode_load(void)
{
    nvs_handle_t h;
    if (rbgrid_nvs_open("rbled", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, "mode", &v) == ESP_OK && v <= RBLED_MODE_OFF) s_mode = (rbled_mode_t)v;
    nvs_close(h);
}

static int pin_setup(int g, rbled_pin_busy_t busy, const char *name)
{
    if (g < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(g)) return -1;
    if (busy && busy(g)) {
        rbpower_diag_printf("led: %s LED on GPIO%d not driven — the pin is used by the I2C configuration", name, g);
        return -1;
    }
    gpio_config_t c = { .pin_bit_mask = 1ULL << g, .mode = GPIO_MODE_OUTPUT, .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE };
    if (gpio_config(&c) != ESP_OK) return -1;
    gpio_set_level((gpio_num_t)g, 0);
    return g;
}

void rbled_start(rbled_probe_t probe, rbled_pin_busy_t busy)
{
    if (s_started) return;
    s_probe = probe;
    mode_load();
    const esp_reset_reason_t rr = esp_reset_reason();
    s_unexpected_reset = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
                         rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT;
    s_gpio_g = pin_setup(CONFIG_RBGRID_LED_GREEN_GPIO, busy, "green");
    s_gpio_r = pin_setup(CONFIG_RBGRID_LED_RED_GPIO, busy, "red");
    s_start_us = esp_timer_get_time();
    s_started = true;
    if (s_gpio_g < 0 && s_gpio_r < 0) { ESP_LOGI(TAG, "no status LEDs on this build"); return; }
    if (!xTaskCreateStatic(led_task, "rbled", sizeof(s_stack) / sizeof(StackType_t), NULL, 2, s_stack, &s_tcb))
        ESP_LOGE(TAG, "LED task not started");
    ESP_LOGI(TAG, "status LEDs: green GPIO%d, red GPIO%d, mode %s", s_gpio_g, s_gpio_r, rbled_mode_str(s_mode));
}

void rbled_boot_done(void) { s_boot_done = true; }

void rbled_process(rbled_proc_t p, bool on)
{
    if (p == RBLED_PROC_FACTORY_RESET) s_proc_factory = on;
    else if (p == RBLED_PROC_OTA) s_proc_ota = on;
    if (s_started) evaluate();
}

void rbled_identify(uint32_t seconds)
{
    if (seconds > 300) seconds = 300;
    s_ident_until_us = seconds ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    if (s_started) evaluate();
}

void rbled_flag_rollback(void) { s_rollback = true; s_acked = false; }
void rbled_ack(void) { s_acked = true; }

rbled_mode_t rbled_mode_get(void) { return s_mode; }

esp_err_t rbled_mode_set(rbled_mode_t m)
{
    if (m > RBLED_MODE_OFF) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = rbgrid_nvs_open("rbled", NVS_READWRITE, &h);
    if (e == ESP_OK) { e = nvs_set_u8(h, "mode", (uint8_t)m); if (e == ESP_OK) e = nvs_commit(h); nvs_close(h); }
    if (e == ESP_OK) { s_mode = m; if (s_started) evaluate(); }
    return e;
}

const char *rbled_mode_str(rbled_mode_t m) { return m == RBLED_MODE_QUIET ? "quiet" : m == RBLED_MODE_OFF ? "off" : "full"; }

int rbled_mode_from_str(const char *s)
{
    if (!s) return -1;
    if (!strcmp(s, "full")) return RBLED_MODE_FULL;
    if (!strcmp(s, "quiet")) return RBLED_MODE_QUIET;
    if (!strcmp(s, "off")) return RBLED_MODE_OFF;
    return -1;
}

static void add_entry(cJSON *o, const char *k, const entry_t *e)
{
    if (!e) { cJSON_AddNullToObject(o, k); return; }
    cJSON *x = cJSON_AddObjectToObject(o, k);
    cJSON_AddStringToObject(x, "id", e->id);
    cJSON_AddStringToObject(x, "state", e->state);
    cJSON_AddStringToObject(x, "pattern", PAT_NAME[e->pat]);
    if (e->pat == PAT_N) cJSON_AddNumberToObject(x, "flashes", e->n);
}

cJSON *rbled_status_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mode", rbled_mode_str(s_mode));
    cJSON *l = cJSON_AddObjectToObject(o, "leds");
    if (s_gpio_g >= 0) cJSON_AddNumberToObject(l, "green_gpio", s_gpio_g); else cJSON_AddNullToObject(l, "green_gpio");
    if (s_gpio_r >= 0) cJSON_AddNumberToObject(l, "red_gpio", s_gpio_r); else cJSON_AddNullToObject(l, "red_gpio");
    portENTER_CRITICAL(&s_mux);
    const entry_t *g = s_g, *r = s_r, *p = s_p;
    portEXIT_CRITICAL(&s_mux);
    add_entry(o, "green", g);
    add_entry(o, "red", r);
    add_entry(o, "process", p);
    const int64_t up = esp_timer_get_time() - s_start_us;
    cJSON_AddBoolToObject(o, "events_pending", (s_rollback || s_unexpected_reset) && !s_acked && up < EVENT_SHOW_US);
    return o;
}

const char *rbled_apply_json(const cJSON *req)
{
    const cJSON *m = cJSON_GetObjectItem(req, "mode"), *id = cJSON_GetObjectItem(req, "identify"), *ack = cJSON_GetObjectItem(req, "ack");
    if (!m && !id && !ack) return "expected {mode: full|quiet|off} and/or {identify: seconds|true|false} and/or {ack: true}";
    int mv = -1;
    if (m) { mv = rbled_mode_from_str(cJSON_GetStringValue(m)); if (mv < 0) return "mode: full | quiet | off"; }
    uint32_t secs = 0; bool do_id = false;
    if (id) {
        if (cJSON_IsBool(id)) { secs = cJSON_IsTrue(id) ? 10 : 0; do_id = true; }
        else if (cJSON_IsNumber(id) && id->valuedouble >= 0 && id->valuedouble <= 300) { secs = (uint32_t)id->valuedouble; do_id = true; }
        else return "identify: seconds 0..300, or true (10 s) / false (stop)";
    }
    if (ack && !cJSON_IsTrue(ack)) return "ack: true";
    if (mv >= 0 && rbled_mode_set((rbled_mode_t)mv) != ESP_OK) return "mode not saved";
    if (do_id) rbled_identify(secs);
    if (ack) rbled_ack();
    return NULL;
}

#if CONFIG_RBGRID_IF_HTTP
static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

static esp_err_t led_get(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbwebserver_api_check_read(req)) return ESP_FAIL;
    return send_json(req, "200 OK", rbled_status_json());
}

static esp_err_t led_post(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbwebserver_api_check_auth(req)) return ESP_FAIL;
    char buf[128] = { 0 };
    int len = req->content_len < (int)sizeof(buf) - 1 ? req->content_len : (int)sizeof(buf) - 1;
    if (len > 0 && httpd_req_recv(req, buf, len) <= 0) len = -1;
    cJSON *b = len >= 0 ? cJSON_Parse(len > 0 ? buf : "{}") : NULL;
    const char *why = b ? rbled_apply_json(b) : "bad JSON";
    cJSON_Delete(b);
    if (why) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "status", "error");
        cJSON_AddStringToObject(e, "code", "INVALID_ARGS");
        cJSON_AddStringToObject(e, "message", why);
        return send_json(req, "400 Bad Request", e);
    }
    cJSON *o = rbled_status_json();
    cJSON_AddStringToObject(o, "status", "ok");
    return send_json(req, "200 OK", o);
}

void rbled_register_http(void)
{
    httpd_handle_t srv = rbwebserver_core_server();
    if (!srv) return;
    httpd_uri_t g = { .uri = "/api/v1/led", .method = HTTP_GET,  .handler = led_get,  .user_ctx = NULL };
    httpd_uri_t p = { .uri = "/api/v1/led", .method = HTTP_POST, .handler = led_post, .user_ctx = NULL };
    if (httpd_register_uri_handler(srv, &g) != ESP_OK || httpd_register_uri_handler(srv, &p) != ESP_OK)
        ESP_LOGE(TAG, "URI registration failed");
}
#else
void rbled_register_http(void) {}
#endif
