# HTTP request benchmarks

`make bench` builds optimized native and Goose drivers and records five raw
samples per workload in `.build/bench.json`. Each sample performs 100 GET
requests for a 4096-byte response against the local Python fixture. Each driver
gets one excluded warm-up sample; measured samples alternate execution order.
Override `BENCH_ITERATIONS` for longer runs. No external network, credentials,
proxy or system trust modification is used. OpenSSL creates the fixture's unused
ephemeral TLS endpoint.

Two separately labeled workloads expose different layers of cost:

- `native_kernel` calls the private native adapter with a fixed 4096-byte body
  buffer and 1024-byte header buffer. These buffers are not prezeroed per request.
- `goose_public_defaults` calls the Goose public API with its default 1 MiB body
  and 64 KiB header limits. Buffer validation, packing, allocation and zero-fill
  are included; buffers are reused between requests and cleared by the API.

Results include compiler identity and flags, compiler executable/version, libcurl
version, platform, machine, CPU model/count, Goose pin, source revision and dirty
state. Both workloads include per-call easy-handle creation, native HTTP transfer,
fixture scheduling and process startup; neither isolates TLS, DNS or throughput.
Each request gets a fresh connection, as the public contract requires. Different
buffer contracts mean the samples are not competing implementations or universal
rankings. No performance threshold is implied. Profile representative application
workloads before adding pooling or changing ownership. CI checks execution only.

A [recorded local sample](RESULTS.md) includes the unaggregated durations and
full source/toolchain/environment provenance.

For native validation/setup and bounded-copy CPU costs without a network or
Python server, use the separate [CPU profile harness](PROFILE.md). It reports
compiler-specific stack frames and paired CPU/wall samples against a Git baseline.

## Metadata consumer workload

`make metadata-bench` records `.build/metadata-bench.json`: five samples of ten
fresh CLI processes fetching synthetic GitHub-shaped metadata over real loopback
TLS, plus five internally timed samples of 2000 validate-and-render operations.
Each TLS result and parser checksum is checked. Certificate/fixture creation,
two TLS warmups and 100 validation warmups are excluded. TLS durations include
process/runtime startup, buffer preparation, server scheduling/work, transport,
validation, escaping, capture and Python decoding/assertions. Validation durations
exclude file reading and process startup; they include owned result creation and
JSON rendering. These workloads measure different boundaries.

The test-linked shim redirects the fixed production URL only in fixture binaries;
production has no URL override or CA injection. Source hashes, dependency pins,
flags, fixture hash/size, raw samples and environment are recorded. CPU affinity
defaults to allowed CPU 2, otherwise the first allowed CPU; override
`METADATA_CPU`. This is a shared host, with no isolation or timing threshold claim.
Override `METADATA_REQUESTS`, `METADATA_ITERATIONS` and `METADATA_SAMPLES` for
longer or smoke runs. The explicit live CLI is not part of normal benchmark/test
targets. See [the consumer contract](../docs/HONK_METADATA.md).

[Recorded metadata results](METADATA_RESULTS.md) retain raw samples and full provenance.
