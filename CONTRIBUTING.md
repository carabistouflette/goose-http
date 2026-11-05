# Contributing

Keep the API small, synchronous and bounded. Goose validates inputs and owns returned data; the native adapter uses system libcurl. Preserve TLS verification, explicit resource limits, ownership and error behavior. Do not add a custom TLS implementation, HTTP parser or automatic retry policy.

Use a focused branch and an imperative Conventional Commit subject, such as `fix: reject malformed headers` or `docs: clarify timeout behavior`. Keep each commit to one coherent change. Do not commit generated output, credentials or private planning.

For behavior changes, add regression tests for invalid input, bounds and ownership. Run `make format`, `make check test` and `git diff --check`. Native changes also need GCC and Clang sanitizer runs and bounded fuzzing. Request independent correctness and security review for code changes; fix concrete findings before merge.

Keep benchmark evidence tied to exact source, compiler, flags, workload and raw samples. Do not turn shared-host measurements into universal rankings. Report vulnerabilities through [SECURITY.md](SECURITY.md).
