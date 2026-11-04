/* SPDX-License-Identifier: Apache-2.0 */
#include "../native/http.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "clock_failures:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { OK, INVALID, SETUP, TRANSPORT, TIMEOUT, BODY, HEADERS };

static int fail_clock = 0;

int __real_clock_gettime(clockid_t clk_id, struct timespec *tp);
int __wrap_clock_gettime(clockid_t clk_id, struct timespec *tp) {
    if (fail_clock) {
        errno = EIO;
        return -1;
    }
    return __real_clock_gettime(clk_id, tp);
}

int main(void) {
    gh_input url = { (const uint8_t *)"https://example.com/test", 24 };
    gh_input none = { NULL, 0 };
    int64_t meta[7];

    /* Case 1: clock_gettime failure during stream open -> must return GH_SETUP */
    fail_clock = 1;
    CHECK(gh_stream_open(url, none, 1000, 1000, 4096, 1048576, 65536, 0, (gh_metadata){meta, 3}) == SETUP);
    CHECK(meta[0] == 0);

    /* Case 2: clock_gettime failure after start (during gh_stream_step) -> must return error with GH_TRANSPORT */
    fail_clock = 0;
    CHECK(gh_stream_open(url, none, 1000, 1000, 4096, 1048576, 65536, 0, (gh_metadata){meta, 3}) == OK);
    int64_t hid = meta[0];
    CHECK(hid > 0);

    fail_clock = 1;
    int64_t step_res = gh_stream_step(hid, 100, (gh_metadata){meta, 7});
    CHECK(step_res == 3); /* Error step */
    CHECK(meta[0] == 3);
    CHECK(meta[1] == TRANSPORT); /* Error.Transport */

    /* Close must succeed and release slot */
    fail_clock = 0;
    int64_t cmeta[2];
    CHECK(gh_stream_close(hid, (gh_metadata){cmeta, 2}) == OK);

    puts("native stream clock failure regression passed");
    return 0;
}
