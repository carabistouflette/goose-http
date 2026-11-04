/* SPDX-License-Identifier: Apache-2.0 */
#include "../native/http.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    char *end = NULL;
    errno = 0;
    long iterations = strtol(argv[2], &end, 10);
    if (errno || *end || iterations < 1 || iterations > 100000) return 2;
    unsigned char body[4096], headers[1024];
    int64_t meta[4];
    for (long i = 0; i < iterations; ++i) {
        int64_t result = gh_request((gh_input){(const uint8_t *)argv[1], (int64_t)strlen(argv[1])},
            0, (gh_input){NULL, 0}, (gh_input){NULL, 0}, 2000, 1,
            (gh_output){body, sizeof body}, (gh_output){headers, sizeof headers}, (gh_metadata){meta, 4});
        if (result || meta[1] != 200 || meta[2] != 4096) {
            fprintf(stderr, "request failed: error=%lld curl=%lld status=%lld bytes=%lld\n",
                (long long)result, (long long)meta[0], (long long)meta[1], (long long)meta[2]);
            return 1;
        }
    }
    return 0;
}
