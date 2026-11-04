# Changelog

## Unreleased

- Add deterministic differential metadata validation campaigns with a Python JSON
  oracle, owned-result checks, fixed resource/Unicode/duplicate boundaries and
  GCC/Clang sanitizer coverage. No runtime or example behavior change.
- Add a separate Debian 12/libcurl 7.88 native HTTP/1 and HTTP/2 compatibility job,
  alongside Ubuntu's libcurl 8.5 full suite. Distro security updates remain enabled.

## 0.1.0 — 2026-11-04 (experimental prerelease)

Initial public release of the bounded synchronous Goose HTTP client, Apache-2.0.
The API remains experimental with no compatibility promise before 1.0.
Linux x86-64 is validated; macOS and Windows are unsupported in this release.

### Included

- GET and binary POST, explicit headers, owned response buffers, configurable
  body/header limits and transfer timeout. HTTPS certificate/hostname verification
  uses system libcurl trust. Cleartext HTTP requires explicit opt-in.
- No automatic proxies, redirects, retries, netrc, cookie store, content
  decompression, file writes, pooling or streaming. Accept-Encoding permits only
  the explicit value `identity` (ASCII case-insensitive).
- Fixed-origin source-metadata reference consumer with bounded JSON/Unicode
  validation, duplicate/type checks, exact requested SHA equality and escaped
  output. It is an application example, not a new core API or installer.

### Validation and consumer evidence

Runtime, native build and example files are unchanged from reviewed source
[`c879c6fec88eaf7229f890a5dd6e3fc313261eab`](https://github.com/carabistouflette/goose-http/commit/c879c6fec88eaf7229f890a5dd6e3fc313261eab),
whose [CI passed](https://github.com/carabistouflette/goose-http/actions/runs/41838671886).
Coverage includes interpreted/release/debug builds, ownership rejection, HTTP/1
and real HTTP/2 TLS, startup/allocation/runtime fault injection, incomplete headers,
GCC/Clang ASan+UBSan and bounded native fuzzing. Local validation included 100000
fuzz runs and 57 compiled metadata HTTPS cases in release and both sanitizer modes.
Two independent final component reviews found no outstanding blocker.

The real Honk adapter at
[`91f66d4c188012104b65a42b697a79f636721b18`](https://github.com/carabistouflette/honk/commit/91f66d4c188012104b65a42b697a79f636721b18)
passed [exact-commit CI](https://github.com/carabistouflette/honk/actions/runs/41853137174).
Combined process/HTTP/opt-in diagnostics at
[`453aa53dc7b20622782423c64cc180097f653650`](https://github.com/carabistouflette/honk/commit/453aa53dc7b20622782423c64cc180097f653650)
passed [combined CI](https://github.com/carabistouflette/honk/actions/runs/41859461760)
and the Honk owner's fresh public-clone checks, tests, security, frozen self-check,
packaging and source-archive inspection. Honk's caller review found no outstanding
issue; an explicit unshimmed request returned the exact compiler SHA with cache
untouched and an unusable compiler override. Its ordinary commands stay local;
the network operation is opt-in. `--log-file` adds only explicitly requested
application diagnostics. Honk retains the immutable `c879c6` dependency pin.

Reproducible measurements retain raw samples, source hashes, toolchain and timing
boundaries: [request results](bench/RESULTS.md), [CPU/stack profile](bench/PROFILE.md),
and [metadata workload](bench/METADATA_RESULTS.md). Shared-host observations are
not universal speed or throughput claims. See [development commands](README.md#development)
and [metadata handoff/build requirements](docs/HONK_METADATA.md).

### Limits and integration requirements

Use Goose `eadca409f68210893f366e60d0c2f0dc29a339be` and a maintained system libcurl
development package >=7.85 with TLS, asynchronous DNS and thread-safe initialization;
native requirements are described in the README. System TLS/libcurl dependencies
are not instrumented by the sanitizer tests or bundled in this source release.

- Transfer timeout is not a hard wall deadline: threaded DNS cleanup can wait
  longer. Applications own signal policy, destination authorization and credentials.
- Callback headers can be normalized or incomplete on some libcurl backends,
  including omitted known-length HTTP/2 trailers. Verify every required field;
  transport success is not integrity or authenticity proof.
- Input/output caps are not an exact allocator/TLS memory budget. Response buffers
  are cleared on reported error; returned bytes remain application input to validate.
- Source metadata does not authenticate executable artifacts or authorize installs.
  Passing tests and reviews do not prove absence of bugs.

See [API](docs/API.md), [security policy](SECURITY.md) and
[third-party notices](THIRD_PARTY_NOTICES.md) before integration.
