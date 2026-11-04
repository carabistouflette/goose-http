/* SPDX-License-Identifier: Apache-2.0 */
#include "../native/http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "stream_retire:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { OK, INVALID, SETUP, TRANSPORT, TIMEOUT, BODY, HEADERS };

int main(void) {
    int64_t meta[3];
    const char *url_str = "https://example.com/test";
    gh_input url = { (const uint8_t *)url_str, (int64_t)strlen(url_str) };
    gh_input headers = { NULL, 0 };
    int64_t saved_handles[GH_MAX_STREAMS * 2];
    size_t count = 0;

    // Exhaust all 32 slots with GH_MAX_GENERATION = 3.
    // Each slot starts at gen 1, closes to gen 2 (can be opened once more),
    // then closes to gen 3 (clamped, retired).
    for (size_t cycle = 0; cycle < 2; ++cycle) {
        for (size_t s = 0; s < GH_MAX_STREAMS; ++s) {
            memset(meta, 0, sizeof(meta));
            int64_t res = gh_stream_open(url, headers, 1000, 1000, 4096, 1048576, 65536, 0,
                                         (gh_metadata){meta, 3});
            if (res != OK) {
                fprintf(stderr, "cycle=%zu s=%zu res=%ld\n", cycle, s, (long)res);
                exit(1);
            }
            int64_t hid = meta[0];
            CHECK(hid > 0);
            saved_handles[count++] = hid;

            int64_t close_meta[2];
            CHECK(gh_stream_close(hid, (gh_metadata){close_meta, 2}) == OK);
        }
    }

    // Now all 32 slots are retired (generation == 3 == GH_MAX_GENERATION).
    // The next open must fail with GH_SETUP because no free slot with gen < MAX exists.
    memset(meta, 0, sizeof(meta));
    int64_t fail_res = gh_stream_open(url, headers, 1000, 1000, 4096, 1048576, 65536, 0,
                                       (gh_metadata){meta, 3});
    CHECK(fail_res == SETUP);
    CHECK(meta[0] == 0);

    // All previously issued handle IDs must be rejected as INVALID.
    for (size_t i = 0; i < count; ++i) {
        int64_t step_meta[7];
        CHECK(gh_stream_step(saved_handles[i], 100, (gh_metadata){step_meta, 7}) == 3);
        CHECK(step_meta[1] == INVALID);

        int64_t close_meta[2];
        CHECK(gh_stream_close(saved_handles[i], (gh_metadata){close_meta, 2}) == INVALID);
    }

    puts("stream generation retirement and exhaustion passed");
    return 0;
}
