/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwebserver_llm.c
 * @brief HTTP transport of the LLM protocol v1 (§2): /api/llm/v1/{manifest,call,pair/start,pair/complete} with LLM
 *        tokens, and /api/v1/llm (the hub's AI / LLM page: admin token).
 *
 * A call never runs in the httpd task: the handler detaches the request (httpd async handler) and the rbllm worker
 * answers it, so a slow history read doesn't stall the web UI.
 */
#include "rbllm.h"
#include "rbwebserver_internal.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "RBLLM_HTTP";

bool rbapi_require_auth(httpd_req_t *req);   /* rbwebserver_api.c: admin gate (hub page) */
bool rbwebserver_api_token_armed(void);       /* an admin token is set (not the open onboarding state) */

static esp_err_t send_json(httpd_req_t *req, int status, char *body)
{
    const char *st = status == 200 ? "200 OK" : status == 400 ? "400 Bad Request" : status == 401 ? "401 Unauthorized" :
                     status == 403 ? "403 Forbidden" : status == 404 ? "404 Not Found" : status == 409 ? "409 Conflict" : status == 429 ? "429 Too Many Requests" :
                     status == 507 ? "507 Insufficient Storage" : "500 Internal Server Error";
    httpd_resp_set_status(req, st);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (status == 429) httpd_resp_set_hdr(req, "Retry-After", "2");
    esp_err_t e = httpd_resp_sendstr(req, body ? body : "{\"ok\":false,\"error\":{\"code\":\"internal\",\"message\":\"out of memory\"}}");
    free(body);
    return e;
}

static char *read_body(httpd_req_t *req, int cap)
{
    if (req->content_len <= 0 || req->content_len > cap) return NULL;
    char *b = malloc(req->content_len + 1);
    if (!b) return NULL;
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, b + got, req->content_len - got);
        if (r <= 0) { free(b); return NULL; }
        got += r;
    }
    b[got] = '\0';
    return b;
}

/* LLM API gate: enabled + a live LLM token (admin/read REST tokens are not accepted, §6). */
static bool llm_gate(httpd_req_t *req, rbllm_caller_t *who)
{
    /* §2: the open onboarding state (no admin token) never opens the LLM API — the pairing code would be readable by
     * anyone on the network through the open hub page */
    if (!rbllm_enabled() || !rbwebserver_api_token_armed()) {
        send_json(req, 403, rbllm_error_json(NULL, "llm_disabled", "the LLM API is off",
                                             "Ask the user to switch it on on the hub's AI / LLM page.", 0));
        return false;
    }
    char hdr[112] = {0};
    const char *tok = NULL;
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK && strncmp(hdr, "Bearer ", 7) == 0) tok = hdr + 7;
    if (!tok || !rbllm_auth(tok, who)) {
        send_json(req, 401, rbllm_error_json(NULL, "unauthorized", "missing or revoked LLM token", "Pair again.", 0));
        return false;
    }
    return true;
}

static esp_err_t h_manifest(httpd_req_t *req)
{
    add_cors_headers(req);
    rbllm_caller_t who;
    if (!llm_gate(req, &who)) return ESP_OK;
    return send_json(req, 200, rbllm_manifest_json());
}

static void call_done(char *reply, void *ctx)
{
    httpd_req_t *r = (httpd_req_t *)ctx;
    send_json(r, 200, reply);
    httpd_req_async_handler_complete(r);
}

static esp_err_t h_call(httpd_req_t *req)
{
    add_cors_headers(req);
    rbllm_caller_t who;
    if (!llm_gate(req, &who)) return ESP_OK;
    char *body = read_body(req, RBLLM_MAX_REQUEST_BYTES);
    if (!body) return send_json(req, 400, rbllm_error_json(NULL, "invalid_args", "empty or oversized body (max 4 KB)", NULL, 0));
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        free(body);
        return send_json(req, 429, rbllm_error_json(NULL, "busy", "no free connection slot", "Retry.", 1000));
    }
    if (!rbllm_submit(body, strlen(body), &who, call_done, copy)) {
        free(body);
        send_json(copy, 429, rbllm_error_json(NULL, "busy", "the hub is answering another call", "Retry.", 1000));
        httpd_req_async_handler_complete(copy);
    }
    return ESP_OK;
}

static esp_err_t h_pair(httpd_req_t *req)
{
    add_cors_headers(req);
    char *body = read_body(req, 512);
    if (!rbwebserver_api_token_armed()) {
        free(body);
        return send_json(req, 403, rbllm_error_json(NULL, "llm_disabled", "the hub has no admin token", "Set an admin token on the hub first.", 0));
    }
    int st = 400;
    char *out = strstr(req->uri, "/pair/start") ? rbllm_pair_start(body, &st) : rbllm_pair_complete(body, &st);
    free(body);
    return send_json(req, st, out);
}

static esp_err_t h_admin(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_auth(req)) return ESP_OK;   /* 401/403 sent */
    if (req->method == HTTP_GET) return send_json(req, 200, rbllm_admin_json());
    char *body = read_body(req, 512);
    cJSON *bj = body ? cJSON_Parse(body) : NULL;
    bool want_on = cJSON_IsTrue(cJSON_GetObjectItem(bj, "enabled"));
    cJSON_Delete(bj);
    if (want_on && !rbwebserver_api_token_armed()) {
        free(body);
        return send_json(req, 409, rbllm_error_json(NULL, "conflict", "set an admin token first",
                                                    "The LLM API can only be switched on once the hub is protected by an admin token.", 0));
    }
    int st = 400;
    char *out = rbllm_admin_set(body, &st);
    free(body);
    return send_json(req, st, out);
}

void rbwebserver_llm_register(httpd_handle_t srv)
{
    if (!srv) return;
    rbllm_init();
    const httpd_uri_t u[] = {
        { .uri = "/api/llm/v1/manifest",      .method = HTTP_GET,  .handler = h_manifest },
        { .uri = "/api/llm/v1/call",          .method = HTTP_POST, .handler = h_call },
        { .uri = "/api/llm/v1/pair/start",    .method = HTTP_POST, .handler = h_pair },
        { .uri = "/api/llm/v1/pair/complete", .method = HTTP_POST, .handler = h_pair },
        { .uri = "/api/v1/llm",               .method = HTTP_GET,  .handler = h_admin },
        { .uri = "/api/v1/llm",               .method = HTTP_POST, .handler = h_admin },
    };
    for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); i++)
        if (httpd_register_uri_handler(srv, &u[i]) != ESP_OK)
            ESP_LOGE(TAG, "URI registration FAILED: %s — check max_uri_handlers", u[i].uri);
}
