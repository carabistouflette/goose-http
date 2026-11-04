/* SPDX-License-Identifier: Apache-2.0 */
#include "../.build/metadata_coverage_export.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t valid_revision[] = "0123456789abcdef0123456789abcdef01234567";
static const uint8_t valid_headers[] = "HTTP/1.1 200 OK\r\n\r\n";
static const uint8_t valid_body[] =
    "{\"sha\":\"0123456789abcdef0123456789abcdef01234567\","
    "\"html_url\":\"https://example.invalid/metadata\","
    "\"commit\":{\"message\":\"subject\\nbody\"}}";

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    goose_init(*argc, *argv);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 262145) return 0;

    sl_u8 rev_valid = { (uint8_t *)valid_revision, 40 };
    sl_u8 head_valid = { (uint8_t *)valid_headers, sizeof(valid_headers) - 1 };
    sl_u8 body_valid = { (uint8_t *)valid_body, sizeof(valid_body) - 1 };

    sl_u8 arbitrary = { (uint8_t *)data, (int64_t)size };

    /* 1. Direct body fuzzing with valid revision and valid headers */
    fuzz_metadata_validate(rev_valid, 200, arbitrary, head_valid);

    /* 2. Direct headers fuzzing with valid revision and valid body */
    fuzz_metadata_validate(rev_valid, 200, body_valid, arbitrary);

    /* 3. Split input: dual header and body fuzzing */
    if (size >= 2) {
        size_t remaining = size - 2;
        size_t hlen = ((size_t)data[0] | ((size_t)data[1] << 8)) % (remaining + 1);
        size_t blen = remaining - hlen;
        sl_u8 dual_headers = { (uint8_t *)(data + 2), (int64_t)hlen };
        sl_u8 dual_body = { (uint8_t *)(data + 2 + hlen), (int64_t)blen };
        fuzz_metadata_validate(rev_valid, 200, dual_body, dual_headers);
    }

    /* 4. Arbitrary revision fuzzing with valid body and headers */
    fuzz_metadata_validate(arbitrary, 200, body_valid, head_valid);

    /* 5. Status code variation */
    if (size > 0) {
        int64_t status = (int64_t)data[0];
        fuzz_metadata_validate(rev_valid, status, arbitrary, head_valid);
    }

    return 0;
}
