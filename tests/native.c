/* SPDX-License-Identifier: Apache-2.0 */
#include "../native/http.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "native:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { OK, INVALID, SETUP, TRANSPORT, TIMEOUT, BODY, HEADERS };
static const unsigned char binary[] = "goose\0\xff\r\nbody";
static const char *base;
static gh_input input(const void *p, size_t n) { return (gh_input){p, (int64_t)n}; }
static int64_t call(const char *path, int64_t method, gh_input fields, gh_input upload,
                    int64_t timeout, unsigned char *out, size_t out_len,
                    unsigned char *head, size_t head_len, int64_t meta[4]) {
    char url[9000];
    CHECK(snprintf(url, sizeof url, "%s%s", base, path) < (int)sizeof url);
    return gh_request(input(url, strlen(url)), method, fields, upload, timeout, 1,
                      (gh_output){out, (int64_t)out_len}, (gh_output){head, (int64_t)head_len},
                      (gh_metadata){meta, 4});
}
static void *concurrent(void *unused) {
    (void)unused;
    unsigned char out[64], head[1024]; int64_t meta[4];
    CHECK(call("/binary", 0, input(NULL, 0), input(NULL, 0), 2000,
               out, sizeof out, head, sizeof head, meta) == OK);
    CHECK(meta[1] == 200 && meta[2] == sizeof binary - 1);
    CHECK(!memcmp(out, binary, sizeof binary - 1));
    return NULL;
}
int main(int argc, char **argv) {
    CHECK(argc == 3); base = argv[1];
    /* First-ever adapter calls race global initialization. */
    pthread_t threads[8];
    for (size_t i = 0; i < 8; ++i) CHECK(!pthread_create(&threads[i], NULL, concurrent, NULL));
    for (size_t i = 0; i < 8; ++i) CHECK(!pthread_join(threads[i], NULL));
    unsigned char out[4096], head[4096]; int64_t meta[4];
    gh_input none = input(NULL, 0);
    CHECK(call("/binary", 0, none, none, 2000, out, sizeof out, head, sizeof head, meta) == OK);
    size_t header_size = (size_t)meta[3];
    CHECK(header_size > 0 && meta[0] == CURLE_OK);
    CHECK(call("/binary", 0, none, none, 2000, out, sizeof binary - 1, head, header_size, meta) == OK);
    CHECK(call("/binary", 0, none, none, 2000, out, sizeof binary - 2, head, sizeof head, meta) == BODY);
    CHECK(meta[2] == 0 && meta[3] == 0);
    CHECK(call("/binary", 0, none, none, 2000, out, sizeof out, head, header_size - 1, meta) == HEADERS);
    CHECK(meta[2] == 0 && meta[3] == 0);
    CHECK(call("/empty", 0, none, none, 2000, NULL, 0, head, sizeof head, meta) == OK);
    CHECK(meta[2] == 0);
    CHECK(call("/binary", 0, none, none, 2000, NULL, 0, head, sizeof head, meta) == BODY);
    CHECK(call("/empty", 0, none, none, 2000, out, sizeof out, NULL, 0, meta) == HEADERS);
    CHECK(call("/echo", 1, none, input(binary, sizeof binary - 1), 2000,
               out, sizeof out, head, sizeof head, meta) == OK);
    CHECK(meta[2] == sizeof binary - 1 && !memcmp(out, binary, sizeof binary - 1));
    CHECK(call("/echo", 1, none, none, 2000, out, sizeof out, head, sizeof head, meta) == OK && meta[2] == 0);
    CHECK(call("/status", 0, none, none, 2000, out, sizeof out, head, sizeof head, meta) == OK);
    CHECK(meta[1] == 404 && meta[2] == 7);
    CHECK(call("/redirect", 0, none, none, 2000, out, sizeof out, head, sizeof head, meta) == OK);
    CHECK(meta[1] == 302 && meta[2] == 8 && !memcmp(out, "redirect", 8));
    CHECK(call("/partial", 0, none, none, 2000, out, sizeof out, head, sizeof head, meta) == TRANSPORT);
    CHECK(meta[0] == CURLE_PARTIAL_FILE && meta[2] == 0 && meta[3] == 0);
    static const char *truncated[] = {"/truncated-status", "/truncated-headers", "/truncated-after-informational", "/truncated-lowercase-status"};
    for (size_t i = 0; i < sizeof truncated / sizeof *truncated; ++i) {
        CHECK(call(truncated[i], 0, none, none, 2000, out, sizeof out, head, sizeof head, meta) == TRANSPORT);
        CHECK(meta[0] != CURLE_OK && meta[1] == 200 && meta[2] == 0 && meta[3] == 0);
    }
    CHECK(call("/timeout", 0, none, none, 50, out, sizeof out, head, sizeof head, meta) == TIMEOUT);
    CHECK(meta[0] == CURLE_OPERATION_TIMEDOUT && meta[2] == 0 && meta[3] == 0);
    CHECK(call("/trailers", 0, none, none, 2000, out, sizeof out, head, sizeof head, meta) == OK);
    CHECK(meta[1] == 200 && meta[2] == 3 && !memcmp(out, "abc", 3));
    head[meta[3]] = 0;
    CHECK(strstr((char *)head, "103 Early Hints\r\n") && strstr((char *)head, "X-End: done\r\n"));
    CHECK(call("/gzip", 0, none, none, 2000, out, sizeof out, head, sizeof head, meta) == OK);
    CHECK(meta[2] > 2 && out[0] == 0x1f && out[1] == 0x8b);
    const char empty_header[] = "X-Empty:\0";
    CHECK(call("/empty-header", 0, input(empty_header, sizeof empty_header - 1), none, 2000,
               out, sizeof out, head, sizeof head, meta) == OK);
    CHECK(meta[2] == 3 && !memcmp(out, "yes", 3));
    const char spaced_header[] = "X-Empty: \0";
    CHECK(call("/empty-header", 0, input(spaced_header, sizeof spaced_header - 1), none, 2000,
               out, sizeof out, head, sizeof head, meta) == OK);
    CHECK(meta[2] == 3 && !memcmp(out, "yes", 3));
    const char tab_header[] = "X-Empty: \t \0";
    CHECK(call("/empty-header", 0, input(tab_header, sizeof tab_header - 1), none, 2000,
               out, sizeof out, head, sizeof head, meta) == OK);
    CHECK(meta[2] == 3 && !memcmp(out, "yes", 3));
    CHECK(gh_request(input(argv[2], strlen(argv[2])), 0, none, none, 2000, 0,
                     (gh_output){out, sizeof out}, (gh_output){head, sizeof head},
                     (gh_metadata){meta, 4}) == TRANSPORT);
    CHECK(meta[0] == CURLE_PEER_FAILED_VERIFICATION && meta[2] == 0 && meta[3] == 0);
    static const char *bad_urls[] = {"file:///etc/passwd", "ftp://example.com", "http://localhost/",
        "https://user:pass@localhost/", "https://localhost/#secret", "https:///", "https://local host/", "https://localhost/\r\n"};
    for (size_t i = 0; i < sizeof bad_urls / sizeof *bad_urls; ++i) {
        CHECK(gh_request(input(bad_urls[i], strlen(bad_urls[i])), 0, none, none, 200, 0,
                         (gh_output){out, sizeof out}, (gh_output){head, sizeof head},
                         (gh_metadata){meta, 4}) == INVALID);
    }
    char empty_authority[9000];
    CHECK(!strncmp(base, "http://", 7));
    CHECK(snprintf(empty_authority, sizeof empty_authority, "http:///%s/binary", base + 7) < (int)sizeof empty_authority);
    CHECK(gh_request(input(empty_authority, strlen(empty_authority)), 0, none, none, 2000, 1,
                     (gh_output){out, sizeof out}, (gh_output){head, sizeof head},
                     (gh_metadata){meta, 4}) == INVALID);
    CHECK(!strncmp(argv[2], "https://", 8));
    CHECK(snprintf(empty_authority, sizeof empty_authority, "https:///%s/binary", argv[2] + 8) < (int)sizeof empty_authority);
    CHECK(gh_request(input(empty_authority, strlen(empty_authority)), 0, none, none, 2000, 0,
                     (gh_output){out, sizeof out}, (gh_output){head, sizeof head},
                     (gh_metadata){meta, 4}) == INVALID);
    static const char *bad_headers[] = {"Host: evil", "cOnTeNt-LeNgTh: 5", "Transfer-Encoding: chunked",
        "Connection: keep-alive", "Proxy-Connection: x", "TE: trailers", "Trailer: x", "Upgrade: x",
        "Expect: x", "Accept-Encoding: gzip", "X-Test: ok\r\nInjected: yes", "Bad Name: x", ": empty", "NoColon"};
    for (size_t i = 0; i < sizeof bad_headers / sizeof *bad_headers; ++i)
        CHECK(call("/binary", 0, input(bad_headers[i], strlen(bad_headers[i]) + 1), none, 200,
                   out, sizeof out, head, sizeof head, meta) == INVALID);
    CHECK(call("/binary", 0, input("unterminated", 12), none, 200, out, sizeof out, head, sizeof head, meta) == INVALID);
    CHECK(call("/binary", 0, none, input(binary, sizeof binary - 1), 200, out, sizeof out, head, sizeof head, meta) == INVALID);
    CHECK(call("/binary", 2, none, none, 200, out, sizeof out, head, sizeof head, meta) == INVALID);
    CHECK(call("/binary", 0, none, none, 0, out, sizeof out, head, sizeof head, meta) == INVALID);
    CHECK(call("/binary", 0, none, none, 86400001, out, sizeof out, head, sizeof head, meta) == INVALID);
    CHECK(call("/binary", 0, none, none, 200, out, sizeof out, out, sizeof out, meta) == INVALID);
    CHECK(gh_request(input(argv[1], strlen(argv[1])), 0, none, none, 200, 1,
                     (gh_output){NULL, 1}, (gh_output){head, sizeof head}, (gh_metadata){meta, 4}) == INVALID);
    CHECK(gh_request(input(argv[1], strlen(argv[1])), 0, none, none, 200, 1,
                     (gh_output){out, -1}, (gh_output){head, sizeof head}, (gh_metadata){meta, 4}) == INVALID);
    puts("native HTTP integration passed");
    return 0;
}
