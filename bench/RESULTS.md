# Historical one-shot request sample

This sample was recorded on 2026-11-04 from clean source `92b50c924d2f450f77b7493e173c00b027f4fbf7` with Goose `eadca409f68210893f366e60d0c2f0dc29a339be`, GCC 16.2.1 and libcurl 8.23.0 on Linux 7.2.11 x86-64, Intel Core Ultra 7 258V. It is retained as a historical sample, not current performance evidence for main `f81d12197c26f48ab61562f056aba0a1a19f0ff6`.

Each workload ran five measured batches of 100 sequential requests after one excluded warmup batch. The loopback Python server returned 4096 bytes per request. Process startup, native handle setup and fixture scheduling are included. No HTTPS, DNS or external network was measured.

| Workload | Median per 100 requests | Sample range |
| --- | ---: | ---: |
| Native adapter with fixed 4 KiB body / 1 KiB header buffers | 117.089 ms | 115.384–120.591 ms |
| Goose public API with default 1 MiB body / 64 KiB header buffers | 165.770 ms | 115.461–168.650 ms |

The workloads use different buffer contracts and are not competing implementations. The second workload varied widely; its raw five samples remain in [the report](results/2026-11-04.json). No ranking follows from this small run.

Reproduce the historical workload with `make bench BENCH_ITERATIONS=100`. Current main's reusable-client measurements are in [REUSE_RESULTS.md](REUSE_RESULTS.md).
