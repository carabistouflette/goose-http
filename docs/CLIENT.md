# Reusable HTTPS GET clients

An opt-in client amortizes connection/TLS setup across sequential buffered GETs
to one configured origin. Existing one-shot `request` and streaming calls keep
their independent handles. No existing consumer/dependency pin is advanced.

```goose
var client = http::ClientHandle {};
let opened = http::open_client("https://example.com", client);
if opened.error == http::Error.None {
    defer { assert(http::close_client(client).error == http::Error.None); }
    let options = http::Options { body_limit: 65536, header_limit: 16384 };
    var body: u8[>..] = [];
    var headers: u8[>..] = [];
    let first = http::request(client, "https://example.com/first", options, body, headers);
    let second = http::request(client, "https://example.com/second", options, body, headers);
}
```

See the buildable [CLI example](../examples/client.goose). It makes network calls
only when explicitly invoked with an origin and request URLs.

## Origin and method policy

`open_client(origin, client)` is setup-only: it validates and copies the parsed
origin without connecting, resolving DNS or verifying a server certificate.
Origin syntax is lower-case `https://` plus authority, optionally a final `/`.
Paths, queries, fragments, userinfo (even empty), and IPv6 zone IDs are rejected.
Ports must be 1..65535; absent HTTPS ports mean 443. The URL/input byte bound is
8192. Parsed hosts must contain nonempty printable ASCII bytes.

Every request must have the same libcurl-parsed host (ASCII case-insensitive)
and numeric effective port as the configured origin. Only HTTPS is supported,
even if an `Options` value has `allow_http=true`. Origin mismatches, fragments,
zone IDs, unsupported methods and all existing invalid input are rejected before
transfer. Numeric host spellings follow libcurl's parser; this is not raw URL
equality, DNS alias equivalence or DNS/address authorization. Apply a separate
caller URL policy first where necessary. Accepted request URL bytes are passed
unchanged to libcurl; the adapter does not construct or resolve relative URLs.

Reusable clients accept GET only, with no upload body. POST remains available
through the existing one-shot API. There is no application-level retry loop;
libcurl can transparently reconnect/replay a GET when a reused connection fails.
GET must be safe to repeat under the application's protocol. A transfer failure
does not establish whether the server saw a request. No replay guarantee is made
for the new client, and it must not be used as an exactly-once transaction API.

## Lifetime, isolation and limits

Use one sequential logical owner per client. Different clients can make requests
concurrently. A process has at most 32 client slots, independent of the 32 stream
slots. Opening into a nonzero public handle rejects without consuming it.
Capacity exhaustion returns `SetupFailed` with a zero fresh handle.

`ClientHandle.id` is an opaque positive generational token. Do not manufacture
tokens or treat them as security capabilities. Copies alias one resource and do
not extend its lifetime. Close the owning handle exactly once; other copies then
become stale. Generations retire before wrap and are separate from stream IDs.

`close_client(client)` consumes the token on success and clears its ID. Closing
zero is idempotent. Invalid/stale or concurrently busy operations return
`InvalidInput`; failed close preserves the caller's ID so a busy live client can
be closed after its request completes. Discard stale copies rather than retrying
them. The registry holds no global lock across network or backend cleanup work.

The client uses one private libcurl easy handle, HTTP/1.1, and a connection-cache
limit of one. Each request revalidates inputs and reapplies options. Before
returning, it resets options and detaches all borrowed header/body/callback
pointers. Authorization/custom headers never carry over to the next request.
Cookies, shared handles, HSTS and Alt-Svc storage are never enabled. Environment
proxies, netrc, HTTP authentication negotiation and redirects remain disabled;
peer/hostname verification and existing body/header/time limits remain enforced.

One client belongs to one caller authentication/security context. Clearing an
Authorization header does not erase server-side connection identity, so do not
share a client across principals or tenants. Reuse intentionally retains private
connection, DNS and TLS-session caches. TLS verification occurs when establishing
a connection, not again for every GET over an established TLS channel. Close the
client when its trust/security context changes. libcurl/server policy can close
a connection at any time; reuse is an optimization, not a guaranteed outcome.

Setup, transport, timeout and callback-limit failures discard the retained easy
handle/cache. The next valid request creates a fresh one while keeping the bound
origin. Invalid input preserves an otherwise good idle cache. Complete HTTP
3xx/4xx/5xx responses can reuse a connection and retain `Error.None`; callers
still interpret the status code. Outputs are empty on every error, as with
one-shot requests. Bodies/headers remain bounded caller-owned buffers, and no
borrowed input survives a call. Cache count is bounded; system/TLS backend memory
is not a fixed process-RSS ceiling.

There is no background worker, asynchronous cancellation or streaming client
reuse in this API. It is not signal-handler-safe or asynchronous thread-
cancellation-safe. Callers must close resources on ordinary return; process
termination does not guarantee cleanup. Existing DNS/cleanup and filesystem
deadline caveats remain: transfer timeouts are not hard total wall deadlines.

## Validation and measurement

`make clients CC=gcc` and `CC=clang` exercise native lifetime/capacity/origin,
request isolation, real verified TLS, default ports, failures, and public Goose
bindings. `make clients-sanitize SANITIZER_CC=gcc` and `clang` instrument generated
Goose/runtime/native code; installed libcurl/TLS libraries are not instrumented.
`make test` and `make sanitize` include these checks. Bounded fuzzing also drives
two-client/stale-token/callback sequences without networking.

`make reuse-bench CC=gcc` records rotated samples of one-shot native calls,
the new native client, and direct libcurl cold/reset prototypes over the same
fixed valid GET workload. It checks exact payload/header bytes, HTTP/1.1, peer
verification, libcurl new-connection counts, and independent fixture TCP/request
counts. The fixture uses an ephemeral CA and TCP_NODELAY; production offers no
custom-CA/insecure switch. The first connection and open/close work are included
in each native-client batch. Direct prototypes do not implement every library
input/error contract and are not general feature-equivalent competitors.

`make reuse-sanitize SANITIZER_CC=clang` collects correctness only, without timing
fields. Release and sanitizer records/commands have separate paths. Raw monotonic
elapsed samples retain source, binary, compiler, flags, backend and host metadata.
Shared-host loopback measurements are workload-specific; they do not establish
public Goose API end-to-end latency, WAN throughput or a universal ranking.

### Recorded reuse run

On 2026-11-05, clean source commit `8b17458e367c657e5e902d8f5b24529b4d8641d8`
was measured with GCC 16.2.1 and Clang 23.1.4, both using libcurl 8.23.0. Each
compiler ran seven rotated 30-request samples for 0-byte and 4096-byte bodies.
The actual native client measurement includes `open_client`, its first verified
TLS connection, all requests, and `close_client`.

| Compiler | Body bytes | One-shot native | Native client | Median paired speedup |
| --- | ---: | ---: | ---: | ---: |
| GCC | 0 | 4,718.9 µs/request | 383.8 µs/request | 12.39× |
| GCC | 4,096 | 4,737.1 µs/request | 404.1 µs/request | 11.68× |
| Clang | 0 | 4,755.8 µs/request | 373.4 µs/request | 12.72× |
| Clang | 4,096 | 4,792.5 µs/request | 402.8 µs/request | 11.89× |

These are one nonexclusive Linux x86-64 run on an Intel Core Ultra 7 258V,
affined to CPU 3. The fixture checked every response byte, TLS verification,
HTTP/1.1, and connection count; for 30-request measured batches, cold native
requests opened 30 TCP connections and the reusable client opened one. The
results describe this loopback workload only. The compressed raw GCC and Clang
reports retain sample timings, counters, build commands and hashes; only local
workspace and temporary fixture paths are redacted:

- [GCC report](../bench/results/reuse-gcc-8b17458-2026-11-05.json.gz), SHA-256 `20523b61cbc1eb2cfade372570b302decaccdc56fd51872204d026623a653dda`
- [Clang report](../bench/results/reuse-clang-8b17458-2026-11-05.json.gz), SHA-256 `18e8a6b84e8ef7f5584a5fa17bf655299f418a674442bbac496c31f778658cea`
