// SPDX-License-Identifier: Apache-2.0
import http from "..";
import std;
import os;
extern fn test_trust(origin: const u8[:], ca: const u8[:]) -> i64 = "gh_test_client_trust";

fn request_ok(client: http::ClientHandle&, address: const u8[:], fields: const http::Header[:]) {
    var body:    u8[>..] = [];
    var headers: u8[>..] = [];
    let options = http::Options { headers: fields, body_limit: 4096, header_limit: 4096 };
    let status = http::request(client, address, options, body, headers);
    assert(http::success(status) && body == "ok" && headers.len > 0);
}

fn main() {
    let argv = args();
    assert(argv.len == 3 || argv.len == 4);
    let origin = argv[1];
    let options = http::Options { body_limit: 4096, header_limit: 4096 };
    var client = http::ClientHandle {};
    var body:    u8[>..] = [];
    var headers: u8[>..] = [];
    assert(http::close_client(client).error == http::Error.None);
    assert(http::open_client("http://localhost", client).error == http::Error.InvalidInput && client.id == 0);
    assert(http::open_client(origin, client).error == http::Error.None && client.id != 0);
    let id = client.id;
    assert(http::open_client(origin, client).error == http::Error.InvalidInput && client.id == id);
    if argv.len == 4 {
        assert(argv[3] == "--wrong-hostname");
        assert(test_trust(origin, argv[2]) == 0);
        let status = http::request(client, str(origin, "/forbidden"), options, body, headers);
        assert(status.error == http::Error.Transport && status.transport_code == 60 && body.len == 0 && headers.len == 0);
        assert(http::close_client(client).error == http::Error.None);
        print("{\"hostname_verification\":\"passed\"}");
        return;
    }
    let untrusted = http::request(client, str(origin, "/untrusted"), options, body, headers);
    assert(untrusted.error == http::Error.Transport && untrusted.transport_code == 60 && body.len == 0 && headers.len == 0);
    assert(test_trust(origin, argv[2]) == 0);
    request_ok(client, str(origin, "/id"), http::NO_HEADERS);
    let fields = [http::Header { name: "Authorization", value: "Bearer private" }, http::Header { name: "X-Custom", value: "once" }];
    request_ok(client, str(origin, "/auth"), fields);
    request_ok(client, str(origin, "/clean"), http::NO_HEADERS);
    let post = http::Options { method: http::Method.Post };
    assert(http::request(client, str(origin, "/forbidden"), post, body, headers).error == http::Error.InvalidInput);
    assert(body.len == 0 && headers.len == 0);
    assert(http::request(client, "https://elsewhere.invalid/forbidden", options, body, headers).error == http::Error.InvalidInput);
    assert(body.len == 0 && headers.len == 0);
    assert(http::request(client, str(origin, "/forbidden"), options, body, body).error == http::Error.InvalidInput);
    assert(body.len == 0);
    var copied = http::ClientHandle { id: client.id };
    assert(http::close_client(client).error == http::Error.None && client.id == 0);
    assert(http::request(copied, str(origin, "/forbidden"), options, body, headers).error == http::Error.InvalidInput);
    assert(http::close_client(copied).error == http::Error.InvalidInput);
    assert(http::request(client, str(origin, "/forbidden"), options, body, headers).error == http::Error.InvalidInput);
    assert(http::close_client(client).error == http::Error.None);
    print("{\"public_clients\":\"passed\"}");
}
