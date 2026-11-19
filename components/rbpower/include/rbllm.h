/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbllm.h
 * @brief LLM protocol v1 (documents/api/LLM_PROTOCOL_V1.md): tool registry, manifest, call envelope, LLM tokens,
 *        pairing and the call log. Transport-neutral: the HTTP routes (rbwebserver_llm.c) and later the remote
 *        tunnel and the ESPHome build carry the same JSON.
 *
 * Calls run on one low-priority worker task (rbllm_submit), never in the transport's own task.
 */
#ifndef RBLLM_H
#define RBLLM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBLLM_PROTO              "1.0"   ///< Protocol version (§11)
#define RBLLM_MAX_RESPONSE_BYTES 16384   ///< limits.max_response_bytes (§3)
#define RBLLM_MAX_MANIFEST_BYTES 65536   ///< limits.max_manifest_bytes (§3)
#define RBLLM_MAX_CONCURRENT     2       ///< limits.max_concurrent: one running + one queued
#define RBLLM_CALL_TIMEOUT_MS    8000    ///< limits.call_timeout_ms
#define RBLLM_MAX_REQUEST_BYTES  4096    ///< A call body larger than this is refused (invalid_args)

/** @brief Token scopes (§6). */
#define RBLLM_SCOPE_READ   0x01u
#define RBLLM_SCOPE_CONFIG 0x02u
#define RBLLM_SCOPE_ADMIN  0x04u

/** @brief Who is calling: from a validated LLM token. */
typedef struct {
    uint8_t scopes;      ///< RBLLM_SCOPE_* bits
    char    client[33];  ///< Client name given at pairing
    char    token_id[9]; ///< Token id (8 hex), "" for an internal caller
} rbllm_caller_t;

/** @brief Load /llm.json (state + token hashes). Call once after the config store is mounted. */
void rbllm_init(void);

/** @brief The LLM API is switched on (the hub's AI / LLM page). Default off. */
bool rbllm_enabled(void);

/**
 * @brief Check a presented bearer token.
 * @param[in]  token  Token text (without "Bearer ").
 * @param[out] out    Caller (scopes, client name) on success.
 * @return true if the token is a live LLM token.
 */
bool rbllm_auth(const char *token, rbllm_caller_t *out);

/** @brief The manifest (§3). @return malloc'd JSON (caller frees), or NULL on OOM. */
char *rbllm_manifest_json(void);

/**
 * @brief Run one call synchronously in the calling task (tests, the worker).
 * @param[in] body      Request JSON {tool, args, request_id}.
 * @param[in] len       Body length.
 * @param[in] caller    Validated caller.
 * @param[in] deadline_us  esp_timer deadline; 0 = now + call_timeout_ms.
 * @return malloc'd envelope JSON (§4), never NULL unless out of memory.
 */
char *rbllm_call(const char *body, size_t len, const rbllm_caller_t *caller, int64_t deadline_us);

/**
 * @brief Queue a call for the worker task.
 * @param[in] body    malloc'd request body; ownership passes to rbllm (freed after the call).
 * @param[in] len     Body length.
 * @param[in] caller  Caller (copied).
 * @param[in] done    Called on the worker task with the malloc'd reply (the callee frees it) and @p ctx.
 * @param[in] ctx     Opaque context for @p done.
 * @return false if the queue is full (answer `busy`); @p body is then still the caller's.
 */
bool rbllm_submit(char *body, size_t len, const rbllm_caller_t *caller, void (*done)(char *reply, void *ctx), void *ctx);

/** @brief An error envelope without a call (transport-level errors: llm_disabled, unauthorized, busy). */
char *rbllm_error_json(const char *request_id, const char *code, const char *message, const char *hint, int retry_after_ms);

/* ---- Pairing (§6) ---- */
/** @brief POST pair/start {client_name, scopes[]}. @param[out] status HTTP-style status. @return malloc'd JSON. */
char *rbllm_pair_start(const char *body, int *status);
/** @brief POST pair/complete {pair_id, code}. @param[out] status HTTP-style status. @return malloc'd JSON. */
char *rbllm_pair_complete(const char *body, int *status);

/* ---- Hub-side admin view (the AI / LLM page; admin token, not an LLM token) ---- */
/** @brief State, pending pairing codes, tokens (no hashes), the call log. @return malloc'd JSON. */
char *rbllm_admin_json(void);
/**
 * @brief Change the state: {enabled?, read_only?, revoke?: "<token id>"}.
 * @param[out] status HTTP-style status. @return malloc'd JSON (the new state, or an error naming the field).
 */
char *rbllm_admin_set(const char *body, int *status);

/** @brief Hub id: 12 hex of the base MAC (manifest, envelope, mDNS TXT `id`). @param[out] out 13 bytes. */
void rbllm_hub_id(char out[13]);

/** @brief Register the event-log reader (rbevlog_json in rbgrid_app). @param fn Reader. */
void rbllm_register_events_reader(char *(*fn)(uint32_t since, uint32_t limit, int minlvl));

#ifdef __cplusplus
}
#endif

#endif /* RBLLM_H */
