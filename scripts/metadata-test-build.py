#!/usr/bin/env python3
"""Build the real metadata CLI with an isolated, test-only HTTPS routing shim."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--goose", required=True)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--output", help="executable path (default: compiler/mode build directory)")
    parser.add_argument("--mode", choices=("release", "sanitize"), default="release")
    arguments = parser.parse_args()
    compiler = shutil.which(arguments.cc)
    if compiler is None:
        parser.error(f"compiler not found: {arguments.cc}")
    # Goose keys native package caches by compiler path; every mode/compiler gets its own.
    flags = ["-O2"] if arguments.mode == "release" else [
        "-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined"]
    # Fingerprint every wrapper input as well as mode/compiler. Otherwise Goose's
    # native cache may keep a kernel built with an older wrapper's flags/shim.
    identity = hashlib.sha256(Path(__file__).read_bytes() +
        (ROOT / "tests/metadata_transport.c").read_bytes() +
        repr((compiler, flags)).encode()).hexdigest()[:16]
    name = f"metadata-{Path(compiler).name}-{arguments.mode}-{identity}"
    folder = ROOT / ".build" / name
    folder.mkdir(parents=True, exist_ok=True)
    shim = folder / "transport.o"
    curl_flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "libcurl"], text=True, timeout=10))
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fPIC",
                    *flags, *curl_flags, "-c", str(ROOT / "tests/metadata_transport.c"),
                    "-o", str(shim)], check=True, timeout=60)
    wrapper = folder / "cc"
    # Compile flags instrument both generated Goose/runtime C and the native kernel.
    # Only final links receive the precompiled shim; compiler probes may link it too.
    wrapper.write_text("#!/usr/bin/env python3\nimport os, sys\n"
                       f"compiler = {compiler!r}\nflags = {flags!r}\n"
                       "args = sys.argv[1:]\n"
                       "probe = any(a in args for a in ('-v', '--version', '-dumpversion', '-dumpmachine'))\n"
                       "link = not probe and not any(a in args for a in ('-c', '-E', '-S'))\n"
                       f"extra = [{str(shim)!r}, '-Wl,--wrap=curl_easy_perform', '-Wl,--wrap=curl_easy_setopt', '-lcurl'] if link else []\n"
                       + ("if link: extra.append('-no-pie')\n" if arguments.mode == "sanitize" else "")
                       + "os.execv(compiler, [compiler, *args, *([] if probe else flags), *extra])\n")
    wrapper.chmod(0o755)
    binary = (ROOT / arguments.output).resolve() if arguments.output else folder / "toolchain-metadata"
    binary.parent.mkdir(parents=True, exist_ok=True)
    command = [str(Path(arguments.goose).resolve()), "build", "examples/toolchain-metadata.goose",
               "--cc", str(wrapper), "--standalone", "-o", str(binary)]
    print(shlex.join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True, timeout=240)
    print(binary)


if __name__ == "__main__":
    main()
