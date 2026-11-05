/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GOOSE_HTTP_INTERNAL_H
#define GOOSE_HTTP_INTERNAL_H

#include "http.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if LIBCURL_VERSION_NUM < 0x075500
#error "goose-http requires libcurl >= 7.85.0"
#endif

enum {
    GH_OK = 0,
    GH_INVALID = 1,
    GH_SETUP = 2,
    GH_TRANSPORT = 3,
    GH_TIMEOUT = 4,
    GH_BODY = 5,
    GH_HEADERS = 6
};

int gh_ensure_initialized(void);
bool gh_valid_slice(const void *p, int64_t n, int64_t max);
bool gh_overlap(const void *a, int64_t an, const void *b, int64_t bn);
bool gh_token(uint8_t c);
bool gh_equal_name(const uint8_t *p, size_t n, const char *s);
bool gh_valid_header(const uint8_t *p, size_t n);

/* Caller owns the parsed host returned by this setup-only origin validator. */
int64_t gh_parse_client_origin(gh_input origin, char **host, int64_t *port);
int64_t gh_request_engine(CURL **storage, const char *bound_host, int64_t bound_port,
    gh_input url, int64_t method, gh_input packed, gh_input upload,
    int64_t timeout_ms, int64_t allow_http, gh_output body, gh_output headers, gh_metadata meta);

#endif
