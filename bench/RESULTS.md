# Recorded local sample

[Raw samples and provenance](results/2026-11-04.json) were recorded on
2026-11-04 from clean source commit
`92b50c924d2f450f77b7493e173c00b027f4fbf7`, using the compiler pin in
`GOOSE_REVISION`, GCC 16.2.1, libcurl 8.23.0, and an Intel Core Ultra 7 258V
on Linux x86-64. Only executable path names were normalized for publication;
raw durations and compiler, system, flags, source and buffer metadata remain
unchanged. This is one machine and one fixture, not a general performance claim.

Each workload has five measured samples of 100 sequential requests after one
excluded warm-up sample, with execution order alternating between workloads.
Each request fetches 4096 bytes from the loopback Python server. The benchmark
includes process startup, native handle creation and server/thread scheduling.
It does not isolate adapter CPU cost, HTTPS, DNS, real networks or keep-alive.

The private native-kernel workload has a median sample duration of 117.089 ms
(100 requests), using fixed 4096-byte body/1024-byte header buffers without
per-call zero-fill. The Goose public-defaults workload has a median sample
duration of 165.770 ms (100 requests), including validation and the default
1 MiB body/64 KiB header buffer preparation. The scopes differ; these are not
competing implementations or evidence of a universal ranking. Raw samples show
substantial scheduling variation, especially for the public workload.

Reproduce with `make bench BENCH_ITERATIONS=100`; see [the harness scope](README.md).
Longer, application-specific profiles should guide any change to connection
reuse, buffer limits or ownership. CI only checks that the harness executes.
