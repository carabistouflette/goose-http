# Opt-in source metadata reference consumer

[`honk_metadata.goose`](../examples/honk_metadata.goose) is an application example,
not a new HTTP API or an installer. It is independent of Honk modules. Honk owns
its command dispatch, manifest/default compiler pin selection, vendoring, output
error handling and tests. The intended command is opt-in `honk toolchain --metadata`,
dispatched before compiler/cache work, with no Git, compiler, bootstrap or cache
mutation. Ordinary commands must not trigger a metadata request.

## Contract

`fetch(revision)` accepts exactly 40 lowercase hexadecimal bytes. It makes one
GET to `https://api.github.com/repos/aardappel/goose/commits/<revision>` with
User-Agent `honk-toolchain-metadata/0.1`, Accept `application/vnd.github+json` and
Accept-Encoding `identity`. It uses system TLS trust with certificate and hostname
verification, no credentials, proxies, netrc, redirects or retries. Response body
and callback-header limits are 256 KiB and 32 KiB; transfer timeout is 5000 ms.
Threaded DNS cleanup can exceed this timeout: it is **not a hard wall deadline**.
Pooling and streaming remain deferred.

Only HTTP 200 is accepted. Any observed nonidentity Content-Encoding, duplicate
encoding or folded callback line fails closed. Header callback bytes can already
be normalized by libcurl, and some backends omit known-length HTTP/2 trailers;
the example cannot validate wire bytes it never receives. Content decoding is
disabled. An unsolicited compressed body must still pass the JSON validation.

Validation checks raw and decoded UTF-8, JSON grammar, a 32-container nesting
limit and 128-byte numeric lexeme limit before accepting required fields. Unknown
fields are allowed. The decoded keys `sha`, `html_url`, `commit` and
`commit.message` must each occur exactly once in their respective object, with
the required object/string types. The full `sha` must equal the requested pin;
`html_url` must be nonempty. Subject is the first line of the message, stopping
at CR or LF. All four returned `Info` fields own their bytes, including on return
from the parser; failures return empty fields and an error without echoing bodies.

`validate(revision, status, body, headers)` exposes the same validation for tests.
`render(info)` produces escaped JSON keys `requested_revision`, `sha`, `html_url`
and `subject`. Treat values as untrusted text: the URL is not validated for
navigation, and decoded control characters must never be printed raw to a terminal.
Metadata is **not executable authentication, artifact verification or install/trust
proof**. Honk must check output writes through its own adapter.

## Vendoring and build prerequisites

Vendor the reviewed `examples/honk_metadata.goose` with its SPDX header and this
repository's Apache-2.0 `LICENSE`; record the full Git revision and SHA-256 of the
copied source. The module imports `http`, `text`, `std` and `json` without package
mappings. Bind `http` and `text` in the application's root source. The executable
example shows those bindings; its text pin is
`46059e30be79ece6efd4317eb0adc6d213c808ab`. `std`/`json` come from the pinned Goose
compiler (`GOOSE_REVISION`). No fixture wrapper belongs in the production build.

The HTTP package needs `http.goose`, `http.build`, root `CMakeLists.txt`,
`native/http.c` and `native/http.h`. Preserve the generated fingerprint in
`http.build`; `make format` refreshes it after native edits. Native prerequisites
are a C11 compiler, CMake, pkg-config, POSIX threads, and libcurl development
headers/library >=7.85 with TLS, asynchronous DNS and thread-safe initialization.
On Debian/Ubuntu install `build-essential cmake pkg-config libcurl4-openssl-dev`;
the compiler setup additionally needs Git, Python 3, CMake 3.27+ and C++20.
Linux x86-64 is the validated platform. See the [API contract](API.md),
[requirements](../README.md#requirements) and [third-party notices](../THIRD_PARTY_NOTICES.md).

From a standalone clone, after `make setup`:

```sh
make metadata-test
make metadata-sanitize SANITIZER_CC=gcc
make metadata-sanitize SANITIZER_CC=clang
make metadata-bench
# Explicit live smoke only: this command contacts GitHub.
.toolchain/goose/build/goose run examples/toolchain-metadata.goose -- eadca409f68210893f366e60d0c2f0dc29a339be
```

Pass `GOOSE=/absolute/path/to/pinned/goose` to reuse a compiler. These Make targets
use only loopback HTTPS and pure fixtures; fetching compiler/package dependencies
on a first build can require network access. The test-only GNU linker wrappers
verify the original fixed endpoint and timeout before routing to an ephemeral
localhost TLS server. They retain certificate/hostname verification and fail
before transport when fixture environment is absent. Sanitizer builds instrument
the generated Goose/runtime C, native kernel and shim, not installed libcurl/TLS.
The content-fingerprinted compiler wrapper prevents stale native cache reuse.

The fixture matrix covers success, exact/exceeded caps, invalid pin, wrong SHA,
non-200/redirect, raw/escaped Unicode, critical duplicates/types, depth/number
bounds, content encoding, TLS trust/hostname, hostile proxy environment,
request policy and delayed-response timeout. Benchmark boundaries and recorded
results are in [the benchmark documentation](../bench/README.md).
