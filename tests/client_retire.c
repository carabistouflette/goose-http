/* SPDX-License-Identifier: Apache-2.0 */
#include "../native/http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "client_retire:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { OK, INVALID, SETUP };

int main(void) {
    static const char origin[] = "https://localhost";
    gh_input url = {(const uint8_t *)origin, sizeof origin - 1}, none = {NULL, 0};
    int64_t saved[GH_MAX_CLIENTS * 2], ids[GH_MAX_CLIENTS], meta[4];
    size_t count = 0;
    /* Built with GH_MAX_GENERATION=3: generations 1 and 2 are issued,
     * then every slot is permanently retired at generation 3. */
    for (size_t cycle = 0; cycle < 2; ++cycle) {
        for (size_t i = 0; i < GH_MAX_CLIENTS; ++i) {
            CHECK(gh_client_open(url, (gh_metadata){meta, 1}) == OK);
            CHECK(meta[0] > 0);
            ids[i] = saved[count++] = meta[0];
        }
        CHECK(gh_client_open(url, (gh_metadata){meta, 1}) == SETUP && meta[0] == 0);
        for (size_t i = 0; i < GH_MAX_CLIENTS; ++i) CHECK(gh_client_close(ids[i]) == OK);
    }
    CHECK(gh_client_open(url, (gh_metadata){meta, 1}) == SETUP && meta[0] == 0);
    for (size_t i = 0; i < count; ++i) {
        CHECK(gh_client_close(saved[i]) == INVALID);
        CHECK(gh_client_request(saved[i], url, 0, none, none, 100, 0,
            (gh_output){NULL, 0}, (gh_output){NULL, 0}, (gh_metadata){meta, 4}) == INVALID);
        CHECK(meta[0] == 0 && meta[1] == 0 && meta[2] == 0 && meta[3] == 0);
    }
    CHECK(gh_client_close(0) == OK);
    puts("client generation retirement and exhaustion passed");
    return 0;
}
