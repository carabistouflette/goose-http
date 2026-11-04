// SPDX-License-Identifier: Apache-2.0
import http from "..";
import text from "carabistouflette/goose-text@46059e30be79ece6efd4317eb0adc6d213c808ab";
import honk_metadata from "../examples";
import std;
import os;
const REVISION = "0123456789abcdef0123456789abcdef01234567";
fn one(body: u8[:]) -> i64 {
    var info, error = honk_metadata::validate(REVISION, http::Status { status_code: 200 }, body, "HTTP/1.1 200 OK\r\n\r\n");
    assert(error.len == 0 && info.sha == REVISION);
    let rendered = honk_metadata::render(info);
    return rendered.len;
}
fn main() {
    let argv = args(); assert(argv.len == 3);
    let iterations, valid = parse_int(argv[2]); assert(valid && iterations >= 1 && iterations <= 1000000);
    var body:     u8[>..] = []; assert(read_file(argv[1], body) && body.len <= honk_metadata::MAX_BODY);
    var checksum: i64     = 0;
    for i in 100 { checksum += one(body); }
    let started = clock_ns();
    for i in iterations { checksum += one(body); }
    let elapsed = clock_ns() - started;
    assert(checksum > 0);
    print("{\"iterations\":", iterations, ",\"warmup\":100,\"elapsed_ns\":", elapsed, ",\"checksum\":", checksum, "}");
}
