# Contributing

Keep the API small, explicit and bounded. Goose owns validation and returned
storage; a private libcurl adapter owns short-lived native handles. Preserve
TLS verification and the documented ownership/limit/error contracts. No bespoke
cryptography or HTTP parser. No speculative abstractions or automatic retries.

Use focused branches and Conventional Commits with imperative subjects:
`feat: add ...`, `fix: reject ...`, `test: cover ...`, `docs: clarify ...`,
`chore: update ...`. Each commit contains one coherent change. No stable API or
release promise before 1.0. Do not commit generated output or credentials.

Behavior changes require meaningful public API regression tests, including
malformed input and ownership. Run `make format`, `make check test`, and
`git diff --check`. Native changes require `make sanitize SANITIZER_CC=gcc` and
`make sanitize SANITIZER_CC=clang`, plus the bounded fuzz harness. Request
independent correctness, security and performance reviews and fix findings.
Keep CI passing. Report vulnerabilities privately through the process in
[SECURITY.md](SECURITY.md). Compiler changes require a reviewed full upstream commit and
complete validation. Never import private workspace notes into published code.

Profile before optimizing. Preserve validation. Benchmarks retain raw samples,
compiler revision, hardware, flags and source revision. Timing is observational;
CI checks execution, not noisy timing thresholds. Do not claim universal rankings.
