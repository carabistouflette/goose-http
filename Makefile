GOOSE ?= .toolchain/goose/build/goose
CC ?= gcc
SANITIZER_CC ?= gcc
FUZZ_CC ?= clang
FUZZ_RUNS ?= 10000
HTTP2_PYTHON ?= python3
METADATA_REQUESTS ?= 10
METADATA_ITERATIONS ?= 2000
METADATA_SAMPLES ?= 5
METADATA_CPU ?=
METADATA_FUZZ_CASES ?= 10000
METADATA_FUZZ_SEED ?= 491
METADATA_COVERAGE_RUNS ?= 10000
RUNTIME_WRAPS = -Wl,--wrap=curl_global_init -Wl,--wrap=curl_version_info -Wl,--wrap=curl_easy_setopt -Wl,--wrap=curl_easy_perform -Wl,--wrap=curl_easy_getinfo
DNS_WRAPS = -Wl,--wrap=curl_easy_perform -Wl,--wrap=curl_easy_cleanup
BENCH_ITERATIONS ?= 100
PROFILE_BASELINE ?= 9a8bed6
PROFILE_ITERATIONS ?= 20000
PROFILE_SAMPLES ?= 5
CFLAGS = -std=c11 -Wall -Wextra -Werror
CURL_FLAGS = $(shell pkg-config --cflags --libs libcurl)
FAIL_WRAPS = -Wl,--wrap=curl_easy_getinfo -Wl,--wrap=malloc -Wl,--wrap=curl_easy_init -Wl,--wrap=curl_url -Wl,--wrap=curl_url_set -Wl,--wrap=curl_url_get -Wl,--wrap=curl_slist_append -Wl,--wrap=curl_easy_setopt -Wl,--wrap=curl_easy_perform
NATIVE_SOURCES = native/http.c native/stream.c
.NOTPARALLEL:
.PHONY: setup format check test native sanitize fuzz bench profile http2 http2-sanitize metadata-build metadata-test metadata-sanitize metadata-bench metadata-fuzz metadata-fuzz-sanitize metadata-coverage-fuzz
setup:
	./scripts/setup-goose.sh
format:
	python3 scripts/native-manifest.py
	"$(GOOSE)" format http.goose tests examples bench
check:
	python3 scripts/native-manifest.py --check
	"$(GOOSE)" format --check http.goose tests examples bench
	"$(GOOSE)" check tests/api.goose
	"$(GOOSE)" check examples/request.goose
	"$(GOOSE)" check examples/toolchain-metadata.goose
	"$(GOOSE)" check tests/metadata.goose
	"$(GOOSE)" check tests/metadata_fuzz.goose
	"$(GOOSE)" check tests/metadata_coverage.goose
	"$(GOOSE)" check bench/metadata.goose
	"$(GOOSE)" check bench/bench.goose
	python3 -m py_compile scripts/*.py tests/*.py bench/*.py
test:
	mkdir -p .build
	python3 scripts/native-manifest.py --check
	python3 tests/integration.py "$(abspath $(GOOSE))" run tests/api.goose --
	"$(GOOSE)" build tests/api.goose --cc "$(CC)" -o .build/api
	python3 tests/integration.py .build/api
	"$(GOOSE)" build tests/api.goose --debug --cc "$(CC)" -o .build/api-debug
	python3 tests/integration.py .build/api-debug
	python3 tests/ownership.py "$(abspath $(GOOSE))"
	$(MAKE) native
native:
	mkdir -p .build
	$(CC) $(CFLAGS) -O2 -pthread tests/native.c $(NATIVE_SOURCES) $(CURL_FLAGS) -o .build/native
	python3 tests/integration.py .build/native
	$(CC) $(CFLAGS) -O2 -pthread tests/failures.c $(NATIVE_SOURCES) $(CURL_FLAGS) $(FAIL_WRAPS) -o .build/failures
	./.build/failures
	$(CC) $(CFLAGS) -O2 -pthread tests/tls.c $(NATIVE_SOURCES) $(CURL_FLAGS) -Wl,--wrap=curl_easy_perform -o .build/tls
	python3 tests/integration.py --with-ca .build/tls
	$(CC) $(CFLAGS) -O2 -pthread tests/dns.c $(NATIVE_SOURCES) $(CURL_FLAGS) $(DNS_WRAPS) -o .build/dns
	python3 scripts/run.py .build/dns
	$(CC) $(CFLAGS) -O2 -pthread tests/runtime.c $(NATIVE_SOURCES) $(CURL_FLAGS) $(RUNTIME_WRAPS) -o .build/runtime
	python3 tests/runtime.py .build/runtime
	$(CC) $(CFLAGS) -O2 -pthread -DGH_MAX_GENERATION=3 tests/stream_retire.c $(NATIVE_SOURCES) $(CURL_FLAGS) -o .build/stream_retire
	./.build/stream_retire
sanitize:
	mkdir -p .build
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread tests/native.c $(NATIVE_SOURCES) $(CURL_FLAGS) -o .build/native-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/integration.py .build/native-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread tests/failures.c $(NATIVE_SOURCES) $(CURL_FLAGS) $(FAIL_WRAPS) -o .build/failures-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./.build/failures-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread tests/tls.c $(NATIVE_SOURCES) $(CURL_FLAGS) -Wl,--wrap=curl_easy_perform -o .build/tls-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/integration.py --with-ca .build/tls-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread tests/dns.c $(NATIVE_SOURCES) $(CURL_FLAGS) $(DNS_WRAPS) -o .build/dns-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/dns-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread tests/runtime.c $(NATIVE_SOURCES) $(CURL_FLAGS) $(RUNTIME_WRAPS) -o .build/runtime-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/runtime.py .build/runtime-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread -DGH_MAX_GENERATION=3 tests/stream_retire.c $(NATIVE_SOURCES) $(CURL_FLAGS) -o .build/stream_retire-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./.build/stream_retire-sanitize
fuzz:
	mkdir -p .build/fuzz-corpus
	python3 scripts/fuzz-seeds.py
	$(FUZZ_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -pthread tests/fuzz.c $(NATIVE_SOURCES) $(CURL_FLAGS) -Wl,--wrap=curl_easy_getinfo -Wl,--wrap=curl_easy_setopt -Wl,--wrap=curl_easy_perform -o .build/fuzz
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/fuzz -runs=$(FUZZ_RUNS) -seed=491 -max_len=65536 -timeout=5 -artifact_prefix=.build/ .build/fuzz-corpus
bench:
	mkdir -p .build
	$(CC) $(CFLAGS) -O2 -pthread bench/client.c $(NATIVE_SOURCES) $(CURL_FLAGS) -o .build/bench
	"$(GOOSE)" build bench/bench.goose --cc "$(CC)" -O2 -o .build/bench-goose
	python3 bench/measure.py --binary .build/bench --goose-binary .build/bench-goose --goose "$(abspath $(GOOSE))" --iterations $(BENCH_ITERATIONS) --cc $(CC)

profile:
	python3 bench/profile.py --baseline "$(PROFILE_BASELINE)" --iterations $(PROFILE_ITERATIONS) --samples $(PROFILE_SAMPLES) --cc "$(CC)"

http2:
	mkdir -p .build
	$(CC) $(CFLAGS) -O2 -pthread tests/http2.c $(NATIVE_SOURCES) $(CURL_FLAGS) -Wl,--wrap=curl_easy_perform -o .build/http2
	"$(HTTP2_PYTHON)" tests/http2.py .build/http2
http2-sanitize:
	mkdir -p .build
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread tests/http2.c $(NATIVE_SOURCES) $(CURL_FLAGS) -Wl,--wrap=curl_easy_perform -o .build/http2-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$(HTTP2_PYTHON)" tests/http2.py .build/http2-sanitize

metadata-build:
	python3 scripts/metadata-test-build.py --goose "$(abspath $(GOOSE))" --cc "$(CC)" --mode release --output .build/metadata-consumer
metadata-test: metadata-build
	"$(GOOSE)" run tests/metadata.goose
	"$(GOOSE)" build tests/metadata.goose --standalone -O2 -o .build/metadata-unit
	./.build/metadata-unit
	python3 tests/metadata.py .build/metadata-consumer
metadata-sanitize:
	python3 scripts/metadata-test-build.py --goose "$(abspath $(GOOSE))" --cc "$(SANITIZER_CC)" --mode sanitize --output .build/metadata-consumer-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/metadata.py .build/metadata-consumer-sanitize
metadata-bench: metadata-build
	"$(GOOSE)" build bench/metadata.goose --standalone --cc "$(CC)" -O2 -o .build/metadata-parse
	python3 bench/metadata-measure.py --goose "$(abspath $(GOOSE))" --cc "$(CC)" --requests $(METADATA_REQUESTS) --iterations $(METADATA_ITERATIONS) --samples $(METADATA_SAMPLES) $(if $(METADATA_CPU),--cpu $(METADATA_CPU),)

metadata-fuzz:
	"$(GOOSE)" build tests/metadata_fuzz.goose --standalone --cc "$(CC)" -O2 -o .build/metadata-fuzz
	python3 tests/metadata_fuzz.py .build/metadata-fuzz --cases $(METADATA_FUZZ_CASES) --seed $(METADATA_FUZZ_SEED) --label "$(CC)-release" --report .build/metadata-fuzz-release-results.json
metadata-fuzz-sanitize:
	python3 scripts/metadata-test-build.py --goose "$(abspath $(GOOSE))" --cc "$(SANITIZER_CC)" --mode sanitize --source tests/metadata_fuzz.goose --output .build/metadata-fuzz-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/metadata_fuzz.py .build/metadata-fuzz-sanitize --cases $(METADATA_FUZZ_CASES) --seed $(METADATA_FUZZ_SEED) --label "$(SANITIZER_CC)-asan-ubsan" --report .build/metadata-fuzz-$(notdir $(SANITIZER_CC))-sanitize-results.json

metadata-coverage-fuzz:
	mkdir -p .build/metadata-coverage-corpus
	python3 scripts/metadata-fuzz-seeds.py
	"$(GOOSE)" -o .build/metadata_coverage_export.c --header .build/metadata_coverage_export.h --standalone tests/metadata_coverage.goose
	$(FUZZ_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -c tests/metadata_coverage.c -o .build/metadata_coverage.o
	$(FUZZ_CC) -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -Wno-unterminated-string-initialization -Wno-align-mismatch -c .build/metadata_coverage_export.c -o .build/metadata_coverage_export.o
	$(FUZZ_CC) -fsanitize=fuzzer,address,undefined -pthread .build/metadata_coverage.o .build/metadata_coverage_export.o -lm -ldl -o .build/metadata-coverage-fuzz
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/metadata-coverage-fuzz -runs=$(METADATA_COVERAGE_RUNS) -seed=491 -max_len=65536 -timeout=5 -artifact_prefix=.build/ .build/metadata-coverage-corpus

