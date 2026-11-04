/* SPDX-License-Identifier: Apache-2.0 */
/* Link with --wrap=curl_easy_perform: fuzz input can never cause network I/O. */
#include "../native/http.h"
#include <curl/curl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

CURLcode __wrap_curl_easy_perform(CURL *handle) {
    (void)handle;
    return CURLE_RECV_ERROR;
}
static void check(int condition) { if (!condition) abort(); }
static void run(gh_input url, gh_input headers, gh_input upload, int64_t method,
                int64_t timeout, int64_t allow_http, size_t capacity) {
    unsigned char body[258], head[258]; int64_t meta[4] = {0};
    memset(body, 0xa5, sizeof body); memset(head, 0xa5, sizeof head);
    int64_t result = gh_request(url, method, headers, upload, timeout, allow_http,
        (gh_output){body + 1, (int64_t)capacity}, (gh_output){head + 1, (int64_t)capacity},
        (gh_metadata){meta, 4});
    check(result == 1 || result == 2 || result == 3);
    check(meta[2] == 0 && meta[3] == 0);
    /* No callbacks were invoked by the stub: neither output may change. */
    for (size_t i = 0; i < sizeof body; ++i) check(body[i] == 0xa5 && head[i] == 0xa5);
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536) return 0;
    static const char valid[] = "https://example.invalid/fuzz";
    gh_input address = {(const uint8_t *)valid, sizeof valid - 1};
    gh_input none = {NULL, 0}, arbitrary = {data, (int64_t)size};
    unsigned selector = size ? data[0] : 0;
    size_t capacity = selector;
    /* Arbitrary URL validation, arbitrary packed headers, and guaranteed valid
       setup/upload path all run on each input, keeping coverage meaningful. */
    run(arbitrary, none, none, 0, 100, 1, capacity);
    run(address, arbitrary, none, 0, 100, 0, capacity);
    run(address, none, arbitrary, 1, 100, 0, capacity);
    run(address, none, none, (int64_t)(selector % 4) - 1,
        selector & 1 ? 0 : 100, selector % 3, capacity);
    return 0;
}
