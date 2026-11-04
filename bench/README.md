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
