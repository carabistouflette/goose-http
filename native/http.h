/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GOOSE_HTTP_H
#define GOOSE_HTTP_H
#include <stdint.h>
typedef struct { const uint8_t *data; int64_t len; } gh_input;
typedef struct { uint8_t *data; int64_t len; } gh_output;
typedef struct { int64_t *data; int64_t len; } gh_metadata;

/* Private slice ABI. No pointer is retained. meta: curl code, HTTP code,
 * body length, header length. Inputs must remain live throughout the call. */
int64_t gh_request(gh_input url, int64_t method, gh_input packed_headers,
                   gh_input upload, int64_t timeout_ms, int64_t allow_http,
                   gh_output body, gh_output headers, gh_metadata meta);

#ifndef GH_MAX_CLIENTS
#define GH_MAX_CLIENTS 32
#endif
/* Reusable, origin-bound GET clients. No borrowed input survives a call. */
int64_t gh_client_open(gh_input origin, gh_metadata meta);
int64_t gh_client_request(int64_t id, gh_input url, int64_t method,
    gh_input packed, gh_input upload, int64_t timeout_ms, int64_t allow_http,
    gh_output body, gh_output headers, gh_metadata meta);
int64_t gh_client_close(int64_t id);

#ifndef GH_MAX_STREAMS
#define GH_MAX_STREAMS 32
#endif

/* Streaming API: Pull-based transfer with generational handle registry. */
int64_t gh_stream_open(gh_input url, gh_input packed, int64_t timeout_ms,
                       int64_t connect_timeout_ms, int64_t chunk_limit,
                       int64_t body_limit, int64_t header_limit,
                       int64_t allow_http, gh_metadata meta);

int64_t gh_stream_step(int64_t handle_id, int64_t step_timeout_ms, gh_metadata meta);

int64_t gh_stream_read_data(int64_t handle_id, gh_output chunk, gh_output headers, gh_metadata meta);

int64_t gh_stream_read_headers(int64_t handle_id, gh_output headers, gh_metadata meta);

int64_t gh_stream_close(int64_t handle_id, gh_metadata meta);

#endif
