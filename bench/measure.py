#!/usr/bin/env python3
"""Measure native and Goose HTTP requests against a loopback fixture."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("http_fixture", ROOT / "tests/integration.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def output(*command):
    return subprocess.check_output(command, cwd=ROOT, text=True, timeout=10).strip()


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
    parser.add_argument("--binary", default=str(ROOT / ".build/bench"))
    parser.add_argument("--goose-binary", default=str(ROOT / ".build/bench-goose"))
    parser.add_argument("--goose", default=str(ROOT / ".toolchain/goose/build/goose"))
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--output", default=str(ROOT / ".build/bench.json"))
    args = parser.parse_args()
    if not 1 <= args.iterations <= 100000 or not 1 <= args.samples <= 100:
        parser.error("iterations must be 1..100000 and samples 1..100")
    result = {
        "schema": 2, "workload": "per-call easy handle, HTTP loopback, 4096-byte body",
        "iterations_per_sample": args.iterations, "platform": platform.platform(),
        "machine": platform.machine(), "processor": cpu_model(),
        "cpu_count": os.cpu_count(), "compiler": output(args.cc, "--version"),
        "libcurl": output("pkg-config", "--modversion", "libcurl"),
        "libcurl_build_flags": output("pkg-config", "--cflags", "--libs", "libcurl"),
        "source_revision": output("git", "rev-parse", "HEAD"),
        "source_dirty": bool(output("git", "status", "--porcelain")),
        "goose_revision": (ROOT / "GOOSE_REVISION").read_text().strip(),
        "goose_executable": str(Path(args.goose).resolve()),
        "goose_version": output(str(Path(args.goose).resolve()), "--version"),
        "workloads": [
            {"label": "native_kernel", "binary": str(Path(args.binary).resolve()),
             "body_limit": 4096, "header_limit": 1024, "prezero_output_buffers": False,
             "flags": "-std=c11 -Wall -Wextra -Werror -O2 -pthread (plus pkg-config libcurl)",
             "raw_elapsed_ns": []},
            {"label": "goose_public_defaults", "binary": str(Path(args.goose_binary).resolve()),
             "body_limit": 1048576, "header_limit": 65536, "prezero_output_buffers": True,
             "flags": "goose build --cc " + args.cc + " -O2 (native CMake Release)",
             "raw_elapsed_ns": []},
        ],
    }
    with fixture.fixtures() as (base, _tls):
        for workload in result["workloads"]:
            command = [workload["binary"], base + "/bytes/4096", str(args.iterations)]
            subprocess.run(command, check=True, timeout=60, env=fixture.hostile_environment())
        # Alternate order to expose both workloads to comparable fixture drift.
        for sample in range(args.samples):
            workloads = result["workloads"] if sample % 2 == 0 else reversed(result["workloads"])
            for workload in workloads:
                command = [workload["binary"], base + "/bytes/4096", str(args.iterations)]
                start = time.perf_counter_ns()
                subprocess.run(command, check=True, timeout=300, env=fixture.hostile_environment())
                workload["raw_elapsed_ns"].append(time.perf_counter_ns() - start)
    destination = Path(args.output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(result, indent=2) + "\n")
    print(f"saved {args.samples} raw samples per workload to {destination}")


if __name__ == "__main__":
    main()
