#!/usr/bin/env python3
"""Compare native setup and copy callbacks against a Git baseline, without networking."""
# SPDX-License-Identifier: Apache-2.0
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess

ROOT = Path(__file__).resolve().parent.parent
FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-pthread"]
WORKLOADS = [
    ("setup_zero", 0, False, 0),
    ("setup_one", 1, False, 0),
    ("setup_64", 64, False, 0),
    ("setup_64_blank", 64, True, 0),
    ("callback_4096", 0, False, 4096),
    ("callback_16384", 0, False, 16384),
]


def run(command, timeout=300):
    return subprocess.check_output(command, cwd=ROOT, text=True, timeout=timeout).strip()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def normalized(text):
    return text.replace(str(ROOT), "<SOURCE>").replace(str(Path.home()), "<HOME>")


def cpu_model():
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unavailable"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default="9a8bed6", help="Git commit with native/http.c and native/http.h")
    parser.add_argument("--iterations", type=int, default=20000, help="setup iterations per sample; callbacks use 100 times this")
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--cpu", type=int, help="allowed Linux CPU; default 1 when available, otherwise another allowed CPU")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--output", default=".build/profile-results.json")
    args = parser.parse_args()
    if not 1 <= args.iterations <= 1000000 or not 1 <= args.samples <= 100:
        parser.error("iterations must be 1..1000000 and samples 1..100")
    if not hasattr(os, "sched_getaffinity"):
        parser.error("this profile harness requires Linux CPU affinity support")
    allowed = sorted(os.sched_getaffinity(0))
    cpu = args.cpu if args.cpu is not None else (1 if 1 in allowed else next((c for c in allowed if c != 0), allowed[0]))
    if cpu not in allowed:
        parser.error("--cpu must be in the process's allowed affinity set")
    # Pin the driver before compilation so compiler children use the same CPU.
    os.sched_setaffinity(0, {cpu})
    compiler = shlex.split(args.cc)
    if not compiler:
        parser.error("--cc must name a compiler")
    try:
        baseline = run(["git", "rev-parse", "--verify", "--end-of-options", args.baseline + "^{commit}"])
    except subprocess.CalledProcessError:
        parser.error("baseline commit unavailable; fetch its history or pass --baseline COMMIT")
    curl_cflags = shlex.split(run(["pkg-config", "--cflags", "libcurl"]))
    curl_libs = shlex.split(run(["pkg-config", "--libs", "libcurl"]))
    curl_flags = curl_cflags + curl_libs
    source_hashes, stacks, binaries = {}, {}, {}
    for label in ("baseline", "current"):
        snapshot = Path(".build") / ("profile-" + label + "-src")
        (ROOT / snapshot).mkdir(parents=True, exist_ok=True)
        hashes = {}
        for name in ("http.c", "http.h"):
            relative = "native/" + name
            data = (subprocess.check_output(["git", "show", baseline + ":" + relative], cwd=ROOT)
                    if label == "baseline" else (ROOT / relative).read_bytes())
            (ROOT / snapshot / name).write_bytes(data)
            hashes[relative] = digest(data)
        source_hashes[label] = hashes
        binary = str(Path(".build") / ("profile-" + label))
        run(compiler + FLAGS + ["-I" + str(snapshot), '-DPROFILE_IMPLEMENTATION="http.c"',
            "bench/profile.c"] + curl_flags + ["-Wl,--wrap=curl_easy_perform", "-o", binary])
        binaries[label] = binary
        # Compile production separately: this frame report excludes the driver.
        obj = str(Path(".build") / ("profile-" + label + "-kernel.o"))
        run(compiler + FLAGS + curl_cflags + ["-fstack-usage", "-c", str(snapshot / "http.c"), "-o", obj])
        stack_file = ROOT / Path(obj).with_suffix(".su")
        frames = []
        for line in stack_file.read_text().splitlines():
            fields = line.split("\t")
            if fields[0].rsplit(":", 1)[-1] == "gh_request":
                frames.append({"bytes": int(fields[1]), "kind": fields[2]})
        if not frames:
            raise RuntimeError("compiler stack report did not contain gh_request")
        stacks[label] = {"gh_request_frames": frames, "max_gh_request_frame_bytes": max(f["bytes"] for f in frames)}
    result = {
        "schema": 1,
        "recorded_utc": datetime.now(timezone.utc).isoformat(),
        "baseline_revision": baseline,
        "current_revision": run(["git", "rev-parse", "HEAD"]),
        "current_source_dirty": bool(run(["git", "status", "--porcelain"])),
        "source_sha256": source_hashes,
        "harness_sha256": {name: digest((ROOT / name).read_bytes()) for name in ("bench/profile.c", "bench/profile.py")},
        "compiler_command": normalized(args.cc),
        "compiler": normalized(run(compiler + ["--version"])),
        "flags": FLAGS,
        "link_flags": ["-Wl,--wrap=curl_easy_perform"],
        "libcurl": run(["pkg-config", "--modversion", "libcurl"]),
        "libcurl_runtime": {label: run([binary, "--version"]) for label, binary in binaries.items()},
        "libcurl_build_flags": normalized(" ".join(curl_flags)),
        "platform": platform.platform(), "machine": platform.machine(),
        "processor": cpu_model(), "cpu_count": os.cpu_count(),
        "original_allowed_cpus": allowed, "measured_cpu_affinity": [cpu],
        "goose_revision": (ROOT / "GOOSE_REVISION").read_text().strip(),
        "stack_usage": stacks,
        "boundaries": [
            "Setup includes native validation, URL parsing, easy handle/list allocation, option setup, getinfo and cleanup.",
            "curl_easy_perform returns CURLE_RECV_ERROR without DNS, sockets, TLS or HTTP parsing; each setup call must report GH_TRANSPORT and zero returned lengths.",
            "Callback workloads call the selected implementation's actual body callback through a volatile function pointer using its private buffer type, reset per copy.",
            "Each process performs 1000 excluded warmup iterations before internal wall/process-CPU clocks. Process startup and curl global initialization are excluded.",
            "Callback buffers are allocated and initialized before timing; setup output buffers are bounded 4096/1024 bytes and not prezeroed.",
            "Setup packed header formatting is included once per timed loop, not per request. Assertions and checksums are included; no Goose public preparation is measured.",
            "The reported production gh_request frame excludes its callees, compiler/runtime stacks and the benchmark driver.",
            "Samples alternate baseline/current order. Timing is observational, with no thresholds or network-throughput ranking.",
        ],
        "workloads": [],
    }
    for name, headers, blank, body_bytes in WORKLOADS:
        iterations = args.iterations * (100 if body_bytes else 1)
        samples = []
        for sample in range(args.samples):
            order = ["baseline", "current"] if sample % 2 == 0 else ["current", "baseline"]
            pair = {"sample": sample, "execution_order": order}
            for label in order:
                pair[label] = json.loads(run([binaries[label], name, str(iterations)]))
            samples.append(pair)
        summary = {}
        for label in ("baseline", "current"):
            summary[label] = {
                "median_cpu_ns_per_iteration": statistics.median(s[label]["cpu_ns"] / iterations for s in samples),
                "median_wall_ns_per_iteration": statistics.median(s[label]["wall_ns"] / iterations for s in samples),
            }
        result["workloads"].append({"name": name, "request_header_count": headers,
            "blank_request_values": blank, "callback_bytes": body_bytes,
            "iterations_per_sample": iterations, "samples": samples, "summary": summary})
    destination = Path(args.output)
    if not destination.is_absolute():
        destination = ROOT / destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(result, indent=2) + "\n")
    print("saved profile samples to " + normalized(str(destination)))


if __name__ == "__main__":
    main()
