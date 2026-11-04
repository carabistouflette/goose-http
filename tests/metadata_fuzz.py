#!/usr/bin/env python3
"""Deterministic offline differential campaign for the metadata validator."""
# SPDX-License-Identifier: Apache-2.0
import argparse
from collections import Counter
import hashlib
import itertools
import json
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
REVISION = '0123456789abcdef0123456789abcdef01234567'
MAX_BODY = 262144
MAX_BATCH = 32 * 1024 * 1024


class Object(list):
    """Preserve decoded object keys and duplicates separately from arrays."""


def number(token):
    if len(token) > 128:
        raise ValueError('numeric lexeme limit')
    return 0  # Numeric fields are ignored; avoid huge integer/float conversion.


def constant(_):
    raise ValueError('non-JSON numeric constant')


def unicode_and_depth(value, depth=0):
    if isinstance(value, str):
        value.encode('utf-8', errors='strict')
    elif isinstance(value, (Object, list)):
        depth += 1
        if depth > 32:
            raise ValueError('container depth limit')
        if isinstance(value, Object):
            for key, child in value:
                key.encode('utf-8', errors='strict')
                unicode_and_depth(child, depth)
        else:
            for child in value:
                unicode_and_depth(child, depth)


def unique(value, key):
    if not isinstance(value, Object):
        raise ValueError('required object')
    matches = [child for name, child in value if name == key]
    if len(matches) != 1:
        raise ValueError('required unique member')
    return matches[0]


def oracle(body):
    try:
        if len(body) > MAX_BODY:
            return None
        document = json.loads(body.decode('utf-8', errors='strict'),
            object_pairs_hook=Object, parse_int=number, parse_float=number,
            parse_constant=constant)
        unicode_and_depth(document)
        sha = unique(document, 'sha')
        url = unique(document, 'html_url')
        message = unique(unique(document, 'commit'), 'message')
        if not all(isinstance(value, str) for value in (sha, url, message)):
            return None
        if sha != REVISION or not url:
            return None
        return dict(requested_revision=REVISION, sha=sha, html_url=url,
                    subject=re.split('[\r\n]', message, maxsplit=1)[0])
    except (ValueError, UnicodeError, RecursionError):
        return None


def document(message='subject\nbody', extra=None, ensure_ascii=False):
    return json.dumps(dict(sha=REVISION, html_url='https://example.invalid/metadata',
        commit=dict(message=message), extra=extra), ensure_ascii=ensure_ascii,
        separators=(',', ':')).encode('utf-8')


def fixed_corpus():
    good = document()
    corpus = [good, document(''), document('a\rb\nc'), document('"\\\x00\x1b\t雪😀\u2028\u2029'),
        document('雪😀', ensure_ascii=True), b'', b'{}', b'[]', b'null', b'0',
        b'"root"', b'\xff', good[:-1], good + b' trailing', good + b' {}',
        b'\xef\xbb\xbf' + good, good[:-1] + b',"unknown":"\xff"}',
        good[:-1] + b',"unknown":"\\ud800"}',
        good[:-1] + b',"unknown":"\\udfff"}',
        good[:-1] + b',"unknown":"\\ud83d\\ude00"}',
        good[:-1] + b',"\\ud800":0}']
    for token in [b'NaN', b'Infinity', b'-Infinity', b'01', b'+1', b'1.', b'.1',
                  b'1e', b'--1', b'1e+', b'1' + b'0' * 127,
                  b'1' + b'0' * 128, b'1e' + b'9' * 126, b'1e' + b'9' * 127]:
        corpus.append(good[:-1] + b',"number":' + token + b'}')
    for depth in [30, 31, 32, 33, 100]:
        corpus.append(good[:-1] + b',"nested":' + b'[' * depth + b'0' + b']' * depth + b'}')
    for key in ['sha', 'html_url', 'commit']:
        parsed = json.loads(good)
        original = parsed[key]
        del parsed[key]
        corpus.append(json.dumps(parsed).encode())
        for name in [key, '\\u%04x%s' % (ord(key[0]), key[1:])]:
            corpus.append(good[:-1] + b',"' + name.encode() + b'":' + json.dumps(original).encode() + b'}')
    for name in [b'message', b'messa\\u0067e']:
        corpus.append(good.replace(b'"message":"subject\\nbody"',
            b'"message":"subject\\nbody","' + name + b'":"duplicate"'))
    for value in [None, 0, True, [], {}, '']:
        for key in ['sha', 'html_url', 'commit']:
            parsed = json.loads(good); parsed[key] = value
            corpus.append(json.dumps(parsed).encode())
        corpus.append(good.replace(b'"message":"subject\\nbody"',
            b'"message":' + json.dumps(value).encode()))
    corpus.extend([good + b' ' * (MAX_BODY - len(good)),
                   good + b' ' * (MAX_BODY + 1 - len(good))])
    return corpus


def random_case(rng, seeds):
    mode = rng.randrange(4)
    if mode == 0:
        alphabet = ['a', '"', '\\', '\x00', '\x1b', '\t', '\r', '\n', '雪', '😀', '\u2028', '\u2029']
        message = ''.join(rng.choice(alphabet) for _ in range(rng.randrange(65)))
        return document(message, extra={'ignored': [None, True, rng.randrange(100)]},
                        ensure_ascii=bool(rng.randrange(2))), 'generated'
    if mode == 1:
        return rng.randbytes(rng.randrange(513)), 'raw'
    body = bytearray(rng.choice(seeds))
    for _ in range(rng.randint(1, 5)):
        operation = rng.randrange(3)
        at = rng.randrange(len(body) + 1)
        if operation == 0:
            body[at:at] = bytes([rng.randrange(256)])
        elif operation == 1:
            if at < len(body): del body[at]
        elif at < len(body):
            body[at] = rng.randrange(256)
    return bytes(body[:MAX_BODY + 1]), 'mutated'


def main():
    if not __debug__:
        raise SystemExit('oracle campaign requires Python without optimization')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary')
    parser.add_argument('--cases', type=int, default=10000, help='random cases in addition to fixed boundaries')
    parser.add_argument('--seed', type=int, default=491)
    parser.add_argument('--report', default='.build/metadata-fuzz-results.json')
    parser.add_argument('--label', default='unspecified', help='caller-supplied compiler/mode description')
    args = parser.parse_args()
    if not 1 <= args.cases <= 100000:
        parser.error('--cases must be 1..100000')
    binary = str(Path(args.binary).resolve())
    folder = ROOT / '.build'; folder.mkdir(exist_ok=True)
    report = ROOT / args.report
    report.parent.mkdir(parents=True, exist_ok=True)
    report.unlink(missing_ok=True)  # Never retain an old success after a failed run.
    for name in ['metadata-fuzz-failure.bin', 'metadata-fuzz-crash.bin']:
        (folder/name).unlink(missing_ok=True)
    corpus_path = folder / 'metadata-fuzz-corpus.bin'
    rng = random.Random(args.seed)
    seeds = fixed_corpus()
    bodies = itertools.chain(((body, 'fixed') for body in seeds),
        (random_case(rng, seeds) for _ in range(args.cases)))
    counts = Counter()
    digest = hashlib.sha256()
    accepted = 0
    start = time.monotonic()
    at = 0

    def execute(batch, framed, at):
        assert batch
        corpus_path.write_bytes(framed)
        try:
            result = subprocess.run([binary, str(corpus_path)], cwd=ROOT,
                capture_output=True, timeout=120)
        except subprocess.TimeoutExpired:
            (folder/'metadata-fuzz-crash.bin').write_bytes(framed)
            raise
        if result.returncode != 0 or result.stderr:
            (folder/'metadata-fuzz-crash.bin').write_bytes(framed)
            raise AssertionError(f'driver failure seed={args.seed} batch_start={at}: '
                                 f'exit={result.returncode}, stderr={result.stderr[:4096]!r}; '
                                 f'reproducer={folder / "metadata-fuzz-crash.bin"}')
        lines = result.stdout.splitlines()  # Bytes: U+2028/U+2029 are string data.
        assert len(lines) == len(batch), (at, len(lines), len(batch))
        accepted = 0
        for offset, (body, line) in enumerate(zip(batch, lines)):
            expected = oracle(body)
            try:
                actual = json.loads(line)
            except ValueError:
                actual = {'invalid_driver_output': line.hex()}
            if actual != expected:
                failure = folder / 'metadata-fuzz-failure.bin'
                failure.write_bytes(struct.pack('<I', len(body)) + body)
                raise AssertionError(f'oracle mismatch seed={args.seed} index={at + offset} '
                    f'sha256={hashlib.sha256(body).hexdigest()} expected={expected!r} actual={actual!r}; '
                    f'reproducer={failure}')
            accepted += expected is not None
        return accepted

    batch = []; framed = bytearray()
    for body, kind in bodies:
        record = struct.pack('<I', len(body)) + body
        if batch and (len(batch) == 1000 or len(framed) + len(record) > MAX_BATCH):
            accepted += execute(batch, framed, at)
            at += len(batch)
            batch = []; framed = bytearray()
        assert len(record) <= MAX_BATCH
        batch.append(body); framed.extend(record)
        digest.update(record); counts[kind] += 1
    if batch:
        accepted += execute(batch, framed, at)
        at += len(batch)
    command = lambda *a: subprocess.check_output(a, cwd=ROOT, text=True).strip()
    result = dict(status='passed', label=args.label,
        binary_sha256=hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
        python_version=sys.version,
        seed=args.seed, random_cases=args.cases, total_cases=at,
        case_kinds=dict(counts), accepted=accepted, rejected=at-accepted,
        corpus_sha256=digest.hexdigest(), elapsed_seconds=time.monotonic()-start,
        timing_scope='campaign execution only; not a performance benchmark',
        source_revision=command('git', 'rev-parse', 'HEAD'),
        source_dirty=bool(command('git', 'status', '--porcelain')),
        source_sha256={name: hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in
            ['examples/honk_metadata.goose', 'tests/metadata_fuzz.goose', 'tests/metadata_fuzz.py']})
    report.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
