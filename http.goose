// SPDX-License-Identifier: Apache-2.0
namespace http;
import std;

enum Method { Get, Post }
enum Error { None, InvalidInput, SetupFailed, Transport, TimedOut, BodyLimit, HeaderLimit }
struct Header { name: const u8[:], value: const u8[:] }
const NO_HEADERS: Header[0] = [];
struct Options {
    method:       Method          = Method.Get,
    headers:      const Header[:] = NO_HEADERS,
    body:         const u8[:]     = "",
    timeout_ms:   i64             = 30000,
    body_limit:   i64             = 1048576,
    header_limit: i64             = 65536,
    allow_http:   bool            = false,
}
struct Status {
    error:          Error = Error.None,
    transport_code: i64   = 0,
    status_code:    i64   = 0,
}
extern fn native_request(url: const u8[:], method: i64, packed: const u8[:],
    upload: const u8[:], timeout_ms: i64, allow_http: i64,
    body: u8[:], headers: u8[:], meta: i64[:]) -> i64 = "gh_request";

fn success(status: Status) -> bool {
    return status.error == Error.None && status.status_code >= 200 && status.status_code < 300;
}
fn token(c: u8) -> bool {
    if (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') { return true; }
    for t in "!#$%&'*+-.^_`|~" { if c == t { return true; } }
    return false;
}
fn equal_name(name: const u8[:], lower: const u8[:]) -> bool {
    if name.len != lower.len { return false; }
    for i in name.len {
        let c = if name[i] >= 'A' && name[i] <= 'Z' { name[i] + 32 } else { name[i] };
        if c != lower[i] { return false; }
    }
    return true;
}
fn valid_header(header: Header) -> bool {
    if header.name.len == 0 { return false; }
    for c in header.name { if !token(c) { return false; } }
    for c in header.value { if c != 9 && (c < 32 || c > 126) { return false; } }
    if equal_name(header.name, "accept-encoding") { return equal_name(header.value, "identity"); }
    for name in ["host", "content-length", "transfer-encoding", "connection", "proxy-connection", "te", "trailer", "upgrade", "expect"] {
        if equal_name(header.name, name) { return false; }
    }
    return true;
}

// Inputs cannot borrow either output; the compiler checks lifetime separation.
fn request(url: const u8[:], options: const Options&, body: u8[>..]&, headers: u8[>..]&) -> Status {
    body.clear(); headers.clear();
    let invalid = Status { error: Error.InvalidInput };
    if body .== headers { return invalid; }
    if url.len == 0 || url.len > 8192 || options.headers.len > 64 ||
        options.body.len > 1048576 || (options.method == Method.Get && options.body.len > 0) ||
        options.timeout_ms < 1 || options.timeout_ms > 86400000 ||
        options.body_limit < 0 || options.body_limit > 16777216 ||
        options.header_limit < 0 || options.header_limit > 262144 { return invalid; }
    if !starts_with(url, "https://") && !(options.allow_http && starts_with(url, "http://")) { return invalid; }
    let authority = if starts_with(url, "https://") { 8 } else { 7 };
    if url.len <= authority || url[authority] == '/' || url[authority] == '?' { return invalid; }
    for c in url { if c <= 32 || c >= 127 || c == '#' { return invalid; } }
    var total: i64 = 0;
    for header in options.headers {
        if header.name.len > 65536 || header.value.len > 65536 || !valid_header(header) { return invalid; }
        total += header.name.len + header.value.len + 4;
        if total > 65536 { return invalid; }
    }
    var packed: u8[>..] = [];
    for header in options.headers {
        packed.append(header.name); packed.append(": "); packed.append(header.value); packed.push(0);
    }
    body.resize(options.body_limit, 0); headers.resize(options.header_limit, 0);
    var meta: i64[4] = [0, 0, 0, 0];
    let code = native_request(url, if options.method == Method.Get { 0 } else { 1 }, packed,
        options.body, options.timeout_ms, if options.allow_http { 1 } else { 0 }, body[..], headers[..], meta[..]);
    body.resize(meta[2]); headers.resize(meta[3]);
    let error = if code == 0 { Error.None } else if code == 1 { Error.InvalidInput }
        else if code == 2 { Error.SetupFailed } else if code == 3 { Error.Transport }
        else if code == 4 { Error.TimedOut } else if code == 5 { Error.BodyLimit } else { Error.HeaderLimit };
    return Status { error: error, transport_code: meta[0], status_code: meta[1] };
}
