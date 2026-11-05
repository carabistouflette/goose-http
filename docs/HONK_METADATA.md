# Source metadata example

`examples/honk_metadata.goose` is a reference component for a source-revision lookup. It uses one fixed GitHub endpoint and returns JSON metadata. It is not an installer or proof that an executable is authentic.

## Request and response

`fetch(revision)` accepts a full 40-character lowercase hexadecimal commit ID. It makes one HTTPS GET to:

`https://api.github.com/repos/aardappel/goose/commits/<revision>`

The request uses User-Agent `honk-toolchain-metadata/0.1`, Accept `application/vnd.github+json` and Accept-Encoding `identity`. TLS verification stays enabled. The example sends no credentials and disables proxies, redirects and retries.

The response must be HTTP 200. Body and callback-header limits are 256 KiB and 32 KiB; the transfer timeout is 5 seconds. This timeout is not a hard wall-clock deadline because system DNS cleanup can continue after it expires. Response decoding is disabled. If libcurl reports nonidentity content encoding, duplicate encoding fields or folded header lines, validation fails.

The validator checks raw and decoded UTF-8, JSON syntax, nesting up to 32 containers and numeric lexemes up to 128 bytes. Unknown fields are allowed. The `sha`, `html_url`, `commit` and `commit.message` values must be present once with the required types. The returned SHA must equal the requested revision. The URL must be nonempty. The subject is the first message line.

Returned fields own their bytes. `render(info)` escapes values as JSON strings. Treat the URL and subject as untrusted text. The URL is not authorized for navigation. Metadata does not authenticate an executable or approve an installation.

## Build and checks

The example imports this HTTP package, Goose's `std` and `json`, and goose-text at `46059e30be79ece6efd4317eb0adc6d213c808ab`. The compiler supplies `std` and `json`. See [third-party notices](../THIRD_PARTY_NOTICES.md).

The native HTTP package needs a C11 compiler, POSIX threads, pkg-config and libcurl >=7.85 with TLS, asynchronous DNS and thread-safe initialization. Goose setup needs Git, Python 3, CMake >=3.27 and C++20. On Debian or Ubuntu, use `build-essential cmake pkg-config libcurl4-openssl-dev` plus the Goose build tools.

After `make setup`, run:

```sh
make metadata-test
make metadata-sanitize SANITIZER_CC=gcc
make metadata-sanitize SANITIZER_CC=clang
```

These commands use fixtures and loopback TLS. A first build may download pinned dependencies. The live example is explicitly opt-in:

```sh
.toolchain/goose/build/goose run examples/toolchain-metadata.goose -- eadca409f68210893f366e60d0c2f0dc29a339be
```

No normal test or example command contacts GitHub. See [test coverage](../tests/README.md) and [historical metadata measurements](../bench/METADATA_RESULTS.md).
