# Source metadata measurements

The pure validation result below applies to current main because its measured code is unchanged. The full HTTPS CLI result is kept as historical data; its native transport code differs from current main.

## Current validator workload

The clean run was recorded on 2026-11-04 from source `c2310d8403d93b24b65eb45e9dae11e36183514d`. The measured validator, driver and harness content match current main `f81d12197c26f48ab61562f056aba0a1a19f0ff6`: `examples/honk_metadata.goose`, `bench/metadata.goose`, `bench/metadata-measure.py` and the source-metadata fixture/helper used for this workload. The report retains the SHA-256 values. Goose `eadca409f68210893f366e60d0c2f0dc29a339be`, goose-text `46059e30be79ece6efd4317eb0adc6d213c808ab` and GCC 16.2.1 were used on Linux 7.2.11 Arch x86-64 with glibc 2.44, Intel Core Ultra 7 258V. The process ran on CPU 2 on a shared host.

The synthetic GitHub-shaped JSON body was 7,972 bytes. Each of five samples included 2,000 validation, owned-result and JSON-render operations after 100 warmups. Timing used the harness's monotonic clock and excluded file reading. The median was 25.522 µs per operation; sample range was 24.815–25.806 µs.

This measures metadata validation and rendering only; the input bytes are already in memory. It does not include HTTP transfer, DNS, TLS or public services.

## Historical full HTTPS CLI run

The same recording included five samples of ten fresh CLI requests over local TLS. The median was 19.422 ms per request; the range was 18.528–22.562 ms. This used the HTTP transport source at `c2310d8403d93b24b65eb45e9dae11e36183514d`, whose `http.goose`, native adapter and build files differ from current main. Do not treat this timing as current main performance. The raw report preserves the source hashes, fixture hash and sample durations.

The full CLI sample includes process/runtime startup, output buffers, TLS, fixture-server scheduling, transfer, validation, escaping and output decoding. It is not an Internet throughput or end-to-end application guarantee.

Reproduce current measurements with `make metadata-bench CC=gcc METADATA_CPU=2` after setup. This target measures both the validator and current local HTTPS fixture; it makes no live GitHub request. Choose an allowed CPU or omit `METADATA_CPU`. Full historical samples are in [metadata-results.json](metadata-results.json).
