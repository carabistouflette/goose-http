// SPDX-License-Identifier: Apache-2.0
// Coverage-guided fuzzing entry point for the metadata validator.
import http from "..";
import text from "carabistouflette/goose-text@46059e30be79ece6efd4317eb0adc6d213c808ab";
import honk_metadata from "../examples";
import std;

export fn fuzz_metadata_validate(revision: const u8[:], status_code: i64, body_input: const u8[:], headers_input: const u8[:]) -> i32 {
    var body = str(body_input);
    let status = http::Status { status_code: status_code };
    var info, error = honk_metadata::validate(revision, status, body, headers_input);
    body.clear();
    if error.len > 0 {
        assert(info.requested_revision.len == 0 && info.sha.len == 0 && info.html_url.len == 0 && info.subject.len == 0);
        return 0;
    }
    assert(info.requested_revision == revision && info.sha == revision);
    assert(text::valid_utf8(info.requested_revision) && text::valid_utf8(info.sha));
    assert(text::valid_utf8(info.html_url) && text::valid_utf8(info.subject));
    assert(info.html_url.len > 0);
    for c in info.subject { assert(c != '\r' && c != '\n'); }
    return 1;
}

fn main() {
}
