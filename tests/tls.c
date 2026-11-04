/* SPDX-License-Identifier: Apache-2.0 */
/* Test-only trust injection. Production has no custom CA option or wrapper. */
#include "../native/http.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "tls:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { OK, INVALID, SETUP, TRANSPORT };
static const char *trusted_ca;
CURLcode __real_curl_easy_perform(CURL *handle);
CURLcode __wrap_curl_easy_perform(CURL *handle) {
    if (trusted_ca) {
        CURLcode code = curl_easy_setopt(handle, CURLOPT_CAINFO, trusted_ca);
        if (code != CURLE_OK) return code;
    }
    return __real_curl_easy_perform(handle);
}
static int64_t request(const char *url, uint8_t body[64], uint8_t headers[1024], int64_t meta[4]) {
    for (size_t i = 0; i < 4; ++i) meta[i] = 123;
    gh_input none = {NULL, 0};
    return gh_request((gh_input){(const uint8_t *)url, (int64_t)strlen(url)},
        0, none, none, 5000, 0, (gh_output){body, 64}, (gh_output){headers, 1024},
        (gh_metadata){meta, 4});
}
int main(int argc, char **argv) {
    CHECK(argc == 4);
    /* Fixtures provide an ephemeral IPv4 origin and a localhost-only certificate. */
    static const char prefix[] = "https://127.0.0.1";
    CHECK(!strncmp(argv[2], prefix, sizeof prefix - 1));
    const char *port = argv[2] + sizeof prefix - 1;
    CHECK(*port == ':');
    char good[256], wrong[256];
    CHECK(snprintf(good, sizeof good, "https://localhost%s/binary", port) < (int)sizeof good);
    CHECK(snprintf(wrong, sizeof wrong, "%s/binary", argv[2]) < (int)sizeof wrong);
    uint8_t body[64], headers[1024]; int64_t meta[4];
    /* Same matching hostname, first with system trust only, then test CA trust. */
    CHECK(request(good, body, headers, meta) == TRANSPORT);
    CHECK(meta[0] == CURLE_PEER_FAILED_VERIFICATION && meta[2] == 0 && meta[3] == 0);
    trusted_ca = argv[3];
    CHECK(request(good, body, headers, meta) == OK);
    static const uint8_t expected[] = "goose\0\xff\r\nbody";
    CHECK(meta[0] == CURLE_OK && meta[1] == 200 && meta[2] == sizeof expected - 1);
    CHECK(!memcmp(body, expected, sizeof expected - 1) && meta[3] > 0);
    /* Certificate is trusted, but the IPv4 host is absent from its DNS-only SAN. */
    CHECK(request(wrong, body, headers, meta) == TRANSPORT);
    CHECK(meta[0] == CURLE_PEER_FAILED_VERIFICATION && meta[2] == 0 && meta[3] == 0);
    puts("native HTTPS trust and hostname verification passed");
    return 0;
}
