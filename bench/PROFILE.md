# Native CPU profiles

Run `make profile` to compare the working native adapter with correctness commit
`9a8bed6`. The default run records five paired samples of 20,000 setup calls per
workload and 2,000,000 callback copies per workload in `.build/profile-results.json`.
No Goose compilation, server, network connection or credentials are needed.

The equivalent command, with explicit controls, is:

```sh
python3 bench/profile.py --baseline 9a8bed6 --iterations 20000 --samples 5 --cpu 1
```

The baseline commit must be available in local Git history. Pass another commit
with `--baseline`; `--baseline HEAD` also supports an execution smoke check in a
shallow checkout. Use `--cc clang` to select another compiler and `--output PATH`
to retain a separate result. The harness requires Linux CPU affinity and a
compiler supporting `-fstack-usage`, in addition to the native library dependencies.
It defaults to CPU 1 when allowed, otherwise an allowed CPU other than 0 when
possible. An explicit `--cpu` must be in the process's allowed affinity set.
Run profiles serially because they share `.build/profile-*` artifacts.

## Measurement boundaries

The setup workloads make calls to the real private adapter with zero, one, or
64 ordinary request headers, plus a workload with 64 whitespace-only values.
They include validation, URL parsing, libcurl easy-handle creation, header-list
allocation, option configuration, response-code retrieval and cleanup. The linker
intercepts only `curl_easy_perform`, returning `CURLE_RECV_ERROR` without DNS,
sockets, TLS or HTTP parsing. Every call must report a transport error and zero
returned body/header lengths. The harness does not fabricate successful HTTP
headers. Output capacities are 4096 body bytes and 1024 header bytes, without
per-request prezeroing.

The callback workloads invoke the selected implementation's real private body
callback through a volatile function pointer. Each iteration resets its bounded
buffer and copies either 4096 or 16384 bytes. Allocation and initialization of
source/destination storage are outside the timed interval. These are individual
copies into reused storage; they do not model transfer scheduling, HTTP parsing
or complete response processing. The harness includes each selected source
snapshot to access its private callback and buffer type without adding production
exports.

Each measured child process performs 1000 excluded warmup iterations, then reads
monotonic wall and process-CPU clocks around its loop. Process startup and global
libcurl initialization are excluded. Assertions, checksum accumulation and loop
control are included. Setup header formatting occurs once per timed loop rather
than per request. Baseline/current execution order alternates between paired
samples; all raw samples are retained. CPU and wall durations help distinguish
preemption from work done, but neither removes clock, frequency, cache or system
load variation.

The two source snapshots use identical compiler flags and dependencies. A
separate production-only compilation records the static `gh_request` frame with
`-fstack-usage`. That frame excludes its callees, libcurl, runtime stacks and the
benchmark driver; it is not the total stack required for a request. Reports are
compiler/build specific.

The JSON contains full baseline/current Git revisions, dirty state, SHA-256
hashes of both native sources/headers and the harness, compiler/version/flags,
libcurl build/runtime versions, platform and CPU metadata, affinity, stack
reports, execution order, raw CPU/wall durations and per-workload medians. Native
snapshots are frozen before compilation, so hashes identify the measured code
even when the current tree is uncommitted. Private source/home paths are
normalized in metadata. Retain this provenance with any published samples.

This harness does not measure Goose preparation, successful HTTP requests, DNS,
HTTPS, connection establishment, network throughput or application latency. The
loopback HTTP benchmark in [README.md](README.md) measures a different boundary.
Use representative application profiles before making broader performance
claims. Timing is observational; CI checks execution rather than thresholds.
