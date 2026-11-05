/* SPDX-License-Identifier: Apache-2.0 */
/* The perform stub never opens a socket; setopt captures the actual callbacks. */
#include "../native/http.h"
#include <curl/curl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { OK, INVALID, SETUP, TRANSPORT, TIMEOUT, BODY, HEADERS };
enum { VALIDATION, COMPLETE, PARTIAL, EXACT_BODY, OVER_BODY, EXACT_HEADERS,
       OVER_HEADERS, ZERO_HEADERS, BODY_OVERFLOW, HEADER_OVERFLOW, FRAGMENTS };
static curl_write_callback body_callback, header_callback;
static void *body_data, *header_data;
static unsigned char payload[256];
static size_t body_capacity, header_capacity, sent_body, sent_headers;
static unsigned mode, selector, performed;
static int expected;
static int use_client;
static int64_t client_id;
static void check(int condition) { if (!condition) abort(); }

CURLcode __real_curl_easy_setopt(CURL *handle, CURLoption option, ...);
CURLcode __wrap_curl_easy_setopt(CURL *handle, CURLoption option, ...) {
    va_list args;
    va_start(args, option);
    CURLcode code;
    if (option < CURLOPTTYPE_OBJECTPOINT) {
        long value = va_arg(args, long);
        code = __real_curl_easy_setopt(handle, option, value);
    } else if (option < CURLOPTTYPE_FUNCTIONPOINT) {
        void *value = va_arg(args, void *);
        if (option == CURLOPT_WRITEDATA) body_data = value;
        if (option == CURLOPT_HEADERDATA) header_data = value;
        code = __real_curl_easy_setopt(handle, option, value);
    } else if (option < CURLOPTTYPE_OFF_T) {
        check(option == CURLOPT_WRITEFUNCTION || option == CURLOPT_HEADERFUNCTION);
        curl_write_callback value = va_arg(args, curl_write_callback);
        if (option == CURLOPT_WRITEFUNCTION) body_callback = value;
        else header_callback = value;
        code = __real_curl_easy_setopt(handle, option, value);
    } else {
        check(option < CURLOPTTYPE_BLOB);
        curl_off_t value = va_arg(args, curl_off_t);
        code = __real_curl_easy_setopt(handle, option, value);
    }
    va_end(args);
    return code;
}
CURLcode __wrap_curl_easy_getinfo(CURL *handle, CURLINFO info, ...) {
    (void)handle;
    check(info == CURLINFO_RESPONSE_CODE);
    va_list args;
    va_start(args, info);
    *va_arg(args, long *) = 200;
    va_end(args);
    return CURLE_OK;
}
static int emit(curl_write_callback callback, void *opaque, const void *data,
                size_t length, size_t *sent) {
    if (callback((char *)data, 1, length, opaque) != length) return 0;
    *sent += length;
    return 1;
}
static int status(void) {
    static const char line[] = "HTTP/1.1 200 OK\r\n";
    return emit(header_callback, header_data, line, sizeof line - 1, &sent_headers);
}
static int complete_headers(size_t total) {
    static const char blank[] = "\r\n";
    check(total == 19 || (total >= 24 && total <= 255));
    if (!status()) return 0;
    if (total != 19) {
        char field[256];
        size_t length = total - 19;
        memcpy(field, "X:", 2);
        memset(field + 2, 'x', length - 4);
        memcpy(field + length - 2, "\r\n", 2);
        if (!emit(header_callback, header_data, field, length, &sent_headers)) return 0;
    }
    return emit(header_callback, header_data, blank, sizeof blank - 1, &sent_headers);
}
CURLcode __wrap_curl_easy_perform(CURL *handle) {
    (void)handle;
    ++performed;
    check(body_callback && header_callback && body_data && header_data);
    if (mode == VALIDATION) return CURLE_RECV_ERROR;
    if (mode == BODY_OVERFLOW || mode == HEADER_OVERFLOW) {
        curl_write_callback callback = mode == BODY_OVERFLOW ? body_callback : header_callback;
        void *opaque = mode == BODY_OVERFLOW ? body_data : header_data;
        check(callback((char *)payload, SIZE_MAX, 2, opaque) == 0);
        return CURLE_WRITE_ERROR;
    }
    if (mode == ZERO_HEADERS) {
        check(!status());
        return CURLE_WRITE_ERROR;
    }
    if (mode == EXACT_HEADERS || mode == OVER_HEADERS) {
        check(complete_headers(header_capacity));
        if (mode == OVER_HEADERS) {
            static const char trailer[] = "X: x\r\n";
            check(!emit(header_callback, header_data, trailer, sizeof trailer - 1, &sent_headers));
            return CURLE_WRITE_ERROR;
        }
        return CURLE_OK;
    }
    if (mode == PARTIAL) {
        check(status());
        return CURLE_OK;
    }
    check(complete_headers(19));
    /* Both zero factors must be harmless, including with a NULL source. */
    check(body_callback(NULL, 0, SIZE_MAX, body_data) == 0);
    check(body_callback(NULL, SIZE_MAX, 0, body_data) == 0);
    check(header_callback(NULL, 0, SIZE_MAX, header_data) == 0);
    if (mode == COMPLETE) return CURLE_OK;
    if (mode == EXACT_BODY || mode == OVER_BODY) {
        check(emit(body_callback, body_data, payload, body_capacity, &sent_body));
        if (mode == OVER_BODY) {
            check(!emit(body_callback, body_data, payload, 1, &sent_body));
            return CURLE_WRITE_ERROR;
        }
        return CURLE_OK;
    }
    check(mode == FRAGMENTS);
    for (size_t i = 0; i < 8; ++i) {
        size_t length = payload[(selector + i) % sizeof payload] % 65;
        if (!emit(body_callback, body_data, payload + i, length, &sent_body)) {
            expected = BODY;
            return CURLE_WRITE_ERROR;
        }
    }
    if (selector % 3 == 0) { expected = TIMEOUT; return CURLE_OPERATION_TIMEDOUT; }
    if (selector % 3 == 1) { expected = TRANSPORT; return CURLE_RECV_ERROR; }
    return CURLE_OK;
}
static void run(gh_input url, gh_input headers, gh_input upload, int64_t method,
                int64_t timeout, int64_t allow_http, size_t body_limit,
                size_t header_limit, unsigned scenario, int want) {
    unsigned char body[258], head[258]; int64_t meta[4] = {0};
    check(body_limit <= 255 && header_limit <= 255);
    memset(body, 0xa5, sizeof body); memset(head, 0xa5, sizeof head);
    body_callback = header_callback = NULL; body_data = header_data = NULL;
    body_capacity = body_limit; header_capacity = header_limit;
    sent_body = sent_headers = performed = 0; mode = scenario; expected = want;
    gh_output body_out = {body + 1, (int64_t)body_limit};
    gh_output header_out = {head + 1, (int64_t)header_limit};
    int64_t result = use_client ?
        gh_client_request(client_id, url, method, headers, upload, timeout, allow_http,
                          body_out, header_out, (gh_metadata){meta, 4}) :
        gh_request(url, method, headers, upload, timeout, allow_http,
                   body_out, header_out, (gh_metadata){meta, 4});
    if (scenario == VALIDATION) {
        check(result == INVALID || result == SETUP || result == TRANSPORT);
        if (want == INVALID) check(result == INVALID && !performed);
    }
    else check(performed == 1 && result == expected);
    if (result == OK) {
        check(meta[0] == CURLE_OK && meta[1] == 200);
        check(meta[2] == (int64_t)sent_body && meta[3] == (int64_t)sent_headers);
        check(sent_body <= body_limit && sent_headers <= header_limit);
        if (scenario == EXACT_BODY) check(!memcmp(body + 1, payload, body_limit));
    } else {
        check(meta[2] == 0 && meta[3] == 0);
        if (scenario == PARTIAL) check(meta[0] == CURLE_PARTIAL_FILE);
    }
    check(body[0] == 0xa5 && head[0] == 0xa5);
    for (size_t i = body_limit + 1; i < sizeof body; ++i) check(body[i] == 0xa5);
    for (size_t i = header_limit + 1; i < sizeof head; ++i) check(head[i] == 0xa5);
    if (scenario == VALIDATION)
        for (size_t i = 0; i < sizeof body; ++i) check(body[i] == 0xa5 && head[i] == 0xa5);
}
static int64_t open_client(gh_input origin) {
    int64_t id = -1;
    check(gh_client_open(origin, (gh_metadata){&id, 1}) == OK && id > 0);
    return id;
}
static void clients(gh_input address, gh_input arbitrary, gh_input none,
                    size_t exact_headers) {
    static const char origin[] = "https://example.invalid";
    gh_input authority = {(const uint8_t *)origin, sizeof origin - 1};
    int64_t arbitrary_id = -1;
    int64_t result = gh_client_open(arbitrary, (gh_metadata){&arbitrary_id, 1});
    check(result == OK || result == INVALID || result == SETUP);
    if (result == OK) check(arbitrary_id > 0 && gh_client_close(arbitrary_id) == OK);
    else check(arbitrary_id == 0);
    int64_t first = open_client(authority), second = open_client(authority);
    check(first != second);
    use_client = 1; client_id = first;
    run(arbitrary, none, none, 0, 100, 0, selector, selector, VALIDATION, TRANSPORT);
    run(address, arbitrary, none, 0, 100, 0, selector, selector, VALIDATION, TRANSPORT);
    run(address, none, arbitrary, 1, 100, 0, selector, selector, VALIDATION, INVALID);
    run(address, none, none, 1, 100, 0, selector, selector, VALIDATION, INVALID);
    run(address, none, none, 0, 100, 0, selector, 64, COMPLETE, OK);
    run(address, none, none, 0, 100, 0, selector, 64, COMPLETE, OK);
    run(address, none, none, 0, 100, 0, selector, 64, EXACT_BODY, OK);
    run(address, none, none, 0, 100, 0, selector, 64, OVER_BODY, BODY);
    run(address, none, none, 0, 100, 0, selector, exact_headers, EXACT_HEADERS, OK);
    run(address, none, none, 0, 100, 0, selector, exact_headers, OVER_HEADERS, HEADERS);
    run(address, none, none, 0, 100, 0, selector, 0, ZERO_HEADERS, HEADERS);
    run(address, none, none, 0, 100, 0, selector, 64, BODY_OVERFLOW, BODY);
    run(address, none, none, 0, 100, 0, selector, 64, HEADER_OVERFLOW, HEADERS);
    run(address, none, none, 0, 100, 0, selector, 64, PARTIAL, TRANSPORT);
    /* Interleave a bounded input-selected sequence on two independent clients. */
    for (unsigned i = 0; i <= selector % 4; ++i) {
        client_id = i & 1 ? first : second;
        run(address, none, none, 0, 100, 0, selector, 64, FRAGMENTS, OK);
    }
    check(gh_client_close(first) == OK);
    client_id = first;
    run(address, none, none, 0, 100, 0, selector, 64, VALIDATION, INVALID);
    check(gh_client_close(first) == INVALID);
    int64_t replacement = open_client(authority);
    check(replacement != first && replacement != second);
    run(address, none, none, 0, 100, 0, selector, 64, VALIDATION, INVALID);
    client_id = selector & 1 ? 0 : -1;
    run(address, none, none, 0, 100, 0, selector, 64, VALIDATION, INVALID);
    client_id = second;
    run(address, none, none, 0, 100, 0, selector, 64, COMPLETE, OK);
    client_id = replacement;
    run(address, none, none, 0, 100, 0, selector, 64, COMPLETE, OK);
    check(gh_client_close(second) == OK && gh_client_close(replacement) == OK);
    check(gh_client_close(0) == OK);
    use_client = 0; client_id = 0;
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536) return 0;
    static const char valid[] = "https://example.invalid/fuzz";
    gh_input address = {(const uint8_t *)valid, sizeof valid - 1};
    gh_input none = {NULL, 0}, arbitrary = {data, (int64_t)size};
    selector = size ? data[0] : 0;
    for (size_t i = 0; i < sizeof payload; ++i) payload[i] = size ? data[i % size] : 0;
    run(arbitrary, none, none, 0, 100, 1, selector, selector, VALIDATION, TRANSPORT);
    run(address, arbitrary, none, 0, 100, 0, selector, selector, VALIDATION, TRANSPORT);
    run(address, none, arbitrary, 1, 100, 0, selector, selector, VALIDATION, TRANSPORT);
    run(address, none, none, (int64_t)(selector % 4) - 1,
        selector & 1 ? 0 : 100, selector % 3, selector, selector, VALIDATION, TRANSPORT);
    run(address, none, none, 0, 100, 0, selector, 64, COMPLETE, OK);
    run(address, none, none, 0, 100, 0, selector, 64, PARTIAL, TRANSPORT);
    run(address, none, none, 0, 100, 0, selector, 64, EXACT_BODY, OK);
    run(address, none, none, 0, 100, 0, selector, 64, OVER_BODY, BODY);
    size_t exact_headers = selector & 1 ? 19 : 24 + selector % 232;
    run(address, none, none, 0, 100, 0, selector, exact_headers, EXACT_HEADERS, OK);
    run(address, none, none, 0, 100, 0, selector, exact_headers, OVER_HEADERS, HEADERS);
    run(address, none, none, 0, 100, 0, selector, 0, ZERO_HEADERS, HEADERS);
    run(address, none, none, 0, 100, 0, selector, 64, BODY_OVERFLOW, BODY);
    run(address, none, none, 0, 100, 0, selector, 64, HEADER_OVERFLOW, HEADERS);
    run(address, none, none, 0, 100, 0, selector, 64, FRAGMENTS, OK);
    clients(address, arbitrary, none, exact_headers);
    return 0;
}
