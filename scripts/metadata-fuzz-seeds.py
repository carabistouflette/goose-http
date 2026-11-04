#!/usr/bin/env python3
"""Seed valid, boundary, and edge-case inputs for metadata coverage fuzzing."""
# SPDX-License-Identifier: Apache-2.0
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
import metadata_fuzz  # Reuse fixed corpus from deterministic test harness


def main():
    corpus_dir = ROOT / '.build/metadata-coverage-corpus'
    corpus_dir.mkdir(parents=True, exist_ok=True)

    seeds = metadata_fuzz.fixed_corpus()

    # Additional HTTP header seeds
    header_seeds = [
        b'HTTP/1.1 200 OK\r\n\r\n',
        b'HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\n\r\n',
        b'HTTP/1.1 200 OK\r\nCONTENT-ENCODING: IDENTITY\r\n\r\n',
        b'HTTP/1.1 200 OK\r\nContent-Encoding:   identity  \r\n\r\n',
        b'HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n',
        b'HTTP/1.1 200 OK\r\n Content-Encoding: identity\r\n\r\n',
        b'HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Encoding: identity\r\n\r\n',
        b'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Encoding: identity\r\n\r\n',
    ]
    seeds.extend(header_seeds)

    # Dual split seed: first 2 bytes specify split point (e.g. 19 bytes header, rest body)
    valid_hdr = b'HTTP/1.1 200 OK\r\n\r\n'
    valid_body = json.dumps({
        'sha': '0123456789abcdef0123456789abcdef01234567',
        'html_url': 'https://example.invalid/metadata',
        'commit': {'message': 'subject\nbody'}
    }).encode('utf-8')
    split_offset = len(valid_hdr)
    split_seed = bytes([split_offset & 0xFF, (split_offset >> 8) & 0xFF]) + valid_hdr + valid_body
    seeds.append(split_seed)

    for i, data in enumerate(seeds):
        h = hashlib.sha256(data).hexdigest()[:16]
        (corpus_dir / f'seed_{i:03d}_{h}.bin').write_bytes(data)


if __name__ == '__main__':
    main()
