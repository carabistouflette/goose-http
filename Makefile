GOOSE ?= .toolchain/goose/build/goose
CC ?= gcc
SANITIZER_CC ?= gcc
FUZZ_CC ?= clang
FUZZ_RUNS ?= 10000
DNS_WRAPS = -Wl,--wrap=curl_easy_perform -Wl,--wrap=curl_easy_cleanup
BENCH_ITERATIONS ?= 100
CFLAGS = -std=c11 -Wall -Wextra -Werror
CURL_FLAGS = $(shell pkg-config --cflags --libs libcurl)
FAIL_WRAPS = -Wl,--wrap=curl_easy_getinfo -Wl,--wrap=malloc -Wl,--wrap=curl_easy_init -Wl,--wrap=curl_url -Wl,--wrap=curl_url_set -Wl,--wrap=curl_url_get -Wl,--wrap=curl_slist_append -Wl,--wrap=curl_easy_setopt -Wl,--wrap=curl_easy_perform
.NOTPARALLEL:
.PHONY: setup format check test native sanitize fuzz bench
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
	$(CC) $(CFLAGS) -O2 -pthread tests/native.c native/http.c $(CURL_FLAGS) -o .build/native
	python3 tests/integration.py .build/native
	$(CC) $(CFLAGS) -O2 -pthread tests/failures.c native/http.c $(CURL_FLAGS) $(FAIL_WRAPS) -o .build/failures
	./.build/failures
	$(CC) $(CFLAGS) -O2 -pthread tests/tls.c native/http.c $(CURL_FLAGS) -Wl,--wrap=curl_easy_perform -o .build/tls
	python3 tests/integration.py --with-ca .build/tls
	$(CC) $(CFLAGS) -O2 -pthread tests/dns.c native/http.c $(CURL_FLAGS) $(DNS_WRAPS) -o .build/dns
	python3 scripts/run.py .build/dns
sanitize:
	mkdir -p .build
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread tests/native.c native/http.c $(CURL_FLAGS) -o .build/native-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/integration.py .build/native-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread tests/failures.c native/http.c $(CURL_FLAGS) $(FAIL_WRAPS) -o .build/failures-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./.build/failures-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread tests/tls.c native/http.c $(CURL_FLAGS) -Wl,--wrap=curl_easy_perform -o .build/tls-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/integration.py --with-ca .build/tls-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread tests/dns.c native/http.c $(CURL_FLAGS) $(DNS_WRAPS) -o .build/dns-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/dns-sanitize
fuzz:
	mkdir -p .build/fuzz-corpus
	python3 scripts/fuzz-seeds.py
	$(FUZZ_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -pthread tests/fuzz.c native/http.c $(CURL_FLAGS) -Wl,--wrap=curl_easy_getinfo -Wl,--wrap=curl_easy_setopt -Wl,--wrap=curl_easy_perform -o .build/fuzz
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/fuzz -runs=$(FUZZ_RUNS) -seed=491 -max_len=65536 -timeout=5 -artifact_prefix=.build/ .build/fuzz-corpus
bench:
	mkdir -p .build
	$(CC) $(CFLAGS) -O2 -pthread bench/client.c native/http.c $(CURL_FLAGS) -o .build/bench
	"$(GOOSE)" build bench/bench.goose --cc "$(CC)" -O2 -o .build/bench-goose
	python3 bench/measure.py --binary .build/bench --goose-binary .build/bench-goose --goose "$(abspath $(GOOSE))" --iterations $(BENCH_ITERATIONS) --cc $(CC)
