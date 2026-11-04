# HTTP client contract

Experimental, synchronous, bounded GET/POST client. Linux/POSIX initially; Goose
compiler pinned by GOOSE_REVISION, libcurl >= 7.85 with TLS, asynchronous DNS and thread-safe initialization.
Applications own destinations, credentials and response interpretation.

## API

`request(url, options, body, headers)` (pass a named `Options` variable by const reference) replaces caller-owned growable byte buffers
and returns `Status { error, transport_code, status_code }`. `Options` holds method
(GET or POST), request headers (`Header { name, value }` views), request body view,
`timeout_ms` (default 30000), `body_limit` (default 1 MiB), `header_limit` (default
64 KiB), and `allow_http` (default false). No borrowed input survives the call. Outputs must be distinct, non-overlapping
buffers. Input references cannot borrow either output (compiler enforced).
Output aliasing is rejected as InvalidInput with empty output.
GET rejects a nonempty request body. POST supports binary/empty bodies.
`success(status)` means transport success AND HTTP 200..299. HTTP 4xx/5xx are valid
responses with Error.None. Status code may be available on transfer failure.

Hard bounds: URL 8192 bytes, 64 request headers, 64 KiB aggregate request header
bytes (name + value + 4 each), 1 MiB upload, 16 MiB response body, 256 KiB response
headers, timeout 1..86400000 milliseconds. Zero output limits are supported.
Validation rejects control/space/non-ASCII URL bytes, embedded credentials,
schemes other than lowercase `https://` (or opted-in `http://`), empty host, URL fragments, non-token header names and header
values outside printable ASCII plus horizontal tab. HTTP requires opt-in.
Application headers cannot override Host, Content-Length, Transfer-Encoding,
Connection, Proxy-Connection, TE, Trailer, Upgrade or Expect. Accept-Encoding is
accepted only when its field value is exactly `identity` (ASCII case-insensitive;
no whitespace, lists or parameters in the public Header value). This does not
enable automatic response decompression.
Empty/whitespace-only request header values send an empty field (libcurl's
semicolon syntax internally), rather than suppressing that header.
No automatic decompression. Transfer-decoded body bytes (chunk framing removed)
and bytes delivered by libcurl's header callback (including informational
responses and trailers) are returned. The library adds no header normalization
or merging. These are not guaranteed to be raw wire bytes: libcurl may unfold
HTTP/1 headers or render HTTP/2 headers as text. Delivery also depends on the
installed backend: libcurl 8.5 can omit HTTP/2 trailers when Content-Length is
present, even though the body completed. The adapter cannot restore a callback
that libcurl never issued. Applications must explicitly require any critical
trailer/header (for example, a digest); transport success does not establish
its presence or verify application payload integrity. Output buffers are empty on ANY error;
partial network responses must not be consumed as complete data. A successful
transfer also requires the final response header section to be complete, even
when libcurl returns success at EOF before that section terminates.

TLS certificate/hostname verification stays enabled. No insecure switch. Default
system trust store; no custom CA option in v1. No automatic redirects (return 3xx
and Location for caller policy). No environment proxy, netrc, cookies, HTTP auth
negotiation or connection reuse across calls. Explicit Authorization is supported
but never forwarded to another origin automatically. No library output/logging.
This is a network client, not an SSRF boundary: applications must authorize URLs,
hostnames and resolved addresses when input is untrusted. Local/private addresses
are permitted. Caller may intentionally re-request a redirect under its policy.

Native adapter uses libcurl, a once-only process-lifetime global initialization,
per-call easy handle and direct copies into prebounded Goose-owned buffers.
No native pointers escape. Buffers do not grow during native calls. No retries,
async API, pooling, or connection reuse across calls. In addition to buffered `request`,
a pull-based streaming transfer API (`open_stream`, `read_chunk`, `close_stream`) provides
bounded chunk delivery directly to caller-owned buffers.

## Streaming API Contract

### Overview & Functions
The streaming client enables incremental, pull-based consumption of HTTP/HTTPS response bodies:
- `open_stream(url: const u8[:], options: const StreamOptions&, handle: StreamHandle&) -> Status`:
  Setup-only; executes zero network I/O or blocking waits. Validates inputs, claims an active slot
  in the 32-slot generational registry, configures libcurl easy and multi handles, and populates `handle.id`.
- `read_chunk(handle: StreamHandle&, chunk_out: u8[>..]&, headers_out: u8[>..]&, step_timeout_ms: i32) -> StreamStep, Status`:
  Drives libcurl multi transfers, header collection, and chunk draining. Returns `StreamStep.Chunk`
  (positive chunk length <= `chunk_limit`), `StreamStep.IdleTimeout` (step timeout elapsed with no data),
  `StreamStep.Done` (normal EOF completion), `StreamStep.Error` (terminal failure), or `StreamStep.Cancelled`.
  Output buffers are completely cleared and empty on any non-Chunk step.
- `close_stream(handle: StreamHandle&) -> Status`:
  Aborts transfer if active, removes and cleans up libcurl easy/multi handles, releases internal ring and header
  buffers, and resets `handle.id = 0`. Idempotent for zero handle IDs. Returns cleanup status; failures report
  the underlying `CURLMcode` (libcurl multi error code) in `transport_code`.

### Registry & Generational Retirement
- Maximum 32 concurrent active streams (`GH_MAX_STREAMS`).
- Registry tracks slots through 4 states: `GH_SLOT_FREE`, `GH_SLOT_RESERVED`, `GH_SLOT_ACTIVE`, `GH_SLOT_CLOSING`.
- Generational IDs combine a 48-bit monotonically bumped generation and 16-bit slot index (`(gen << 16) | (slot + 1)`).
- When a slot's generation reaches `GH_MAX_GENERATION` (`0x7FFFFFFFFFFFLL`), it is permanently retired as a sentinel.
  Generations never wrap or reuse past IDs; all stale or invalid IDs are rejected with `Error.InvalidInput`.

### Flow Control, Ring Buffer & Protocol Enforcement
- Native backpressure is maintained using an internal ring buffer with capacity `max(2 * chunk_limit, 128 KiB)`.
  When the ring buffer is full, libcurl transfer is synchronously paused (`CURL_WRITEFUNC_PAUSE`). Once at least
  16 KiB is consumed via `read_chunk`, the transfer is resumed (`curl_easy_pause(CURLPAUSE_CONT)`).
- Forced HTTP/1.1 (`CURL_HTTP_VERSION_1_1`) and `Accept-Encoding: identity` with content decoding disabled:
  avoids HTTP/2 and HTTP/3 multiplexing pause-window bloat (up to 10 MiB+) and prevents transparent chunk decompression caching.
- `handle.total_bytes` accurately maintains cumulative delivered body bytes after each successful chunk copy.
- Control flow is strictly pull-based: libcurl callbacks execute synchronously within native C; no Goose interpreter
  callbacks or closures cross the FFI boundary.

### Timeouts & Wall-Clock Deadlines
- `options.timeout_ms` bounds total transfer time budget across the stream.
- `step_timeout_ms` bounds maximum wait time for a single `read_chunk` step before returning `StreamStep.IdleTimeout`.
- Neither represents a universal hard wall-clock cancellation bound: threaded resolver cleanup (`getaddrinfo`)
  and libcurl socket cleanup may block beyond the deadline on slow systems. Real-time deadlines require external process supervision.
No TLS or HTTP parser is implemented here. Transport codes are libcurl CURLcode;
errors distinguish InvalidInput, SetupFailed, Transport, TimedOut, BodyLimit,
HeaderLimit. `timeout_ms` sets libcurl's transfer timeout, including resolution,
connection, TLS and transfer work inside `curl_easy_perform`. It does **not** bound
`request` wall time: with libcurl's threaded resolver, cleanup joins an outstanding
system `getaddrinfo` call, which may block beyond the timeout. `ASYNCHDNS` alone
does not imply cancellable DNS. A c-ares resolver build avoids this specific
thread-join limitation; no build promises hard real-time scheduling. Applications
requiring a strict wall-clock deadline need a cancellation-capable transport or
an external process supervisor. The adapter performs normal cleanup rather than
leaking outstanding resolver work.

The adapter does not install signal handlers. With NOSIGNAL, libcurl documents
that applications may need to handle SIGPIPE for some TLS backends; see
https://curl.se/libcurl/c/threadsafe.html. Programs must establish their own
signal policy when required by their chosen libcurl/TLS build.
