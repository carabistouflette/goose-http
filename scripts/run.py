#!/usr/bin/env python3
"""Bound a hostile-input test executable's total run time."""
# SPDX-License-Identifier: Apache-2.0
import subprocess
import sys

result = subprocess.run(sys.argv[1:], timeout=60, check=False)
sys.exit(result.returncode)
