/* SPDX-License-Identifier: Apache-2.0 */
/* Deterministic runtime boundary failures; each mode runs in a fresh process. */
#include "../native/http.h"
#include <curl/curl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "runtime:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { SETUP = 2, TRANSPORT = 3, TIMEOUT = 4 };
static int fail_global, global_calls, missing_feature, old_version, performed, queried;
static CURLcode transfer_code = CURLE_OK;
static curl_write_callback body_callback, header_callback;
static void *body_data, *header_data;

CURLcode __real_curl_global_init(long flags);
CURLcode __wrap_curl_global_init(long flags) {
    ++global_calls;
    return fail_global ? CURLE_OUT_OF_MEMORY : __real_curl_global_init(flags);
}
curl_version_info_data *__real_curl_version_info(CURLversion age);
curl_version_info_data *__wrap_curl_version_info(CURLversion age) {
    static curl_version_info_data version;
    version = *__real_curl_version_info(age);
    version.features &= ~missing_feature;
    if (old_version) version.version_num = 0x075400;
    return &version;
}
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
        CHECK(option == CURLOPT_WRITEFUNCTION || option == CURLOPT_HEADERFUNCTION);
        curl_write_callback value = va_arg(args, curl_write_callback);
        if (option == CURLOPT_WRITEFUNCTION) body_callback = value;
        else header_callback = value;
        code = __real_curl_easy_setopt(handle, option, value);
    } else {
        CHECK(option < CURLOPTTYPE_BLOB);
        curl_off_t value = va_arg(args, curl_off_t);
        code = __real_curl_easy_setopt(handle, option, value);
    }
    va_end(args);
    return code;
}
CURLcode __wrap_curl_easy_perform(CURL *handle) {
    (void)handle;
    ++performed;
    CHECK(body_callback && header_callback && body_data && header_data);
    static const char status[] = "HTTP/1.1 200 OK\r\n";
    static const char blank[] = "\r\n";
    static const char body[] = "abc";
    CHECK(header_callback((char *)status, 1, sizeof status - 1, header_data) == sizeof status - 1);
    CHECK(header_callback((char *)blank, 1, sizeof blank - 1, header_data) == sizeof blank - 1);
    CHECK(body_callback((char *)body, 1, sizeof body - 1, body_data) == sizeof body - 1);
    return transfer_code;
}
CURLcode __wrap_curl_easy_getinfo(CURL *handle, CURLINFO info, ...) {
    (void)handle;
    CHECK(info == CURLINFO_RESPONSE_CODE);
    ++queried;
    /* A failed query need not populate its output pointer. */
    return CURLE_OUT_OF_MEMORY;
}
static int64_t request(int64_t meta[4]) {
    static const unsigned char url[] = "https://example.invalid/runtime";
    unsigned char body[64], headers[64];
    for (size_t i = 0; i < 4; ++i) meta[i] = 123;
    return gh_request((gh_input){url, sizeof url - 1}, 0,
        (gh_input){NULL, 0}, (gh_input){NULL, 0}, 1000, 0,
        (gh_output){body, sizeof body}, (gh_output){headers, sizeof headers},
        (gh_metadata){meta, 4});
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    const char *mode = argv[1];
    int expected = SETUP;
    CURLcode expected_code = CURLE_OK;
    if (!strcmp(mode, "init-failure")) fail_global = 1;
    else if (!strcmp(mode, "missingSSL")) missing_feature = CURL_VERSION_SSL;
    else if (!strcmp(mode, "missingASYNCHDNS")) missing_feature = CURL_VERSION_ASYNCHDNS;
    else if (!strcmp(mode, "missingTHREADSAFE")) missing_feature = CURL_VERSION_THREADSAFE;
    else if (!strcmp(mode, "runtimeoldversion")) old_version = 1;
    else if (!strcmp(mode, "getinfo-failure-after-success")) {
        expected = TRANSPORT;
        expected_code = CURLE_OUT_OF_MEMORY;
    } else if (!strcmp(mode, "getinfo-failure-after-timeout")) {
        transfer_code = CURLE_OPERATION_TIMEDOUT;
        expected = TIMEOUT;
        expected_code = CURLE_OPERATION_TIMEDOUT;
    } else CHECK(0);
    int64_t meta[4];
    CHECK(request(meta) == expected);
    CHECK(global_calls == 1 && performed == (expected != SETUP) && queried == performed);
    CHECK(meta[0] == expected_code && meta[1] == 0 && meta[2] == 0 && meta[3] == 0);
    if (fail_global) {
        /* The once-only initializer preserves a failure on later requests. */
        fail_global = 0;
        CHECK(request(meta) == SETUP && global_calls == 1 && !performed);
        CHECK(meta[0] == CURLE_OK && meta[1] == 0 && meta[2] == 0 && meta[3] == 0);
    }
    printf("native HTTP runtime mode passed: %s\n", mode);
    return 0;
}
