# goose-http

A small, bounded synchronous HTTP client for the
[Goose programming language](https://github.com/aardappel/goose). Apache-2.0.
Experimental API; no tagged release yet. Tested on Linux x86-64; POSIX native adapter.
macOS and Windows are not validated/supported in this initial version.

GET and binary POST, explicit request headers and deadlines, bounded response
body/header buffers, and verified TLS through a private libcurl adapter. Goose
owns returned storage. Redirects are returned to the caller for an explicit
policy. HTTPS is the default; cleartext HTTP needs `allow_http: true`.

```goose
import http from "carabistouflette/goose-http@4a916645145c16643762fe251bd11439bd62aff9";
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

## Requirements

Pinned Goose compiler, CMake, a C11 compiler, POSIX threads, libcurl development
headers/library >= 7.85 with TLS, asynchronous DNS and thread-safe initialization.
Debian/Ubuntu: `build-essential cmake git python3 pkg-config libcurl4-openssl-dev
openssl`. Clang is also needed for the second sanitizer pass and fuzz target.
Goose setup needs CMake 3.27+ and a C++20 compiler. Native dependencies apply to
both `goose run` and building executables. Use a maintained system libcurl/TLS
package: the library does not vendor TLS or implement cryptography.

No automatic redirects, retries, environment proxies, netrc, cookie store,
pooling, streaming sinks or file writes. Every call creates a fresh easy handle;
this favors a simple ownership contract over keep-alive throughput. URLs from
untrusted input need application authorization; private network access is
permitted. See the contract for bounds, DNS deadlines and signal policy.

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

The dedicated TLS test uses a test-only linker wrapper to trust its ephemeral
certificate for successful TLS and hostname-mismatch cases. This injection adds
no public CA option and does not change production or system trust settings.
