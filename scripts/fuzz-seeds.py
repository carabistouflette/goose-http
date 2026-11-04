#!/usr/bin/env python3
"""Seed valid and boundary inputs so fuzzing reaches native header callbacks."""
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path

root = Path(__file__).resolve().parent.parent / '.build/fuzz-corpus'
root.mkdir(parents=True, exist_ok=True)
cases = {
    'url': b'https://example.invalid/fuzz',
    'empty-authority': b'https:///example.invalid/',
    'userinfo': b'https://user:password@example.invalid/',
    'empty-header': b'X:\0',
    'blank-header': b'X: \t \0',
    'headers-64': b'X: value\0' * 64,
    'headers-65': b'X: value\0' * 65,
    'header-limit': b'X: ' + b'x' * 65531 + b'\0',
    'header-over-limit': b'X: ' + b'x' * 65532 + b'\0',
    'blank-long-name': b'X' * 65532 + b':\0',
}
for name, data in cases.items():
    (root / name).write_bytes(data)
