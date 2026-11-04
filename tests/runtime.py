#!/usr/bin/env python3
"""Run each process-lifetime libcurl fault with a fresh initializer."""
# SPDX-License-Identifier: Apache-2.0
import subprocess
import sys

MODES = (
    'init-failure', 'missingSSL', 'missingASYNCHDNS', 'missingTHREADSAFE',
    'runtimeoldversion', 'getinfo-failure-after-success',
    'getinfo-failure-after-timeout',
)
if len(sys.argv) != 2:
    raise SystemExit('usage: runtime.py BINARY')
for mode in MODES:
    subprocess.run([sys.argv[1], mode], check=True, timeout=10)
