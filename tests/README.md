# Test boundaries

## Metadata differential campaign

`make metadata-fuzz` compares the compiled example validator with an independent
Python JSON oracle. The default seed is 491: 10000 generated/mutated/raw byte cases
plus 77 fixed cases for Unicode, decoded duplicate keys, required types, malformed
numbers, nesting and exact/exceeded resource limits. Successful results are checked
field by field; rejected inputs must return empty fields. The driver clears its
parser input before rendering the owned result. No fetch or network request occurs.

`make metadata-fuzz-sanitize SANITIZER_CC=gcc` and the Clang equivalent instrument
the generated Goose/runtime C and native package using the existing content-keyed
test compiler wrapper. The transport shim is linked but never called by this driver;
the HTTPS fixture tests remain separate. CI runs all three modes. Override
`METADATA_FUZZ_CASES` (1..100000) and `METADATA_FUZZ_SEED` to extend a campaign.

The Python generator streams trusted test records into batches of at most 1000
cases and 32 MiB; every record has a little-endian u32 body length. The standalone
driver reads each whole file before checking the 32 MiB cap, so this framing is
not an untrusted-file API. It rejects truncated/oversized records. Each process
has a 120-second outer limit. This is deterministic mutation/property testing,
not coverage-guided fuzzing or a proof of correctness. Headers and transport use
fixed successful inputs here; the separate HTTPS matrix exercises those policies.

The oracle uses Python's JSON grammar, preserving decoded duplicate object keys;
it applies the application's UTF-8/depth/number/body limits and critical-member
policy independently. Unknown numeric values are ignored after lexeme checks,
so this does not verify floating-point conversion. Reports retain seed, counts,
corpus hash, binary hash, caller-supplied mode label and source hashes/dirty state
in separate `.build/metadata-fuzz-*-results.json` files for each Make target.
Old reports for that target are removed before a campaign starts. CI retains
these reports and corpus/reproducer files, including failed-run artifacts.
On a disagreement the harness saves a single framed reproducer at
`.build/metadata-fuzz-failure.bin`; run the compiled driver with that filename.
Crashes/timeouts preserve the batch as `.build/metadata-fuzz-crash.bin`.
Python optimization is rejected because oracle assertions must stay active.
Run campaigns serially per checkout: corpus and failure paths are shared.

## Coverage-guided metadata validator fuzzing

`make metadata-coverage-fuzz` builds an offline coverage-guided harness using Clang
libFuzzer with AddressSanitizer and UndefinedBehaviorSanitizer instrumentation.
The harness exports `fuzz_metadata_validate` from `tests/metadata_coverage.goose`
into standalone C, driven by `tests/metadata_coverage.c`.

Seed inputs are populated by `scripts/metadata-fuzz-seeds.py` into
`.build/metadata-coverage-corpus`, containing valid JSON documents, UTF-8/Unicode
escapes, surrogate edge cases, nested structures, boundary numbers, and raw HTTP
headers with `identity` and non-identity encoding variations.

Each fuzzer iteration exercises five distinct paths:
1. Direct body fuzzing with valid revision and valid HTTP/1.1 200 identity headers.
2. Direct header fuzzing with valid revision and valid JSON document body.
3. Dual split fuzzing: the first two bytes determine a partition point between headers and body.
4. Arbitrary revision validation with valid body and headers.
5. Status code variation.

On any rejection, the harness asserts that returned metadata fields remain empty.
On acceptance, it verifies SHA matching, valid UTF-8 across all fields, non-empty HTML URL,
and absence of carriage returns or newlines in commit subjects. Memory safety and leak
checks run under ASan and UBSan. Override `METADATA_COVERAGE_RUNS` to extend a campaign.

`make test` exercises Goose run/release/debug builds, owned outputs and compiler
lifetime rejection, plus native loopback HTTP/1/TLS, callback/option/allocation
faults and DNS cleanup. The runtime fault driver uses a fresh process per mode:
failed global initialization, missing required libcurl features, an old runtime,
and response-info failure after completed or timed-out transfers. No network is
opened by those injected failure cases. Their outer process timeout is 10 seconds.

`make http2` adds a real loopback TLS/HTTP2 server using Python hyper-h2 4.1+.
The native driver asserts ALPN-negotiated HTTP/2 using libcurl's reported protocol;
there is no HTTP/1 fallback or simulated successful transport. Python `h2` and a
libcurl build with HTTP/2 are required; absence fails this explicit target.
Install `python3-h2` on Debian/Ubuntu, or select an isolated installation with
`HTTP2_PYTHON=/path/to/venv/bin/python`.

The HTTP/2 cases cover binary GET and 1 MiB binary POST with real flow-control
updates, empty request fields, exact and exceeded response limits, empty bodies,
103 informational headers and trailers, compressed bytes without decompression,
stream reset, response timeout, and untrusted/trusted local TLS. Explicit dummy
Authorization is checked at the source. A separate origin detects redirected
requests; 302/401/407 response bytes and relevant headers must survive unchanged,
and auth challenges must not cause negotiation/retries. The fixture proves the
reset and timeout reach the HTTP/2 response phase. Header status reason spacing
is not assumed to be identical across libcurl versions.

An ephemeral localhost certificate is created for each run. Only the test
executable's linker wrapper sets its CA path; production certificate and hostname
verification remain enabled, and no system trust settings change. No real
credentials or external network are used. TLS sockets have a finite read/handshake
timeout, every handler is joined before fixture assertions, and the client process
has a 60-second outer limit. Test HTTP/2 parsing/flow control is delegated to h2.

`make http2-sanitize SANITIZER_CC=gcc` and the Clang equivalent instrument the
native driver and adapter with AddressSanitizer/UndefinedBehaviorSanitizer. This
does not instrument system libcurl, its TLS backend, or the Python protocol stack.
CI runs the normal HTTP/2 and both sanitizer targets on Ubuntu 24.04. Local
validation also ran hyper-h2 4.4.1 with libcurl 8.23.0 on Linux x86-64.

These checks do not prove absence of defects in dependency code. HTTP/3, live
Internet services, every resolver/TLS backend and unsupported platforms are not
covered. The threaded-DNS cleanup and SIGPIPE caveats in [the API contract](../docs/API.md)
remain application integration constraints. Performance measurements have their
own [boundaries](../bench/PROFILE.md); these protocol tests have no timing ranking.

The unsized HTTP/2 trailer case strictly requires the final trailer callback.
A separate known-length case reports callback delivery as a backend capability:
libcurl 8.5 can finish a Content-Length response without delivering its trailers,
whereas the locally tested 8.23.0 delivers them. It still must return the complete
body and informational/final headers. This probe does not silently turn a missing
critical field into application success; callers must validate required fields.
The fixture and assertions are tested against both upstream 8.5.0 and system
8.23.0, with Python hyper-h2 4.4.1. CI additionally exercises Ubuntu's packages.
The older-backend reproduction used curl's `curl-8_5_0` source tag at
`7161cb17c01dcff1dc5bf89a18437d9d729f1ecd` (its source version label is
`8.5.0-DEV`), built separately without changing the installed system library.

`make metadata-test` runs pure validator/ownership tests interpreted and compiled,
then 57 cases through the real compiled reference CLI and loopback HTTPS. These
exercise the fixed original GitHub URL, GET/headers, anonymous proxy-free requests,
exact size boundaries, pin/SHA checks, malformed/ambiguous JSON and Unicode,
content encoding, non-200/redirect, TLS trust/hostname and transfer timeout.
Missing fixture configuration exits before a request; the fixture asserts one
request per valid-pin HTTP case and none for invalid pins or failed TLS.
`metadata-sanitize` with GCC and Clang instruments generated Goose/runtime C,
the native adapter and test shim. The build wrapper's content fingerprint isolates
the native compiler cache by mode/compiler/input. CI runs both passes plus a
benchmark execution smoke test. No GitHub request is part of these targets.
See [the consumer handoff contract](../docs/HONK_METADATA.md) for build bindings,
dependency pins and the explicit live command.

## libcurl compatibility coverage

The full Ubuntu 24.04 workflow exercises its maintained distribution libcurl
8.5 backend, including the Goose API and compiled metadata consumer. A separate
lightweight [compatibility workflow](../.github/workflows/compatibility.yml)
uses official Debian 12 with its maintained libcurl 7.88 backend for the native
HTTP/1, TLS, runtime/fault and real HTTP/2 suites. That job checks the expected
upstream version family and records exact installed package revisions; it does
not build Goose or claim that the compiler is validated on Debian 12.

The Debian base image is pinned by digest. Apt packages deliberately receive
the distribution's current security updates, so the package versions are logged
rather than frozen to obsolete builds. The official image is described at
[Docker Hub](https://hub.docker.com/_/debian/), and the Debian package source is
[libcurl4-openssl-dev in bookworm](https://packages.debian.org/bookworm/libcurl4-openssl-dev).
This matrix does not cover every supported TLS/resolver configuration or establish
support for untested platforms. The API's libcurl feature checks remain required.

Local native/HTTP2 reruns also passed with system libcurl 8.23.0 and the separately
built upstream 8.5 source revision recorded above, using the same current tests.
To select an independently prepared libcurl without modifying system packages,
pass explicit compiler/linker flags (including a runtime search path) to Make:

```sh
make native http2 CURL_FLAGS="-I/absolute/curl/include -L/absolute/curl/lib -Wl,-rpath,/absolute/curl/lib -lcurl"
```

The HTTP/2 output records the runtime version and negotiated protocol. Inspect it
to confirm that the selected backend was actually loaded; header version alone
is insufficient. These commands use loopback only. Old unpatched upstream builds
are test reproductions, not recommended deployment dependencies.

The strict HTTP/2 reset case sends headers, partial DATA and a PING, then waits
for its matching acknowledgement before resetting the stream. This separates
the reset from the initial input without sleeps; the client must report the
received HTTP 200 and negotiated HTTP/2 along with transport failure and empty
outputs. A separate immediate-reset probe sends HEADERS/DATA/RST together and
records available metadata. Older backends can discard that buffered input
before exposing status/version, so this probe permits unknown values while still
requiring failure/empty outputs. The server independently verifies ALPN h2,
both requests, both real resets and the acknowledgement. libcurl documents
unknown values for [response code](https://curl.se/libcurl/c/CURLINFO_RESPONSE_CODE.html)
and [HTTP version](https://curl.se/libcurl/c/CURLINFO_HTTP_VERSION.html).
