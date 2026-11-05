#!/usr/bin/env python3
"""Measure repeated verified loopback HTTPS GETs before changing the public API.

Usage: python3 bench/reuse-measure.py [--cc gcc|clang] [--requests 30]
       python3 bench/reuse-measure.py --cc clang --sanitize

Matrix: bodies 0 and 4096 bytes; native gh_request cold, matched direct libcurl
cold, private easy_reset reuse, and the native reusable GET client; seven
rotated samples of thirty requests.
The first connection is included in every batch. Separate warmup batches are
excluded. Each release invocation first runs ASan/UBSan correctness batches.
--sanitize runs correctness only and emits no transfer or batch timings.

The direct adapters cover this fixed valid GET workload, not all gh_request
input validation, error precedence, or public API semantics. They use the same
URL/path, identity request header, timeout, bounded body/header callbacks, peer
and hostname verification, no redirect/proxy/netrc/credentials/cookies, and an
observed HTTP/1.1 response. The retained easy handle is reset and fully configured
for every request; only libcurl's connection/TLS/DNS caches survive. This is an
upper-bound prototype for connection reuse. The native_client mode measures the
actual origin-bound gh_client_open/request/close API, including its production
validation, HTTP/1.1 selection and one-entry connection-cache bound. Direct
adapters retain native one-shot libcurl HTTP-version defaults; server ALPN and
the response oracle require observed HTTP/1.1 for every mode.

Batch monotonic time includes setup, transfer, cleanup, guards and response
validation. Per-transfer time includes request setup/transfer/cleanup (the reused
handle/client is finally destroyed at batch end; client open is included in its
first transfer). No performance threshold is enforced.
This is a nonexclusive host; raw samples and counters are retained for review.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
from datetime import datetime, timezone
import hashlib
import http.server
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import socket
import socketserver
import ssl
import statistics
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parent.parent
MODES = ("native_cold", "direct_cold", "direct_reuse", "native_client")
REUSED_MODES = ("direct_reuse", "native_client")
BODIES = (0, 4096)
PAYLOADS = {count: bytes((index * 131 + 17) % 256 for index in range(count)) for count in BODIES}
BASE_FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread"]
SOURCES = ("bench/reuse.c", "bench/reuse-measure.py", "native/http.c", "native/http.h",
           "native/client.c", "native/internal.h")


def require(condition, detail="benchmark validation failed"):
    if not condition:
        raise RuntimeError(str(detail))


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def run(command, timeout=120, env=None):
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                            timeout=timeout, env=env, check=False)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {shlex.join(command)}\n"
                           f"{result.stdout}\n{result.stderr}")
    return result.stdout.strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{value:016x}"


def cpu_model():
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor()


class Fixture(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    block_on_close = False
    request_queue_size = 8

    def __init__(self, context, budget):
        self.context = context
        self.budget = budget
        self.lock = threading.Lock()
        self.accepted = 0
        self.requests = 0
        self.errors = []
        self.responses = {count: (f"HTTP/1.1 200 OK\r\nContent-Length: {count}\r\n"
                                 "Content-Type: application/octet-stream\r\n"
                                 "X-Reuse-Fixture: 1\r\n\r\n").encode("ascii") + PAYLOADS[count]
                          for count in BODIES}
        super().__init__(("127.0.0.1", 0), Handler)

    def get_request(self):
        connection, address = self.socket.accept()
        with self.lock:
            self.accepted += 1
            over_budget = self.accepted > self.budget
        connection.settimeout(3)
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if over_budget:
            connection.close()
            raise OSError("fixture connection budget exceeded")
        try:
            connection = self.context.wrap_socket(connection, server_side=True)
        except Exception:
            connection.close()
            raise
        return connection, address

    def snapshot(self):
        with self.lock:
            return {"accepted_tcp": self.accepted, "requests": self.requests,
                    "errors": list(self.errors)}

    def handle_error(self, request, client_address):
        # A failed fixture always fails the run, including handler exceptions.
        with self.lock:
            self.errors.append("handler exception")


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        with self.server.lock:
            self.server.requests += 1
            sequence = self.server.requests
        port = self.server.server_address[1]
        headers = list(self.headers.items())
        expected = {"host": f"localhost:{port}", "accept": "*/*",
                    "accept-encoding": "identity"}
        match = re.fullmatch(r"/payload/(0|4096)", self.path)
        valid = (match and self.request_version == "HTTP/1.1" and
                 len(headers) == len(expected) and
                 {key.lower(): value for key, value in headers} == expected and
                 sequence <= self.server.budget)
        if not valid:
            with self.server.lock:
                self.server.errors.append("unexpected request shape or budget")
            self.close_connection = True
            self.send_error(400)
            return
        count = int(match.group(1))
        # Prebuilt complete responses avoid per-request payload generation cost.
        self.wfile.write(self.server.responses[count])
        self.wfile.flush()


def make_context(directory, commands):
    def openssl(*args):
        command = ["openssl", *map(str, args)]
        commands.append(command)
        run(command)

    ca_key, ca = directory / "ca.key", directory / "ca.pem"
    key, csr, cert = directory / "server.key", directory / "server.csr", directory / "server.pem"
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "1",
            "-subj", "/CN=goose-reuse-ephemeral-ca", "-addext", "basicConstraints=critical,CA:TRUE",
            "-keyout", ca_key, "-out", ca)
    openssl("req", "-new", "-newkey", "rsa:2048", "-nodes", "-sha256",
            "-subj", "/CN=localhost", "-keyout", key, "-out", csr)
    extensions = directory / "server.ext"
    extensions.write_text("subjectAltName=DNS:localhost\nbasicConstraints=critical,CA:FALSE\n"
                          "keyUsage=critical,digitalSignature,keyEncipherment\n"
                          "extendedKeyUsage=serverAuth\n")
    openssl("x509", "-req", "-in", csr, "-CA", ca, "-CAkey", ca_key, "-CAcreateserial",
            "-days", "1", "-sha256", "-extfile", extensions, "-out", cert)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.set_alpn_protocols(["http/1.1"])
    context.load_cert_chain(cert, key)
    return context, ca


def atomic_report(path, report):
    temporary = path.with_suffix(path.suffix + ".tmp")
    try:
        temporary.write_text(json.dumps(report, indent=2) + "\n")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--requests", type=int, default=30)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--warmup-requests", type=int, default=3)
    parser.add_argument("--sanitize", action="store_true", help="correctness only; omit timings")
    args = parser.parse_args()
    if not 2 <= args.requests <= 1000 or not 1 <= args.samples <= 50 or not 2 <= args.warmup_requests <= 100:
        parser.error("requests must be 2..1000, samples 1..50, warmup-requests 2..100")
    compiler = shlex.split(args.cc)
    if not compiler:
        parser.error("--cc must name GCC or Clang")
    label = re.sub(r"[^A-Za-z0-9_.-]", "_", Path(compiler[-1]).name)
    build = ROOT / ".build"
    build.mkdir(exist_ok=True)
    kind = "sanitize" if args.sanitize else "release"
    output = build / f"reuse-results-{label}-{kind}.json"
    # An interrupted/failed run must never leave an older successful report.
    output.unlink(missing_ok=True)
    output.with_suffix(output.suffix + ".tmp").unlink(missing_ok=True)
    compiler_version = run(compiler + ["--version"])
    if not any(name in compiler_version.lower() for name in ("gcc", "clang", "free software foundation")):
        parser.error("--cc must identify GCC or Clang")
    executable = shutil.which(compiler[0])
    require(executable is not None, "compiler executable not found")
    compiler_identity = {"path": str(Path(executable).resolve()), "sha256": digest(Path(executable))}
    started = utc_now()
    source_hashes = {source: digest(ROOT / source) for source in SOURCES}
    head = run(["git", "rev-parse", "HEAD"])
    dirty = run(["git", "status", "--porcelain=v1", "--untracked-files=all"])
    curl_flags = shlex.split(run(["pkg-config", "--cflags", "--libs", "libcurl"]))
    compile_commands, binary_info, library_hashes = [], {}, {}

    def compile_driver(sanitized):
        variant = "sanitize" if sanitized else "release"
        flags = (BASE_FLAGS + ["-O1", "-g", "-fno-omit-frame-pointer",
                              "-fsanitize=address,undefined", "-no-pie"] if sanitized
                 else BASE_FLAGS + ["-O2"])
        binary = build / f"reuse-{label}-{variant}"
        command = compiler + flags + ["bench/reuse.c", "native/http.c", "native/client.c"] + curl_flags + [
            "-Wl,--wrap=curl_easy_perform", "-o", str(binary)]
        compile_commands.append(command)
        run(command)
        binary_info[variant] = {"path": str(binary.relative_to(ROOT)), "sha256": digest(binary),
                                "flags": flags, "linked_libraries": run(["ldd", str(binary)])}
        for word in binary_info[variant]["linked_libraries"].split():
            if word.startswith('/') and Path(word).is_file():
                library_hashes[str(Path(word).resolve())] = digest(Path(word))
        return binary

    sanitized_binary = compile_driver(True)
    binary = sanitized_binary if args.sanitize else compile_driver(False)
    preflight_n = min(args.requests, 5)
    budget = len(BODIES) * len(MODES) * (args.requests if args.sanitize else
                preflight_n + args.warmup_requests + args.samples * args.requests)
    records, certificate_commands = [], []
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="reuse-fixture-", dir=build) as temporary:
        context, ca = make_context(Path(temporary), certificate_commands)
        fixture = Fixture(context, budget)
        server_thread = threading.Thread(target=fixture.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        server_thread.start()
        port = fixture.server_address[1]

        def batch(executable, mode, body, requests, phase, sample=None):
            before = fixture.snapshot()
            command = [str(executable), mode, f"https://localhost:{port}/payload/{body}", str(ca),
                       str(requests), str(body), "1" if phase == "measurement" else "0", str(port)]
            data = json.loads(run(command, timeout=min(300, requests * 3 + 10), env=env))
            after = fixture.snapshot()
            expected_connections = 1 if mode in REUSED_MODES else requests
            delta = {key: after[key] - before[key] for key in ("accepted_tcp", "requests")}
            require(delta == {"accepted_tcp": expected_connections, "requests": requests}, (mode, delta))
            require(not after["errors"], after["errors"])
            require(data["mode"] == mode and data["requests"] == requests and data["body_bytes"] == body)
            require(data["checksum_fnv1a64"] == fnv1a(PAYLOADS[body]))
            require(len(data["transfers"]) == requests)
            require(sum(item["new_connections"] for item in data["transfers"]) == expected_connections)
            for index, transfer in enumerate(data["transfers"]):
                require(transfer["new_connections"] == (1 if mode not in REUSED_MODES or index == 0 else 0))
                require(transfer["http_status"] == 200 and transfer["http_version"] == "1.1")
                require(transfer["ssl_verify_result"] == 0)
            data.update(phase=phase, sample=sample, fixture_delta=delta, fixture_before=before,
                        fixture_after=after, command=command)
            records.append(data)

        try:
            for body in BODIES:
                for mode in MODES:
                    batch(sanitized_binary, mode, body, args.requests if args.sanitize else preflight_n,
                          "sanitizer_correctness")
            if not args.sanitize:
                for body in BODIES:
                    for mode in MODES:
                        batch(binary, mode, body, args.warmup_requests, "warmup")
                for sample in range(args.samples):
                    # Rotate modes and body order to spread order/host drift.
                    offset = sample % len(MODES)
                    modes = MODES[offset:] + MODES[:offset]
                    for body in BODIES[::1 if sample % 2 == 0 else -1]:
                        for mode in modes:
                            batch(binary, mode, body, args.requests, "measurement", sample)
            final_counts = fixture.snapshot()
        finally:
            fixture.shutdown()
            fixture.server_close()
            server_thread.join(timeout=3)
            require(not server_thread.is_alive(), "fixture thread failed to stop")
    require(final_counts["requests"] == budget and not final_counts["errors"], final_counts)
    if source_hashes != {source: digest(ROOT / source) for source in SOURCES}:
        raise RuntimeError("measured sources changed during run; refusing report")
    require(digest(Path(compiler_identity["path"])) == compiler_identity["sha256"], "compiler changed during run")
    require(all(digest(Path(path)) == value for path, value in library_hashes.items()), "linked library changed during run")
    require(all(digest(ROOT / value["path"]) == value["sha256"] for value in binary_info.values()), "driver binary changed during run")
    summaries = []
    if not args.sanitize:
        for body in BODIES:
            groups = {mode: [row["elapsed_monotonic_ns"] / args.requests for row in records
                            if row["phase"] == "measurement" and row["mode"] == mode and row["body_bytes"] == body]
                      for mode in MODES}
            row = {"body_bytes": body, "median_batch_ns_per_request": {
                mode: statistics.median(values) for mode, values in groups.items()}}
            # Pair by rotated sample index; retain every ratio, never threshold it.
            row["paired_direct_cold_over_reuse"] = [cold / warm for cold, warm in
                                                    zip(groups["direct_cold"], groups["direct_reuse"])]
            row["median_paired_direct_speedup"] = statistics.median(row["paired_direct_cold_over_reuse"])
            row["paired_native_cold_over_client"] = [cold / warm for cold, warm in
                                                     zip(groups["native_cold"], groups["native_client"])]
            row["median_paired_native_speedup"] = statistics.median(row["paired_native_cold_over_client"])
            summaries.append(row)
    report = {
        "schema": 1, "started_utc": started, "finished_utc": utc_now(), "mode": kind,
        "configuration": vars(args), "git_head": head, "git_status_porcelain": dirty,
        "source_sha256": source_hashes, "driver_sha256": source_hashes["bench/reuse-measure.py"],
        "compiler_version": compiler_version, "compiler_executable": compiler_identity,
        "linked_library_sha256": library_hashes, "compile_commands": compile_commands,
        "pkg_config_libcurl_version": run(["pkg-config", "--modversion", "libcurl"]),
        "pkg_config_flags": curl_flags, "binaries": binary_info,
        "system": {"platform": platform.platform(), "machine": platform.machine(), "cpu": cpu_model(),
                   "python": platform.python_version(), "python_tls": ssl.OPENSSL_VERSION,
                   "affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
                   "nonexclusive_host": True},
        "fixture": {"bind": "127.0.0.1", "hostname": "localhost", "ephemeral_ca": True,
                    "server_tcp_nodelay": True, "minimum_tls": "1.2", "alpn": ["http/1.1"],
                    "timeout_seconds": 3, "request_budget": budget, "actual_counts": final_counts,
                    "certificate_commands": certificate_commands},
        "scope": __doc__, "records": records, "summary": summaries,
    }
    atomic_report(output, report)
    print(str(output.relative_to(ROOT)))
    for row in summaries:
        values = row["median_batch_ns_per_request"]
        print(f"{row['body_bytes']} bytes: native cold {values['native_cold']/1000:.1f} us/request; "
              f"direct cold {values['direct_cold']/1000:.1f}; reuse {values['direct_reuse']/1000:.1f}; "
              f"native client {values['native_client']/1000:.1f}; "
              f"paired direct/native speedups {row['median_paired_direct_speedup']:.2f}x/"
              f"{row['median_paired_native_speedup']:.2f}x")
    print(f"fixture verified {final_counts['requests']} requests on {final_counts['accepted_tcp']} accepted TCP connections")


if __name__ == "__main__":
    main()
