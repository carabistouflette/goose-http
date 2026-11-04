# Source metadata consumer measurements

Recorded 2026-11-04 from clean source `c2310d8403d93b24b65eb45e9dae11e36183514d` on Linux x86-64,
Intel Core Ultra 7 258V, allowed CPU 2. This was a shared machine; affinity
was used, with no exclusive-isolation claim. Goose `eadca409f68210893f366e60d0c2f0dc29a339be`
(runtime `3025e5810379`), goose-text `46059e30be79ece6efd4317eb0adc6d213c808ab`,
GCC 16.2.1 20260810, `-O2 --standalone`, system libcurl 8.23.0.

The synthetic GitHub-shaped response is 7972 bytes, including ignored file/patch
fields. It uses the production 256 KiB body / 32 KiB callback-header limits and
5000 ms transfer timeout. Fixture SHA-256:
`3867e9d6c8009433271e66813192f2da75e0234e579f184fd6ceaf808b7fa9b6`.

| Workload | Samples × operations | Median per operation | Observed sample range per operation |
| --- | ---: | ---: | ---: |
| Fresh CLI + local TLS + validation/render | 5 × 10 | 19.422 ms | 18.528–22.562 ms |
| Validation + owned result + render | 5 × 2000 | 25.522 µs | 24.815–25.806 µs |

The first row includes process/runtime startup, buffer preparation, fixture-server
scheduling/work, transport, validation, escaping, output capture and Python
JSON decoding/assertions. The second row uses internal monotonic timing and
excludes file reading/process startup. Certificate/fixture setup, two TLS warmups
and 100 validation warmups are excluded. These are different measurement boundaries,
not competing implementations or an Internet throughput claim. No performance
threshold, speedup or universal ranking is inferred from these samples.

Reproduce from a standalone clone with the pinned compiler:

```sh
make metadata-bench CC=gcc METADATA_CPU=2
```

Choose an allowed CPU (or omit `METADATA_CPU` for automatic selection). Full
unaggregated samples, hashes of source/native build inputs and harness, compiler
identity and environment are in [metadata-results.json](metadata-results.json).
The [harness documentation](README.md#metadata-consumer-workload) defines boundaries.

An additional explicitly invoked, unshimmed live CLI smoke on 2026-11-04 returned
HTTP 200 for `eadca409f68210893f366e60d0c2f0dc29a339be`, with exact requested/returned
SHA equality, GitHub commit URL and subject. The complete process took about
0.575 seconds on that run; this one network observation is not a benchmark.
Normal commands/tests/benchmarks never request GitHub metadata.

Validation at this source included normal interpreted/release/debug tests,
metadata validator ownership tests, all 57 compiled HTTPS fixture cases in release,
GCC and Clang ASan+UBSan builds, real HTTP/2 TLS, and 100000 bounded fuzz runs.
Two independent final reviews found no remaining functional/security blocker;
one unsupported test-coverage claim and a benchmark checksum weakness were corrected.
System libcurl/TLS and Python are outside sanitizer instrumentation. These results
and reviews do not establish absence of bugs or authenticate executable artifacts.
