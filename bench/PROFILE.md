# Native profile

`make profile` profiles the current native adapter against a selected baseline commit. The default run uses five paired samples of 20,000 setup calls or 2,000,000 callback copies per workload. It uses no server or network and requires Linux CPU affinity and a compiler supporting `-fstack-usage`.

Run it with explicit settings:

```sh
python3 bench/profile.py --baseline 9a8bed6 --iterations 20000 --samples 5 --cpu 1
```

The baseline must exist in local Git history. Set `--cc clang` for Clang and `--output PATH` to keep a separate report. The harness records source hashes, flags, libcurl versions, raw CPU/wall samples and stack reports. It uses linker wrappers to avoid DNS, sockets and TLS in setup calls. Callback tests copy fixed buffers into reused storage; they are not end-to-end response measurements.

## Historical sample

The raw [2026-11-04 report](results/2026-11-04-native-profile.json) compares baseline `9a8bed6a9017ee44c077da13f5edc1a9985ddb4f` with source `a87429314e9925637366b2d59a8e86341f1eff32`. It used GCC 16.2.1, libcurl 8.23.0 and an Intel Core Ultra 7 258V on Linux x86-64. That source's `native/http.c` and `native/http.h` differ from current main. Keep this report as historical; it is not performance evidence for current main.

The frame-size report covers only `gh_request` and excludes its callees, libcurl, runtime stacks and the driver. The harness does not measure successful HTTP requests, DNS, TLS, public API preparation, network throughput or application latency.
