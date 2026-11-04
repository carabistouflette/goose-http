// SPDX-License-Identifier: Apache-2.0
import http from "..";
import os;
import std;

fn invalid_url(url: const u8[:]) {
    var body = str("old body");
    var headers = str("old headers");
    let options = http::Options { allow_http: true };
    let status = http::request(url, options, body, headers);
    assert(status.error == http::Error.InvalidInput);
    assert(body.len == 0 && headers.len == 0 && !http::success(status));
}
fn invalid_options(options: http::Options) {
    var body = str("old body");
    var headers = str("old headers");
    let status = http::request("https://example.invalid", options, body, headers);
    assert(status.error == http::Error.InvalidInput);
    assert(body.len == 0 && headers.len == 0);
}
fn invalid_header(name: const u8[:], value: const u8[:]) {
    let fields = [http::Header { name: name, value: value }];
    invalid_options(http::Options { headers: fields });
}
fn contains(haystack: const u8[:], needle: const u8[:]) -> bool {
    if needle.len > haystack.len { return false; }
    for i in 0..haystack.len - needle.len + 1 {
        if haystack[i..i + needle.len] == needle { return true; }
    }
    return false;
}
fn empty_header(base: const u8[:]) {
    let fields = [http::Header { name: "X-Empty", value: "" }];
    let options = http::Options { allow_http: true, headers: fields };
    var body:    u8[>..] = [];
    var headers: u8[>..] = [];
    let status = http::request(str(base, "/empty-header"), options, body, headers);
    assert(http::success(status) && body == "yes");
}
fn maximum_requests(base: const u8[:]) {
    var fields: http::Header[>..] = [];
    for i in 64 { fields.push(http::Header { name: "X-Test", value: "yes" }); }
    let many_options = http::Options { allow_http: true, headers: fields };
    var body:    u8[>..] = [];
    var headers: u8[>..] = [];
    var status = http::request(str(base, "/empty"), many_options, body, headers);
    assert(http::success(status) && body.len == 0);
    var text: u8[>..] = [];
    text.resize(65531, 'x');
    let long_fields = [http::Header { name: "X", value: text }];
    let long_options = http::Options { allow_http: true, headers: long_fields };
    status = http::request(str(base, "/empty"), long_options, body, headers);
    assert(http::success(status));
    var upload: u8[>..] = [];
    upload.resize(1048576, 42);
    let post_options = http::Options { allow_http: true, method: http::Method.Post, body: upload };
    status = http::request(str(base, "/echo"), post_options, body, headers);
    assert(http::success(status) && body == upload);
}
fn main() {
    let argv = args();
    assert(argv.len == 3);
    let base: const u8[:] = argv[1];
    let tls:  const u8[:] = argv[2];
    for url in ["", "file:///etc/passwd", "ftp://example.invalid", "http://", "https://", "https://user:password@example.invalid", "https://user@example.invalid", "https://example.invalid/#fragment", "https://example.invalid/has space", "https://example.invalid/\r\nInjected", "https://example.invalid/\x00suffix", "https://example.invalid/\xff"] { invalid_url(url); }
    var long_url = str("https://example.invalid/");
    long_url.resize(8193, 'x');
    invalid_url(long_url);
    for timeout in [0, -1, 86400001] { invalid_options(http::Options { timeout_ms: timeout }); }
    for limit in [-1, 16777217] { invalid_options(http::Options { body_limit: limit }); }
    for limit in [-1, 262145] { invalid_options(http::Options { header_limit: limit }); }
    invalid_options(http::Options { body: "GET must not upload" });
    var upload: u8[>..] = [];
    upload.resize(1048577, 1);
    invalid_options(http::Options { method: http::Method.Post, body: upload });
    for name in ["", "bad name", "name:", "bad\r\nheader", "bad\x00name", "bad\xffname", "HOST", "cOnTeNt-LeNgTh", "Transfer-Encoding", "Connection", "Proxy-Connection", "TE", "Trailer", "Upgrade", "Expect", "Accept-Encoding"] { invalid_header(name, "value"); }
    for value in ["bad\rvalue", "bad\nvalue", "bad\x00value", "bad\x1fvalue", "bad\x7fvalue", "bad\xffvalue"] { invalid_header("X-Test", value); }
    var fields: http::Header[>..] = [];
    for i in 65 { fields.push(http::Header { name: "X-Test", value: "yes" }); }
    invalid_options(http::Options { headers: fields });
    var oversized: u8[>..] = [];
    oversized.resize(65533, 'x');
    invalid_header("X", oversized);

    var body:    u8[>..] = [];
    var headers: u8[>..] = [];
    var options = http::Options { allow_http: true };
    let binary_url = str(base, "/binary");
    var status = http::request(binary_url, options, body, headers);
    assert(http::success(status) && status.status_code == 200 && status.transport_code == 0);
    assert(body == "goose\x00\xff\r\nbody");
    assert(contains(headers, "HTTP/1.1 200") && contains(headers, "Content-Length: 13"));
    let saved = str(body);
    status = http::request(str(base, "/status"), options, body, headers);
    assert(status.error == http::Error.None && status.status_code == 404 && !http::success(status));
    assert(body == "missing" && saved == "goose\x00\xff\r\nbody");
    status = http::request(str(base, "/redirect"), options, body, headers);
    assert(status.error == http::Error.None && status.status_code == 302 && body == "redirect");
    assert(contains(headers, "Location: /binary"));
    options.method = http::Method.Post;
    options.body = "hello\x00\xffworld";
    status = http::request(str(base, "/echo"), options, body, headers);
    assert(http::success(status) && body == "hello\x00\xffworld");
    options.body = "";
    status = http::request(str(base, "/echo"), options, body, headers);
    assert(http::success(status) && body.len == 0);
    options.method = http::Method.Get;
    options.body_limit = 13;
    status = http::request(binary_url, options, body, headers);
    assert(http::success(status) && body.len == 13);
    options.body_limit = 12;
    status = http::request(binary_url, options, body, headers);
    assert(status.error == http::Error.BodyLimit && body.len == 0 && headers.len == 0);
    options.body_limit = 0;
    status = http::request(str(base, "/empty"), options, body, headers);
    assert(http::success(status) && body.len == 0);
    status = http::request(binary_url, options, body, headers);
    assert(status.error == http::Error.BodyLimit && body.len == 0 && headers.len == 0);
    options.body_limit = 1048576;
    status = http::request(binary_url, options, body, headers);
    assert(http::success(status));
    let exact_headers = headers.len;
    options.header_limit = exact_headers;
    status = http::request(binary_url, options, body, headers);
    assert(http::success(status) && headers.len == exact_headers);
    options.header_limit = exact_headers - 1;
    status = http::request(binary_url, options, body, headers);
    assert(status.error == http::Error.HeaderLimit && body.len == 0 && headers.len == 0);
    options.header_limit = 0;
    status = http::request(str(base, "/empty"), options, body, headers);
    assert(status.error == http::Error.HeaderLimit && body.len == 0 && headers.len == 0);
    options.header_limit = 65536;
    options.timeout_ms = 30;
    status = http::request(str(base, "/timeout"), options, body, headers);
    assert(status.error == http::Error.TimedOut && body.len == 0 && headers.len == 0);
    options.timeout_ms = 30000;
    status = http::request(str(base, "/partial"), options, body, headers);
    assert(status.error == http::Error.Transport && body.len == 0 && headers.len == 0);
    status = http::request(str(base, "/trailers"), options, body, headers);
    assert(http::success(status) && body == "abc");
    assert(contains(headers, "103 Early Hints") && contains(headers, "X-End: done"));
    status = http::request(str(base, "/gzip"), options, body, headers);
    assert(http::success(status) && body.len > 2 && body[0] == 31 && body[1] == 139);
    empty_header(base);
    maximum_requests(base);
    let defaults = http::Options { .. };
    status = http::request(binary_url, defaults, body, headers);
    assert(status.error == http::Error.InvalidInput && body.len == 0 && headers.len == 0);
    status = http::request(tls, defaults, body, headers);
    assert(status.error == http::Error.Transport && status.transport_code != 0 && body.len == 0 && headers.len == 0);
    status = http::request(binary_url, options, body, body);
    assert(status.error == http::Error.InvalidInput && body.len == 0);
    print("HTTP API tests passed");
}
