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
Connection, Proxy-Connection, TE, Trailer, Upgrade, Expect or Accept-Encoding.
Empty/whitespace-only request header values send an empty field (libcurl's
semicolon syntax internally), rather than suppressing that header.
No automatic decompression. Transfer-decoded body bytes (chunk framing removed) and raw response header blocks
(including informational responses and trailers) are returned. Headers are not
normalized, decoded or silently merged. Output buffers are empty on ANY error;
partial network responses must not be consumed as complete data.

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
async API, pooling, streaming sinks, file writes or registry policy in v1. Bounded
buffering is the first milestone; streaming should get a separate reviewed API.
No TLS or HTTP parser is implemented here. Transport codes are libcurl CURLcode;
errors distinguish InvalidInput, SetupFailed, Transport, TimedOut, BodyLimit,
HeaderLimit. Total timeout includes DNS with supported asynchronous resolver;
libcurl callbacks and OS scheduling mean it is not a hard real-time guarantee.

The adapter does not install signal handlers. With NOSIGNAL, libcurl documents
that applications may need to handle SIGPIPE for some TLS backends; see
https://curl.se/libcurl/c/threadsafe.html. Programs must establish their own
signal policy when required by their chosen libcurl/TLS build.
