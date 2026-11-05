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
// Sequential, origin-bound reusable HTTPS GET client. Copies are aliases, not owners.
struct ClientHandle { id: u64 = 0 }
struct StreamOptions {
    headers:            const Header[:] = NO_HEADERS,
    timeout_ms:         i64             = 30000,
    connect_timeout_ms: i64             = 10000,
    chunk_limit:        i64             = 65536,
    body_limit:         i64             = 67108864,
    header_limit:       i64             = 65536,
    allow_http:         bool            = false,
}
enum StreamStep {
    Chunk,
    IdleTimeout,
    Done,
    Error,
    Cancelled,
}
struct StreamHandle {
    id:          u64 = 0,
    status_code: i64 = 0,
    total_bytes: i64 = 0,
}

extern fn native_request(url: const u8[:], method: i64, packed: const u8[:],
    upload: const u8[:], timeout_ms: i64, allow_http: i64,
    body: u8[:], headers: u8[:], meta: i64[:]) -> i64 = "gh_request";

extern fn native_client_open(origin: const u8[:], meta: i64[:]) -> i64 = "gh_client_open";
extern fn native_client_request(id: i64, url: const u8[:], method: i64, packed: const u8[:],
    upload: const u8[:], timeout_ms: i64, allow_http: i64,
    body: u8[:], headers: u8[:], meta: i64[:]) -> i64 = "gh_client_request";
extern fn native_client_close(id: i64) -> i64 = "gh_client_close";

extern fn native_stream_open(url: const u8[:], packed: const u8[:], timeout_ms: i64,
    connect_timeout_ms: i64, chunk_limit: i64, body_limit: i64, header_limit: i64,
    allow_http: i64, meta: i64[:]) -> i64 = "gh_stream_open";

extern fn native_stream_step(handle_id: i64, step_timeout_ms: i64, meta: i64[:]) -> i64 = "gh_stream_step";

extern fn native_stream_read_data(handle_id: i64, chunk: u8[:], headers: u8[:], meta: i64[:]) -> i64 = "gh_stream_read_data";

extern fn native_stream_read_headers(handle_id: i64, headers: u8[:], meta: i64[:]) -> i64 = "gh_stream_read_headers";

extern fn native_stream_close(handle_id: i64, meta: i64[:]) -> i64 = "gh_stream_close";

fn to_error(code: i64) -> Error {
    if code == 0 { return Error.None; }
    if code == 1 { return Error.InvalidInput; }
    if code == 2 { return Error.SetupFailed; }
    if code == 3 { return Error.Transport; }
    if code == 4 { return Error.TimedOut; }
    if code == 5 { return Error.BodyLimit; }
    return Error.HeaderLimit;
}

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
    return buffered_request(0, url, options, body, headers);
}

fn open_client(origin: const u8[:], client: ClientHandle&) -> Status {
    if client.id != 0 || origin.len == 0 || origin.len > 8192 { return Status { error: Error.InvalidInput }; }
    var meta: i64[1] = [0];
    let code = native_client_open(origin, meta[..]);
    if code == 0 { client.id = meta[0] as u64; }
    return Status { error: to_error(code) };
}

fn request(client: ClientHandle&, url: const u8[:], options: const Options&, body: u8[>..]&, headers: u8[>..]&) -> Status {
    if client.id == 0 || options.method != Method.Get {
        body.clear(); headers.clear();
        return Status { error: Error.InvalidInput };
    }
    return buffered_request(client.id, url, options, body, headers);
}

fn close_client(client: ClientHandle&) -> Status {
    let code = native_client_close(client.id as i64);
    // A concurrent/busy close is rejected without consuming the live token.
    if code == 0 { client.id = 0; }
    return Status { error: to_error(code) };
}

fn buffered_request(client_id: u64, url: const u8[:], options: const Options&, body: u8[>..]&, headers: u8[>..]&) -> Status {
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
    let code = if client_id == 0 {
        native_request(url, if options.method == Method.Get { 0 } else { 1 }, packed,
            options.body, options.timeout_ms, if options.allow_http { 1 } else { 0 }, body[..], headers[..], meta[..])
    } else {
        native_client_request(client_id as i64, url, if options.method == Method.Get { 0 } else { 1 }, packed,
            options.body, options.timeout_ms, if options.allow_http { 1 } else { 0 }, body[..], headers[..], meta[..])
    };
    body.resize(meta[2]); headers.resize(meta[3]);
    let error = to_error(code);
    return Status { error: error, transport_code: meta[0], status_code: meta[1] };
}

// Single logical reader: a given StreamHandle must be driven by a single logical reader
// thread sequentially between steps. Concurrent operations across distinct handles are safe.
fn open_stream(url: const u8[:], options: const StreamOptions&, handle: StreamHandle&) -> Status {
    let invalid = Status { error: Error.InvalidInput };
    if handle.id != 0 { return invalid; }
    handle.status_code = 0;
    handle.total_bytes = 0;
    if url.len == 0 || url.len > 8192 || options.headers.len > 64 ||
        options.timeout_ms < 1 || options.timeout_ms > 86400000 ||
        options.connect_timeout_ms < 1 || options.connect_timeout_ms > options.timeout_ms ||
        options.chunk_limit < 1 || options.chunk_limit > 1048576 ||
        options.body_limit < 0 ||
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
    var meta: i64[3] = [0, 0, 0];
    let code = native_stream_open(url, packed[..], options.timeout_ms, options.connect_timeout_ms,
        options.chunk_limit, options.body_limit, options.header_limit,
        if options.allow_http { 1 } else { 0 }, meta[..]);
    let err = to_error(code);
    if err == Error.None {
        handle.id = meta[0] as u64;
    }
    return Status { error: err, transport_code: meta[2], status_code: meta[1] };
}

fn read_chunk(handle: StreamHandle&, chunk_out: u8[>..]&, headers_out: u8[>..]&, step_timeout_ms: i32) -> StreamStep, Status {
    chunk_out.clear();
    let invalid_status = Status { error: Error.InvalidInput };
    if chunk_out .== headers_out || step_timeout_ms < 0 || handle.id == 0 {
        return StreamStep.Error, invalid_status;
    }
    var meta: i64[7] = [0, 0, 0, 0, 0, 0, 0];
    let step_ret = native_stream_step(handle.id as i64, step_timeout_ms, meta[..]);
    handle.status_code = meta[4];
    let err = to_error(meta[1]);
    let status = Status { error: err, transport_code: meta[6], status_code: meta[4] };

    if step_ret == 0 {
        let chunk_len = meta[2];
        chunk_out.resize(chunk_len, 0);
        let header_len = meta[3];
        headers_out.resize(header_len, 0);
        var copy_meta: i64[2] = [0, 0];
        let copy_code = native_stream_read_data(handle.id as i64, chunk_out[..], headers_out[..], copy_meta[..]);
        if copy_code != 0 || copy_meta[0] != chunk_len {
            chunk_out.clear();
            headers_out.clear();
            let copy_err = if copy_code != 0 { to_error(copy_code) } else { Error.Transport };
            return StreamStep.Error, Status { error: copy_err, transport_code: meta[6], status_code: meta[4] };
        }
        handle.total_bytes += copy_meta[0];
        return StreamStep.Chunk, status;
    }
    if step_ret == 1 {
        let header_len = meta[3];
        headers_out.resize(header_len, 0);
        if header_len > 0 {
            var copy_meta: i64[2] = [0, 0];
            let copy_code = native_stream_read_headers(handle.id as i64, headers_out[..], copy_meta[..]);
            if copy_code != 0 || copy_meta[0] != header_len {
                headers_out.clear();
                let copy_err = if copy_code != 0 { to_error(copy_code) } else { Error.Transport };
                return StreamStep.Error, Status { error: copy_err, transport_code: meta[6], status_code: meta[4] };
            }
        }
        return StreamStep.IdleTimeout, status;
    }
    if step_ret == 2 {
        let header_len = meta[3];
        headers_out.resize(header_len, 0);
        if header_len > 0 {
            var copy_meta: i64[2] = [0, 0];
            let copy_code = native_stream_read_headers(handle.id as i64, headers_out[..], copy_meta[..]);
            if copy_code != 0 || copy_meta[0] != header_len {
                headers_out.clear();
                let copy_err = if copy_code != 0 { to_error(copy_code) } else { Error.Transport };
                return StreamStep.Error, Status { error: copy_err, transport_code: meta[6], status_code: meta[4] };
            }
        }
        return StreamStep.Done, status;
    }
    if step_ret == 4 {
        return StreamStep.Cancelled, status;
    }
    return StreamStep.Error, status;
}

fn close_stream(handle: StreamHandle&) -> Status {
    if handle.id == 0 {
        return Status { error: Error.None, transport_code: 0, status_code: 0 };
    }
    var meta: i64[2] = [0, 0];
    let code = native_stream_close(handle.id as i64, meta[..]);
    handle.id = 0;
    return Status { error: to_error(code), transport_code: meta[0], status_code: meta[1] };
}
