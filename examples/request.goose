// SPDX-License-Identifier: Apache-2.0
import http from "..";
import os;
import std;
fn main() {
    let argv = args();
    if argv.len != 2 { print("usage: request HTTPS_URL"); return; }
    var body:    u8[>..] = [];
    var headers: u8[>..] = [];
    let options = http::Options { .. };
    let status = http::request(argv[1], options, body, headers);
    if !http::success(status) { print("request failed: ", status.error, ", HTTP ", status.status_code); return; }
    print(body);
}
