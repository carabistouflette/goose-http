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
#endif
