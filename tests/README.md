# Test coverage

`make check test` runs the Goose checks, interpreted/release/debug tests, ownership checks and native integration suite. Tests use loopback services; normal checks do not contact public servers.

`make sanitize SANITIZER_CC=gcc` and the Clang equivalent instrument generated Goose/runtime code and the native adapter. Installed libcurl and TLS libraries are not instrumented.

## Fuzzing

`make fuzz FUZZ_RUNS=10000` runs the native offline libFuzzer harness with AddressSanitizer and UndefinedBehaviorSanitizer. `make metadata-fuzz` compares the metadata validator with an independent Python JSON oracle. `make metadata-coverage-fuzz` uses a separate coverage-guided harness for the metadata validator.

These are bounded test runs, not proofs of correctness or exhaustive security checks. Metadata and native fuzz targets retain their seeds, source hashes and failure inputs in ignored `.build/`. Runs in one checkout should be serial because they share build and corpus paths.

## HTTP/2 and TLS

`make http2` requires libcurl built with HTTP/2 and Python `h2` 4.1 or later. It tests loopback TLS, binary requests, response limits, informational headers, trailers, resets and timeouts. `make http2-sanitize SANITIZER_CC=gcc` and the Clang equivalent run the same suite under sanitizers.

The fixture uses an ephemeral certificate trusted only by the test executable. It does not change system trust or use real credentials. Some libcurl versions omit HTTP/2 trailers on known-length responses; applications must check every required field. The suite does not cover HTTP/3, public services, every TLS/resolver backend or unsupported platforms.

## Source metadata example

`make metadata-test` runs unit and loopback HTTPS cases for the fixed-origin example, including TLS trust, response bounds, encoding, JSON fields and timeout checks. It does not contact GitHub. `make metadata-sanitize SANITIZER_CC=gcc` and Clang instrument the generated program and native adapter. The example's live CLI command is separate and explicit; see [its contract](../docs/HONK_METADATA.md).
