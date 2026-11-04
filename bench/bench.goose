// SPDX-License-Identifier: Apache-2.0
import http from "..";
import os;
import std;

fn main() {
    let argv = args();
    assert(argv.len == 3);
    let iterations, valid = parse_int(argv[2]);
    assert(valid && iterations >= 1 && iterations <= 100000);
    // Use the public defaults deliberately: allocation/zero-fill of the 1 MiB
    // body and 64 KiB header limits is part of the measured application cost.
    let options = http::Options { allow_http: true };
    var body:     u8[>..] = [];
    var headers:  u8[>..] = [];
    var checksum: i64     = 0;
    for i in iterations {
        let status = http::request(argv[1], options, body, headers);
        assert(http::success(status) && body.len == 4096);
        checksum += body[i % body.len];
    }
    assert(checksum == iterations * 120);
}
