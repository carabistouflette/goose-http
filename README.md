# goose-http

A bounded HTTP client for [Goose](https://github.com/aardappel/goose). It uses system libcurl and is licensed under Apache-2.0. The validated platform is Linux x86-64.

## Quick start

From a clone:

```sh
git clone https://github.com/carabistouflette/goose-http.git
cd goose-http
make setup
.toolchain/goose/build/goose run examples/request.goose -- https://example.com/
```

The example sends one GET and prints the response body. It uses the checked-out library. To pin the current published main revision in another Goose program, use:

```goose
import http from "carabistouflette/goose-http@f81d12197c26f48ab61562f056aba0a1a19f0ff6";
```

See [examples/request.goose](examples/request.goose) and the [API contract](docs/API.md).

## What it provides

- Synchronous GET and binary POST requests with caller-owned output buffers.
- Response body and header limits, transfer timeouts, explicit headers and verified TLS.
- An optional origin-bound HTTPS GET client for repeated requests.
- An optional pull-based API for reading large response bodies in bounded chunks.

`Error.None` means the transfer completed; check `success(status)` to require HTTP 2xx. The library does not follow redirects, retry at the application level, use environment proxies, or decode compressed responses. HTTPS is required by default. Set `allow_http` only when cleartext HTTP is intended.

The library is not an SSRF boundary. Applications must authorize untrusted destinations and decide whether to follow redirects. A transfer timeout is not a hard wall-clock deadline: system DNS cleanup can continue after it expires. Response-header callback bytes may be normalized by libcurl, and some older libcurl versions omit HTTP/2 trailers. Require and verify any security-critical response field yourself.

## Versions

Published main is `f81d12197c26f48ab61562f056aba0a1a19f0ff6`. Release `v0.1.0` is `bb30d4c893efbf41fc7306d541c332074aa9ea35`; it contains the one-shot API. The reusable client and streaming APIs are on main after that tag. The API is experimental before 1.0.

## Build and checks

The native adapter needs CMake, a C11 compiler, POSIX threads and system libcurl >=7.85 with TLS, asynchronous DNS and thread-safe initialization. Building Goose also needs CMake >=3.27 and a C++20 compiler. On Debian or Ubuntu, start with `build-essential cmake git python3 pkg-config libcurl4-openssl-dev openssl`.

`make check test` runs the local checks and tests. `make clients CC=gcc` and `make clients CC=clang` check the reusable client; `make http2` runs optional HTTP/2 coverage and needs libcurl built with HTTP/2 plus Python `h2`. See [test coverage](tests/README.md), [security policy](SECURITY.md) and [third-party notices](THIRD_PARTY_NOTICES.md).

For origin-bound reuse, read [the client contract](docs/CLIENT.md). For streaming, see [the API contract](docs/API.md). Performance methods and recorded samples are in [bench/README.md](bench/README.md), [bench/RESULTS.md](bench/RESULTS.md), and [bench/REUSE_RESULTS.md](bench/REUSE_RESULTS.md).
