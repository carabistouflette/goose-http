# Test boundaries

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
