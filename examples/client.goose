// SPDX-License-Identifier: Apache-2.0
import http from "..";
import std;
import os;

fn main() {
    let argv = args();
    if argv.len < 3 {
        write_stderr("usage: client HTTPS_ORIGIN URL [URL ...]\n");
        exit(2);
    }
    var client = http::ClientHandle {};
    if http::open_client(argv[1], client).error != http::Error.None { exit(1); }
    defer { assert(http::close_client(client).error == http::Error.None); }
    let options = http::Options { body_limit: 65536, header_limit: 16384 };
    for address in argv[2..] {
        var body:    u8[>..] = [];
        var headers: u8[>..] = [];
        let status = http::request(client, address, options, body, headers);
        print("error=", status.error, " status=", status.status_code, " bytes=", body.len);
    }
}
