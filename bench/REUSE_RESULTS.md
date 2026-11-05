# Reusable-client results

These results measure the reusable HTTPS GET client on current main. They do not apply to release `v0.1.0`, which predates that API.

## Workload

The clean run was recorded on 2026-11-05 at `8b17458e367c657e5e902d8f5b24529b4d8641d8` with GCC 16.2.1, Clang 23.1.4, libcurl 8.23.0 and OpenSSL 3.6.5 on Linux 7.2.11 Arch x86-64 with glibc 2.44, Intel Core Ultra 7 258V. The process was affined to CPU 3 on a shared host.

Release binaries used `-std=c11 -Wall -Wextra -Werror -pthread -O2` and system `-lcurl`. Each compiler ran seven rotated measured batches of 30 requests for 0-byte and 4096-byte response bodies. Three warmup requests per mode were excluded. Each batch included its first connection. The local TLS fixture gave each mode the same URL, identity header, timeout, bounded callbacks, peer/hostname verification and HTTP/1.1 response check. It verified every response and counted requests and TCP connections. The client batch included `open_client`, the first TLS connection, all requests and `close_client`. Sanitizer builds used separate `-O1 -g -fsanitize=address,undefined` flags and produced correctness records only.

Times include request setup, transfer, response checks and cleanup. They are monotonic elapsed microseconds per request, calculated from batch duration. Values show median and min–max across seven samples; the JSON files retain every raw sample and counter.

| Compiler | Body | One-shot API | Reusable API | Paired one-shot/client ratio |
| --- | ---: | ---: | ---: | ---: |
| GCC | 0 B | 4,718.9 (4,676.5–4,844.1) | 383.8 (368.3–394.8) | 12.39 (11.98–13.27) |
| GCC | 4 KiB | 4,737.1 (4,718.7–4,816.1) | 404.1 (401.5–436.3) | 11.68 (11.04–12.70) |
| Clang | 0 B | 4,755.8 (4,726.7–4,861.8) | 373.4 (368.5–398.7) | 12.72 (12.19–13.14) |
| Clang | 4 KiB | 4,792.5 (4,723.6–4,852.4) | 402.8 (392.9–418.0) | 11.89 (11.44–12.16) |

The fixture counted 30 new TCP connections for each one-shot batch and one for each reusable-client batch. This result describes repeated requests to one local test server. It does not predict WAN throughput, application latency or behavior under concurrent requests.

The run also measured direct libcurl prototypes. They bypass some library validation and error handling, so they are not equivalent APIs. They use libcurl defaults for protocol selection; the fixture still verified HTTP/1.1 responses. Their median and sample range, in microseconds per request:

| Compiler | Body | Direct cold | Direct reused |
| --- | ---: | ---: | ---: |
| GCC | 0 B | 4,690.8 (4,667.3–4,823.4) | 364.7 (358.6–385.4) |
| GCC | 4 KiB | 4,709.9 (4,671.2–4,819.6) | 389.8 (378.8–406.8) |
| Clang | 0 B | 4,738.9 (4,686.6–4,763.4) | 377.8 (354.1–391.0) |
| Clang | 4 KiB | 4,771.9 (4,670.3–4,874.4) | 397.9 (374.5–407.7) |

## Source match and raw reports

The measured clean commit `8b17458e367c657e5e902d8f5b24529b4d8641d8` and published main `f81d12197c26f48ab61562f056aba0a1a19f0ff6` are on different history paths. I checked the six source files recorded by the benchmark against current main; each content SHA-256 matches:

| File | SHA-256 |
| --- | --- |
| `bench/reuse.c` | `0b4794393091c7a0743422c99a754aec74f95e528c888c8317bc368718843348` |
| `bench/reuse-measure.py` | `9936f4dbf2abdd293b70b1cbaf17d7b6cc2f0424b0252afeeb37d43b3b5b4d86` |
| `native/client.c` | `c269c8b625ec457bd2b5a6c8e44d547a97d6408abff9c808341886464ffea543` |
| `native/http.c` | `070c206c6823be0f1b4f851c997c19f8509b3adde78dba5c6407ef5725f5cdbe` |
| `native/http.h` | `86b4f714d7f9c54b93aa458f9a283556d1f2037ea351125d1c43698bd1e31d4c` |
| `native/internal.h` | `38d3c78aa9c805757356fa8a6e80e9916297d026a23167beba05653204f4b704` |

This is a source-file match for the measured paths, not a claim that the two complete trees are identical.

- [GCC report](results/reuse-gcc-8b17458-2026-11-05.json.gz), SHA-256 `20523b61cbc1eb2cfade372570b302decaccdc56fd51872204d026623a653dda`
- [Clang report](results/reuse-clang-8b17458-2026-11-05.json.gz), SHA-256 `18e8a6b84e8ef7f5584a5fa17bf655299f418a674442bbac496c31f778658cea`

Reproduce with `make reuse-bench CC=gcc` and `make reuse-bench CC=clang`. The release run first executes separate sanitizer correctness batches; sanitized timings are not included. See [benchmark scope](README.md).
