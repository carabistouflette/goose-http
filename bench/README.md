# HTTP benchmark scope

Run `make bench` for the one-shot loopback request harness. It uses a local Python server, five samples of 100 requests per workload and one excluded warmup batch. It makes no external network request. The native adapter uses fixed 4 KiB body and 1 KiB header buffers; the Goose API uses its 1 MiB and 64 KiB defaults. Those are different workloads, so they do not establish a ranking. The recorded sample is explicitly [historical](RESULTS.md).

Run `make reuse-bench CC=gcc` or `CC=clang` for repeated verified loopback HTTPS GETs through the one-shot API, reusable client and direct libcurl prototypes. It rotates modes and retains raw samples, counters, compiler commands and source hashes. The complete protocol and current source match are in [REUSE_RESULTS.md](REUSE_RESULTS.md). Direct prototypes bypass some library behavior and are not equivalent APIs.

Run `make profile` for the native setup/callback profile. That profile isolates local adapter calls and copies; it does not measure successful requests or end-to-end latency. The saved 2026-11-04 profile used an earlier source snapshot; see [the profile record](PROFILE.md).

The optional source-metadata consumer benchmark is a separate local TLS and validation workload. Its 2026-11-04 sample used an earlier implementation and is retained as history in [METADATA_RESULTS.md](METADATA_RESULTS.md).
