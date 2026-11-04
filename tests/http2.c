/* SPDX-License-Identifier: Apache-2.0 */
/* Only this test wrapper injects a fixture CA. Production trust stays unchanged. */
#include "../native/http.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "http2:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { OK, INVALID, SETUP, TRANSPORT, TIMEOUT, BODY, HEADERS };
static const char *origin, *trusted_ca;
static unsigned h2_transfers;
static long last_protocol;
CURLcode __real_curl_easy_perform(CURL *handle);
CURLcode __wrap_curl_easy_perform(CURL *handle) {
    if (trusted_ca) {
        CURLcode code = curl_easy_setopt(handle, CURLOPT_CAINFO, trusted_ca);
        if (code != CURLE_OK) return code;
    }
    CURLcode code = __real_curl_easy_perform(handle);
    last_protocol = 0;
    CHECK(curl_easy_getinfo(handle, CURLINFO_HTTP_VERSION, &last_protocol) == CURLE_OK);
    if (code == CURLE_OK) {
        CHECK(last_protocol == CURL_HTTP_VERSION_2_0);
        ++h2_transfers;
    }
    return code;
}
static gh_input input(const void *p, size_t n) { return (gh_input){p, (int64_t)n}; }
static int64_t request(const char *path, int64_t method, gh_input fields, gh_input upload,
                       int64_t timeout, uint8_t *body, size_t body_limit,
                       uint8_t *headers, size_t header_limit, int64_t meta[4]) {
    char address[256];
    int length = snprintf(address, sizeof address, "%s%s", origin, path);
    CHECK(length > 0 && length < (int)sizeof address);
    for (size_t i = 0; i < 4; ++i) meta[i] = 123;
    return gh_request(input(address, strlen(address)), method, fields, upload, timeout, 0,
        (gh_output){body, (int64_t)body_limit}, (gh_output){headers, (int64_t)header_limit},
        (gh_metadata){meta, 4});
}
static void empty(const int64_t meta[4]) { CHECK(meta[2] == 0 && meta[3] == 0); }
int main(int argc, char **argv) {
    CHECK(argc == 4);
    CHECK(strcmp(argv[1], argv[3]) != 0);
    CHECK(curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_HTTP2);
    origin = argv[1];
    uint8_t body[128], headers[4097]; int64_t meta[4];
    gh_input none = input(NULL, 0);
    CHECK(request("/binary", 0, none, none, 5000, body, sizeof body, headers, 4096, meta) == TRANSPORT);
    CHECK(meta[0] == CURLE_PEER_FAILED_VERIFICATION); empty(meta);
    trusted_ca = argv[2];
    static const uint8_t binary[] = "goose\0\xff\r\nbody";
    CHECK(request("/binary", 0, none, none, 5000, body, sizeof body, headers, 4096, meta) == OK);
    CHECK(meta[1] == 200 && meta[2] == sizeof binary - 1 && !memcmp(body, binary, sizeof binary - 1));
    headers[meta[3]] = 0;
    CHECK(strstr((char *)headers, "HTTP/2 200") && strstr((char *)headers, "x-protocol: h2\r\n"));
    size_t exact_headers = (size_t)meta[3];
    CHECK(request("/binary", 0, none, none, 5000, body, sizeof binary - 1, headers, exact_headers, meta) == OK);
    CHECK(request("/binary", 0, none, none, 5000, body, sizeof binary - 2, headers, 4096, meta) == BODY); empty(meta);
    CHECK(request("/binary", 0, none, none, 5000, body, sizeof body, headers, exact_headers - 1, meta) == HEADERS); empty(meta);
    CHECK(request("/empty", 0, none, none, 5000, NULL, 0, headers, 4096, meta) == OK && meta[1] == 204);
    CHECK(request("/empty", 0, none, none, 5000, NULL, 0, NULL, 0, meta) == HEADERS); empty(meta);
    static const char empty_fields[] = "X-Empty: \t \0";
    CHECK(request("/empty-header", 0, input(empty_fields, sizeof empty_fields - 1), none,
        5000, body, sizeof body, headers, 4096, meta) == OK);
    CHECK(meta[2] == 3 && !memcmp(body, "yes", 3));
    CHECK(request("/trailers", 0, none, none, 5000, body, sizeof body, headers, 4096, meta) == OK);
    CHECK(meta[1] == 200 && meta[2] == 3 && !memcmp(body, "abc", 3));
    headers[meta[3]] = 0;
    CHECK(strstr((char *)headers, "HTTP/2 103") && strstr((char *)headers, "x-end: done\r\n"));
    CHECK(request("/gzip", 0, none, none, 5000, body, sizeof body, headers, 4096, meta) == OK);
    CHECK(meta[2] > 2 && body[0] == 0x1f && body[1] == 0x8b);
    static const char auth[] = "Authorization: Bearer fixture-only\0";
    gh_input credentials = input(auth, sizeof auth - 1);
    CHECK(request("/redirect", 0, credentials, none, 5000, body, sizeof body, headers, 4096, meta) == OK && meta[1] == 302);
    CHECK(meta[2] == 8 && !memcmp(body, "redirect", 8));
    headers[meta[3]] = 0;
    char location[256];
    int location_length = snprintf(location, sizeof location, "location: %s/destination\r\n", argv[3]);
    CHECK(location_length > 0 && location_length < (int)sizeof location);
    CHECK(strstr((char *)headers, location));
    CHECK(request("/unauthorized", 0, credentials, none, 5000, body, sizeof body, headers, 4096, meta) == OK && meta[1] == 401);
    CHECK(meta[2] == 4 && !memcmp(body, "auth", 4));
    headers[meta[3]] = 0;
    CHECK(strstr((char *)headers, "www-authenticate: Basic realm=\"fixture\"\r\n"));
    CHECK(request("/proxy-auth", 0, credentials, none, 5000, body, sizeof body, headers, 4096, meta) == OK && meta[1] == 407);
    CHECK(meta[2] == 5 && !memcmp(body, "proxy", 5));
    headers[meta[3]] = 0;
    CHECK(strstr((char *)headers, "proxy-authenticate: Basic realm=\"fixture\"\r\n"));
    CHECK(request("/reset", 0, none, none, 5000, body, sizeof body, headers, 4096, meta) == TRANSPORT);
    CHECK(meta[0] != CURLE_OK && meta[1] == 200 && last_protocol == CURL_HTTP_VERSION_2_0); empty(meta);
    CHECK(request("/timeout", 0, none, none, 100, body, sizeof body, headers, 4096, meta) == TIMEOUT); empty(meta);
    uint8_t *upload = malloc(1048576), *download = malloc(1048576);
    CHECK(upload && download);
    for (size_t i = 0; i < 1048576; ++i) upload[i] = (uint8_t)i;
    static const char content_type[] = "Content-Type:\0";
    CHECK(request("/echo", 1, input(content_type, sizeof content_type - 1), input(upload, 1048576),
        5000, download, 1048576, headers, 4096, meta) == OK);
    CHECK(meta[2] == 1048576 && !memcmp(download, upload, 1048576));
    free(download); free(upload);
    CHECK(h2_transfers >= 10);
    printf("real HTTP/2 TLS passed: %u successful negotiated transfers (%s)\n", h2_transfers, curl_version());
    return 0;
}
