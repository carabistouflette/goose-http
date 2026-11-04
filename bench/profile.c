/* SPDX-License-Identifier: Apache-2.0 */
/* Include the selected snapshot so the callback workload uses its real private
 * buffer type and callback. No additional production symbols are exported. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#ifndef PROFILE_IMPLEMENTATION
#error "Build through bench/profile.py to select a native implementation"
#endif
#include PROFILE_IMPLEMENTATION

/* Real validation, allocation, curl setup, getinfo and cleanup still execute.
 * A transport error avoids fabricating successful HTTP header completion. */
CURLcode __wrap_curl_easy_perform(CURL *handle) {
    (void)handle;
    return CURLE_RECV_ERROR;
}
static int64_t (*volatile request_callback)(gh_input, int64_t, gh_input, gh_input,
    int64_t, int64_t, gh_output, gh_output, gh_metadata) = gh_request;
static size_t (*volatile body_callback)(char *, size_t, size_t, void *) = receive;
static volatile uint64_t checksum;
static uint64_t clock_ns(clockid_t clock) {
    struct timespec value;
    if (clock_gettime(clock, &value)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}
static void setup_requests(size_t iterations, size_t fields, bool blank) {
    static const uint8_t url[] = "https://example.invalid/profile";
    char packed[4096]; size_t used = 0;
    for (size_t i = 0; i < fields; ++i) {
        int n = snprintf(packed + used, sizeof packed - used,
            blank ? "X-Test-%02zu:\t  " : "X-Test-%02zu: abcdefghijklmnopqrstuvwxyz", i);
        assert(n > 0 && (size_t)n < sizeof packed - used);
        used += (size_t)n + 1;
    }
    uint8_t body[4096], headers[1024]; int64_t meta[4]; uint64_t sum = 0;
    for (size_t i = 0; i < iterations; ++i) {
        int64_t result = request_callback((gh_input){url, sizeof url - 1}, 0,
            (gh_input){(uint8_t *)packed, (int64_t)used}, (gh_input){NULL, 0}, 30000, 0,
            (gh_output){body, sizeof body}, (gh_output){headers, sizeof headers},
            (gh_metadata){meta, 4});
        assert(result == GH_TRANSPORT && meta[0] == CURLE_RECV_ERROR &&
               meta[1] == 0 && meta[2] == 0 && meta[3] == 0);
        sum += (uint64_t)result + (uint64_t)meta[0];
    }
    checksum += sum;
}
static void copy_body(size_t iterations, uint8_t *source, uint8_t *destination, size_t bytes) {
    uint64_t sum = 0;
    for (size_t i = 0; i < iterations; ++i) {
        buffer output = {destination, bytes, 0, false};
        size_t written = body_callback((char *)source, 1, bytes, &output);
        assert(written == bytes && output.used == bytes && !output.exceeded);
        sum += written;
    }
    checksum += sum;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) { puts(curl_version()); return 0; }
    if (argc != 3) return 2;
    char *end = NULL; errno = 0;
    unsigned long iterations = strtoul(argv[2], &end, 10);
    if (errno || !*argv[2] || *end || iterations < 1 || iterations > 100000000) return 2;
    const char *workload = argv[1]; size_t fields = 0, bytes = 0; bool blank = false;
    if (!strcmp(workload, "setup_zero")) fields = 0;
    else if (!strcmp(workload, "setup_one")) fields = 1;
    else if (!strcmp(workload, "setup_64")) fields = 64;
    else if (!strcmp(workload, "setup_64_blank")) { fields = 64; blank = true; }
    else if (!strcmp(workload, "callback_4096")) bytes = 4096;
    else if (!strcmp(workload, "callback_16384")) bytes = 16384;
    else return 2;
    uint8_t *source = NULL, *destination = NULL;
    const size_t warmup = 1000;
    if (bytes) {
        source = malloc(bytes); destination = malloc(bytes);
        if (!source || !destination) { free(source); free(destination); return 1; }
        memset(source, 'x', bytes); memset(destination, 0, bytes);
        copy_body(warmup, source, destination, bytes);
    } else setup_requests(warmup, fields, blank);
    uint64_t wall = clock_ns(CLOCK_MONOTONIC), cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    if (bytes) copy_body(iterations, source, destination, bytes);
    else setup_requests(iterations, fields, blank);
    cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    wall = clock_ns(CLOCK_MONOTONIC) - wall;
    if (bytes) assert(!memcmp(source, destination, bytes));
    printf("{\"iterations\":%lu,\"warmup_iterations\":%zu,\"wall_ns\":%" PRIu64
           ",\"cpu_ns\":%" PRIu64 ",\"checksum\":%" PRIu64 "}\n",
           iterations, warmup, wall, cpu, checksum);
    free(destination); free(source);
    return 0;
}
