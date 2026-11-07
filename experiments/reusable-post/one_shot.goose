// SPDX-License-Identifier: Apache-2.0
// Research-only loopback JSON-RPC consumer using the current one-shot API.
import http from "../..";
import std;
import os;

fn main() {
    let argv = args();
    if argv.len != 2 || argv[1].len == 0 || argv[1].len > 5 { exit(2); }
    for c in argv[1] { if c < '0' || c > '9' { exit(2); } }
    let url = str("http://127.0.0.1:", argv[1], "/rpc");
    let requests: (const u8[:])[2] = [
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_blockNumber\",\"params\":[]}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"eth_blockNumber\",\"params\":[]}",
    ];
    let expected: (const u8[:])[2] = [
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"0x1\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":\"0x1\"}",
    ];
    for i in 2 {
        let request_id = if i == 0 { "1" } else { "2" };
        let fields = [http::Header { name: "Content-Type", value: "application/json" },
                      http::Header { name: "X-Request-ID", value: request_id }];
        let options = http::Options { method: http::Method.Post, headers: fields[..], body: requests[i],
            timeout_ms: 3000, body_limit: 1024, header_limit: 4096, allow_http: true, .. };
        var body:    u8[>..] = [];
        var headers: u8[>..] = [];
        let status = http::request(url, options, body, headers);
        assert(http::success(status) && body == expected[i]);
    }
    print("{\"one_shot\":\"passed\"}");
}
