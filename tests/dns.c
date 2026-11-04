/* SPDX-License-Identifier: Apache-2.0 */
/* Linux/ELF interposition keeps DNS deterministic and never calls a resolver. */
#define _POSIX_C_SOURCE 200809L
#include "../native/http.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "dns:%d: %s\n", __LINE__, #x); exit(1); } } while (0)

#if defined(__linux__) && defined(__ELF__)
#include <errno.h>
#include <netdb.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

static atomic_uint resolver_calls, active_resolvers;
static double perform_seconds, cleanup_seconds;
static unsigned perform_calls, cleanup_calls;

static double now(void) {
    struct timespec value;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

/* Interpose the threaded resolver's libc entry point, including calls made
 * from a dynamically linked libcurl. There is no delegation to real DNS. */
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result) {
    (void)service; (void)hints;
    CHECK(node && !strcmp(node, "goose-http-dns-test.invalid"));
    CHECK(result);
    *result = NULL;
    atomic_fetch_add(&resolver_calls, 1);
    atomic_fetch_add(&active_resolvers, 1);
    struct timespec remaining = {0, 250000000};
    while (nanosleep(&remaining, &remaining) != 0) CHECK(errno == EINTR);
    atomic_fetch_sub(&active_resolvers, 1);
    return EAI_NONAME;
}

CURLcode __real_curl_easy_perform(CURL *handle);
void __real_curl_easy_cleanup(CURL *handle);
CURLcode __wrap_curl_easy_perform(CURL *handle) {
    double started = now();
    CURLcode code = __real_curl_easy_perform(handle);
    perform_seconds += now() - started;
    ++perform_calls;
    return code;
}
void __wrap_curl_easy_cleanup(CURL *handle) {
    double started = now();
    __real_curl_easy_cleanup(handle);
    cleanup_seconds += now() - started;
    ++cleanup_calls;
}
#endif

int main(void) {
#if !defined(__linux__) || !defined(__ELF__)
    puts("DNS cleanup test skipped: requires Linux/ELF libc interposition");
#else
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    if (version->ares) {
        puts("DNS cleanup test skipped: c-ares/hybrid resolver build, not threaded-only");
        return 0;
    }
    if (version->version_num < 0x075500 ||
        !(version->features & CURL_VERSION_ASYNCHDNS) ||
        !(version->features & CURL_VERSION_THREADSAFE) ||
        !(version->features & CURL_VERSION_SSL)) {
        puts("DNS cleanup test skipped: libcurl does not meet adapter requirements");
        return 0;
    }
    static const unsigned char address[] = "https://goose-http-dns-test.invalid/";
    unsigned char body[64], headers[1024];
    int64_t meta[4] = {123, 123, 123, 123};
    gh_input none = {NULL, 0};
    double started = now();
    int64_t code = gh_request((gh_input){address, sizeof address - 1}, 0,
        none, none, 25, 0, (gh_output){body, sizeof body},
        (gh_output){headers, sizeof headers}, (gh_metadata){meta, 4});
    double elapsed = now() - started;
    CHECK(atomic_load(&resolver_calls) > 0);
    CHECK(atomic_load(&active_resolvers) == 0);
    CHECK(code == 4 && meta[0] == CURLE_OPERATION_TIMEDOUT);
    CHECK(meta[1] == 0 && meta[2] == 0 && meta[3] == 0);
    CHECK(perform_calls == 1 && cleanup_calls == 1);
    /* Timing is observational: the API's transfer timeout does not cap the
     * threaded resolver cleanup. Scheduler timing has no pass/fail threshold. */
    printf("DNS cleanup passed: timeout=25ms, perform=%.3fs, cleanup=%.3fs, total=%.3fs; "
           "no resolver calls remain active\n", perform_seconds, cleanup_seconds, elapsed);
#endif
    return 0;
}
