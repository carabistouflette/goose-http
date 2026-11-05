# goose-http

A small, bounded synchronous HTTP client for the
[Goose programming language](https://github.com/aardappel/goose). Apache-2.0.
Experimental 0.1.0 API; no compatibility promise before 1.0. Tested on Linux x86-64; POSIX native adapter.
macOS and Windows are not validated/supported in this initial version.

See [release notes and validation evidence](CHANGELOG.md) for the initial prerelease.

GET and binary POST, explicit request headers and transfer timeouts, bounded response
body/header buffers, and verified TLS through a private libcurl adapter. Goose
owns returned storage. Redirects are returned to the caller for an explicit
policy. HTTPS is the default; cleartext HTTP needs `allow_http: true`.

```goose
import http from "carabistouflette/goose-http@c2310d8403d93b24b65eb45e9dae11e36183514d";
import std;

fn main() {
    var body: u8[>..] = [];
    var headers: u8[>..] = [];
    let options = http::Options { .. };
    let status = http::request("https://example.com/", options, body, headers);
    if !http::success(status) {
        print("request failed: ", status.error, ", HTTP ", status.status_code);
        return;
    }
    print(body);
}
```

Declare the package in your program's root source file. This example pins a
reviewed commit; use `import http from "../goose-http";` for a neighboring local
checkout. Bind `Options` to a named variable before calling `request`.
See [the API contract](docs/API.md), [the example](examples/request.goose),
[security policy](SECURITY.md) and [third-party notices](THIRD_PARTY_NOTICES.md).

`Error.None` means the transfer completed, including HTTP 4xx/5xx responses.
`success` additionally requires HTTP 200..299. Both output buffers are empty on
any reported error. Response headers contain bytes delivered by libcurl's header
callback, including informational responses and trailers. The library adds no
normalization; raw wire bytes are not guaranteed. Body bytes have HTTP chunk
framing removed; content compression is not decoded. Request header values are ASCII, uploads and bodies are
arbitrary bytes. Buffers must be distinct; inputs cannot borrow outputs.
Older libcurl backends can omit HTTP/2 trailers on known-length bodies. Require
and validate any security-critical field explicitly; transfer success alone does
not verify a digest or other application metadata.

## Requirements

Pinned Goose compiler, CMake, a C11 compiler, POSIX threads, libcurl development
headers/library >= 7.85 with TLS, asynchronous DNS and thread-safe initialization.
Debian/Ubuntu: `build-essential cmake git python3 pkg-config libcurl4-openssl-dev
openssl`. Clang is also needed for the second sanitizer pass and fuzz target.
Goose setup needs CMake 3.27+ and a C++20 compiler. Native dependencies apply to
both `goose run` and building executables. Use a maintained system libcurl/TLS
package: the library does not vendor TLS or implement cryptography.

No application-level retry loop, automatic redirects, environment proxies,
netrc or cookie store. Response bodies may be fetched as a single bounded buffer via
`request` or consumed incrementally using the pull-based streaming transfer client.
URLs from untrusted input need application authorization; private network access is
permitted. Choose body/header limits for the expected payload: the public API initializes
those capacities before receiving data, so smaller limits reduce preparation work.

For repeated buffered HTTPS GETs, the optional [origin-bound client](docs/CLIENT.md)
reuses a private HTTP/1.1 connection/cache with explicit open/close. It requires
one sequential owner and one authentication context. libcurl may reconnect/replay
a GET after a stale reused connection; POST remains on the one-shot API. Existing
one-shot and streaming calls keep independent handles.

`timeout_ms` bounds libcurl's transfer work, but **not total call wall time**:
threaded DNS resolver cleanup can wait for a slow system resolver after timeout.
Applications requiring a strict deadline need a suitable resolver/transport or
process supervision. See the contract for the DNS cleanup and signal policy.

## Streaming Transfers

`open_stream`, `read_chunk`, and `close_stream` provide a bounded, pull-based client
for streaming large HTTPS response bodies chunk-by-chunk without whole-response memory
buffering (e.g. streaming chunks into incremental SHA-256 or disk spooling).

```goose
import http from "carabistouflette/goose-http";
import std;

fn main() {
    let options = http::StreamOptions {
        chunk_limit:  65536,
        body_limit:   67108864, // 64 MiB total cap
        header_limit: 65536,
    };
    var handle = http::StreamHandle {};
    let open_st = http::open_stream("https://example.com/stream", options, handle);
    if open_st.error != http::Error.None {
        print("open failed: ", open_st.error);
        return;
    }

    var chunk: u8[>..] = [];
    var headers: u8[>..] = [];
    while true {
        let step, st = http::read_chunk(handle, chunk, headers, 1000);
        if step == http::StreamStep.Chunk {
            // chunk is replaced with positive length <= options.chunk_limit
            // handle.total_bytes tracks cumulative delivered body bytes
            // handle.status_code reflects current HTTP response status
            continue;
        }
        if step == http::StreamStep.IdleTimeout {
            // Continuable: step timeout elapsed without data, transfer still active
            continue;
        }
        if step == http::StreamStep.Done {
            // Terminal normal completion: chunk is empty, st.error == None
            break;
        }
        // Terminal error or cancellation: chunk is empty
        print("streaming halted: step=", step, ", error=", st.error);
        break;
    }
    http::close_stream(handle);
}
```

### Streaming API Reference

| Operation / Type | Signature | Description |
|---|---|---|
| `open_stream` | `(url: const u8[:], options: const StreamOptions&, handle: StreamHandle&) -> Status` | Setup-only; performs no network I/O or blocking waits. Validates inputs, claims a native registry slot, initializes libcurl easy and multi handles, and sets `handle.id`. |
| `read_chunk` | `(handle: StreamHandle&, chunk_out: u8[>..]&, headers_out: u8[>..]&, step_timeout_ms: i32) -> StreamStep, Status` | Drives network transfers and header reading. Returns `Chunk`, `IdleTimeout`, `Done`, `Error`, or `Cancelled`. `chunk_out` is replaced each call and empty on non-Chunk steps. |
| `close_stream` | `(handle: StreamHandle&) -> Status` | Aborts active transfer, unregisters slot, frees native buffers and libcurl handles. Sets `handle.id = 0`. Idempotent when called with zeroed handle. Status reports cleanup result. |
| `StreamOptions` | `struct` | Transfer options: `timeout_ms` (overall transfer budget, default 30000ms), `connect_timeout_ms` (default 10000ms), `chunk_limit` (default 64 KiB, max 1 MiB), `body_limit` (default 64 MiB), `header_limit` (default 64 KiB, max 256 KiB), `headers` (`Header` list), `allow_http` (default false). |
| `StreamStep` | `enum` | Step outcome: `Chunk` (0), `IdleTimeout` (1), `Done` (2), `Error` (3), `Cancelled` (4). |
| `StreamHandle` | `struct` | Opaque handle state: `id: u64` (generational handle ID), `status_code: i64` (HTTP response code), `total_bytes: i64` (cumulative delivered body bytes). |

### Streaming Guarantees & Contract

- **Single Logical Reader**: A given `StreamHandle` must be driven by a single logical reader sequentially between steps. Concurrent operations across distinct handles are thread-safe.
- **Generational Slot Registry & Retirement**: Up to 32 concurrent active stream slots (`GH_MAX_STREAMS`). Handle IDs combine a 48-bit generation and 16-bit slot index (`(gen << 16) | (slot + 1)`). Generations are permanently retired at `GH_MAX_GENERATION` (`0x7FFFFFFFFFFFLL`). Exhausted slots are never re-allocated and IDs never wrap, preventing stale handle alias resurrection or use-after-free.
- **Bounded Ring Buffer & Backpressure**: Native backpressure is maintained using an internal ring buffer sized to `max(2 * chunk_limit, 128 KiB)`. When the ring buffer fills, libcurl's write callback pauses transfer (`CURL_WRITEFUNC_PAUSE`). Once at least 16 KiB is consumed via `read_chunk`, transfer is resumed (`curl_easy_pause(CURLPAUSE_CONT)`).
- **Forced HTTP/1.1 & Identity Encoding**: Streaming connections enforce `CURL_HTTP_VERSION_1_1` and `Accept-Encoding: identity` with automatic content decoding disabled (`CURLOPT_HTTP_CONTENT_DECODING = 0`). This prevents HTTP/2/3 flow-control window buffering (which can bloat pause buffers to 10 MiB+) and avoids transparent chunk decompression caching.
- **Empty Output on Terminal Steps**: `chunk_out` is cleared and replaced on every call; on `Done`, `IdleTimeout`, `Error`, or `Cancelled`, `chunk_out` is guaranteed to be empty.
- **Cumulative Delivered Bytes**: `handle.total_bytes` accurately tracks the cumulative sum of body bytes delivered to `chunk_out` after each chunk copy, with no 1-step lag.
- **No Native-to-Goose Callbacks**: Control flow is strictly pull-based. Libcurl callbacks execute synchronously inside native C; no Goose closures or callbacks cross the native boundary.
- **Cleanup Error Domain**: `close_stream` surfaces cleanup failures in `Status.transport_code` using the `CURLMcode` (libcurl multi error) domain.
- **No Universal Hard Wall-Clock Deadlines**: `timeout_ms` represents the transfer budget. Threaded resolver cleanup (`getaddrinfo`) and cleanup cancellation cannot be aborted instantaneously at OS level; strict wall-clock deadlines require external process supervision.

## Development

```sh
git clone https://github.com/carabistouflette/goose-http.git
cd goose-http
make setup
make format check test
make sanitize SANITIZER_CC=gcc
make sanitize SANITIZER_CC=clang
make fuzz FUZZ_RUNS=10000
make bench BENCH_ITERATIONS=100
```

For an existing pinned compiler, pass `GOOSE=/absolute/path/to/goose`. Run Make
serially per checkout because Goose shares native package cache/generated paths.
`make format` refreshes the native-source fingerprint used by that cache.
To run the example after setup:

```sh
.toolchain/goose/build/goose run examples/request.goose -- https://example.com/
```

Tests use ephemeral loopback HTTP/TLS servers and an outer process deadline;
no public service is needed. See [contribution rules](CONTRIBUTING.md) and
[benchmark scope](bench/README.md).

The [Honk source metadata reference consumer](docs/HONK_METADATA.md) demonstrates
a fixed-origin HTTPS request, bounded JSON validation and escaped output.
`make metadata-test` and both `metadata-sanitize` compiler passes exercise it
without contacting GitHub. Its live CLI is explicitly opt-in.

The dedicated TLS test uses a test-only linker wrapper to trust its ephemeral
certificate for successful TLS and hostname-mismatch cases. This injection adds
no public CA option and does not change production or system trust settings.

The optional real HTTP/2 suite requires libcurl built with HTTP/2 and Python
`h2` 4.1+ (`python3-h2` on Debian/Ubuntu). These are test-only requirements:

```sh
make http2
make http2-sanitize SANITIZER_CC=gcc
make http2-sanitize SANITIZER_CC=clang
```

Use `HTTP2_PYTHON=/path/to/venv/bin/python` for an isolated Python environment.
CI runs these targets explicitly. See [test coverage](tests/README.md) for the
TLS fixture, runtime fault cases and remaining protocol coverage limits.
