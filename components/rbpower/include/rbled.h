/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbled.h
 * @brief Hub status LEDs: green = the hub's state, red = what needs attention (documents/architecture/LED_INDICATION.md).
 *
 * One static task drives both LEDs (50 ms tick) and re-evaluates the state registry every 500 ms from the
 * subsystems' own getters; the application adds what lives above this component (modules, ESP-NOW pairing)
 * through a probe callback. Processes (factory reset, firmware update, identify, module update) take both LEDs.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What the application reports each evaluation (modules and ESP-NOW live above rbpower). */
typedef struct {
    int  configured;      ///< configured modules
    int  online;          ///< modules answering
    int  missing;         ///< absent modules that feed a metering point (placeholders excluded)
    bool module_ota;      ///< a module firmware update is writing / committing
    bool espnow_pairing;  ///< the ESP-NOW pairing window is open
} rbled_app_state_t;

typedef void (*rbled_probe_t)(rbled_app_state_t *out);
/** @return true if @p gpio is used by something else (I2C bus, DRDY line): that LED then stays undriven. */
typedef bool (*rbled_pin_busy_t)(int gpio);

typedef enum { RBLED_MODE_FULL = 0, RBLED_MODE_QUIET = 1, RBLED_MODE_OFF = 2 } rbled_mode_t;
typedef enum { RBLED_PROC_FACTORY_RESET = 0, RBLED_PROC_OTA = 1 } rbled_proc_t;

/** @brief Start the LEDs (lamp test, then "booting"). Call once the I2C fleet config is loaded. */
void rbled_start(rbled_probe_t probe, rbled_pin_busy_t busy);
/** @brief Boot finished: the green layer leaves "booting". */
void rbled_boot_done(void);
/** @brief A process that takes both LEDs starts / ends. Safe before rbled_start (ignored). */
void rbled_process(rbled_proc_t p, bool on);
/** @brief Identify: both LEDs blink together for @p seconds (0 = stop). */
void rbled_identify(uint32_t seconds);
/** @brief A firmware update was rolled back (shown as red 8x until acknowledged or 24 h). */
void rbled_flag_rollback(void);
/** @brief Acknowledge the event entries (unexpected restart, rollback). */
void rbled_ack(void);

rbled_mode_t rbled_mode_get(void);
esp_err_t    rbled_mode_set(rbled_mode_t m);          ///< persisted (NVS)
const char  *rbled_mode_str(rbled_mode_t m);          ///< "full" | "quiet" | "off"
int          rbled_mode_from_str(const char *s);      ///< -1 if unknown

/** @brief {mode, leds:{green,red} gpio or null, green:{id,state,pattern}, red:{...}|null, process:{...}|null, events_pending}. */
cJSON *rbled_status_json(void);
/** @brief Apply {mode?, identify?: seconds|true|false, ack?: true}; @return NULL on success, else a reason. */
const char *rbled_apply_json(const cJSON *req);
/** @brief Register GET/POST /api/v1/led on the running web server. */
void rbled_register_http(void);

#ifdef __cplusplus
}
#endif
