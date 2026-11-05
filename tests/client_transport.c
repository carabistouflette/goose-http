/* SPDX-License-Identifier: Apache-2.0 */
/* Linked into the public API test only, never the production native library. */
#include "../native/http.h"
#include <curl/curl.h>
#include <string.h>

static char origin[8193], ca[4097];
int64_t gh_test_client_trust(gh_input address, gh_input certificate) {
    if (address.len < 18 || address.len >= (int64_t)sizeof origin ||
        certificate.len < 1 || certificate.len >= (int64_t)sizeof ca ||
        memcmp(address.data, "https://localhost:", 18) ||
        memchr(address.data, 0, (size_t)address.len) ||
        memchr(certificate.data, 0, (size_t)certificate.len)) return -1;
    memcpy(origin, address.data, (size_t)address.len); origin[address.len] = 0;
    memcpy(ca, certificate.data, (size_t)certificate.len); ca[certificate.len] = 0;
    return 0;
}
CURLcode __real_curl_easy_perform(CURL *easy);
CURLcode __wrap_curl_easy_perform(CURL *easy) {
    if (*origin) {
        char *address = NULL;
        size_t n = strlen(origin);
        if (curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &address) != CURLE_OK ||
            !address || strncmp(address, origin, n) || (address[n] != '/' && address[n] != 0))
            return CURLE_URL_MALFORMAT;
        CURLcode code = curl_easy_setopt(easy, CURLOPT_CAINFO, ca);
        if (code != CURLE_OK) return code;
    }
    return __real_curl_easy_perform(easy);
}
