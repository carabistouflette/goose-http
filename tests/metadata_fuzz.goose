// SPDX-License-Identifier: Apache-2.0
// Offline, generator-owned corpus: little-endian u32 byte length then body.
import http from "..";
import text from "carabistouflette/goose-text@46059e30be79ece6efd4317eb0adc6d213c808ab";
import honk_metadata from "../examples";
import std;
import os;

const REVISION = "0123456789abcdef0123456789abcdef01234567";
const HEADERS = "HTTP/1.1 200 OK\r\n\r\n";
const MAX_CORPUS = 32 * 1024 * 1024;
const MAX_CASES = 100000;

fn validate_owned(input: const u8[:]) -> honk_metadata::Info, u8[>..] {
    var body = str(input);
    var info, error = honk_metadata::validate(REVISION, http::Status { status_code: 200 }, body, HEADERS);
    // Invalidate parser input before the result escapes this lexical scope.
    body.clear();
    return info, error;
}

fn main() {
    let argv = args();
    assert(argv.len == 2);
    // stdlib read_file reads the whole file. The Python generator bounds its
    // corpus before writing; this assertion also rejects oversized test input.
    var corpus: u8[>..] = [];
    assert(read_file(argv[1], corpus) && corpus.len <= MAX_CORPUS);
    var at = 0;
    var cases = 0;
    while at < corpus.len {
        assert(cases < MAX_CASES && corpus.len - at >= 4);
        var length: i64 = 0;
        var factor: i64 = 1;
        for i in 4 { length += corpus[at + i] * factor; factor *= 256; }
        at += 4;
        assert(length <= honk_metadata::MAX_BODY + 1 && length <= corpus.len - at);
        var info, error = validate_owned(corpus[at..at + length]);
        at += length;
        cases++;
        if error.len > 0 {
            assert(info.requested_revision.len == 0 && info.sha.len == 0 && info.html_url.len == 0 && info.subject.len == 0);
            print("null");
        } else {
            assert(info.requested_revision == REVISION && info.sha == REVISION);
            assert(text::valid_utf8(info.requested_revision) && text::valid_utf8(info.sha));
            assert(text::valid_utf8(info.html_url) && text::valid_utf8(info.subject));
            for c in info.subject { assert(c != '\r' && c != '\n'); }
            print(honk_metadata::render(info));
        }
    }
    assert(at == corpus.len);
}
