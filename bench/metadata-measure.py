#!/usr/bin/env python3
"""Measure the real compiled Honk metadata consumer over fixture TLS and pure validation."""
# SPDX-License-Identifier: Apache-2.0
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
import metadata as fixture
from integration import response


def command(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True, timeout=20).strip()


def cpu_model():
    for line in Path('/proc/cpuinfo').read_text().splitlines():
        if line.startswith('model name'):
            return line.split(':', 1)[1].strip()
    return platform.processor()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--consumer', default='.build/metadata-consumer')
    parser.add_argument('--parser', default='.build/metadata-parse')
    parser.add_argument('--goose', required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--samples', type=int, default=5)
    parser.add_argument('--requests', type=int, default=10)
    parser.add_argument('--iterations', type=int, default=2000)
    parser.add_argument('--cpu', type=int, help='CPU 2 when allowed, otherwise the first allowed CPU')
    parser.add_argument('--output', default='.build/metadata-bench.json')
    args = parser.parse_args()
    if not 1 <= args.samples <= 100 or not 1 <= args.requests <= 1000 or not 1 <= args.iterations <= 1000000:
        parser.error('samples1..100, requests1..1000, iterations1..1000000 required')
    allowed = sorted(os.sched_getaffinity(0))
    cpu = args.cpu if args.cpu is not None else (2 if 2 in allowed else allowed[0])
    if cpu not in allowed:
        parser.error('--cpu must be allowed by this process')
    os.sched_setaffinity(0, {cpu})
    binary, parser_binary = (str((ROOT / p).resolve()) for p in (args.consumer, args.parser))
    # Synthetic GitHub-shaped response: required fields plus realistic ignored file metadata.
    body = fixture.document(files=[{'filename': f'src/unit{i}.goose', 'status': 'modified',
        'additions': 20, 'deletions': 5, 'patch': '+fixture source line\n' * 40} for i in range(8)])
    digest = hashlib.sha256(body).hexdigest()
    path = ROOT / '.build/metadata-fixture.json'
    path.write_bytes(body)
    result = {
        'schema': 1, 'recorded_utc': datetime.now(timezone.utc).isoformat(),
        'source_revision': command('git', 'rev-parse', 'HEAD'),
        'source_dirty': bool(command('git', 'status', '--porcelain')),
        'source_sha256': {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in
            ('http.goose', 'http.build', 'CMakeLists.txt', 'native/http.c', 'native/http.h', 'examples/honk_metadata.goose',
             'examples/toolchain-metadata.goose', 'bench/metadata.goose',
             'bench/metadata-measure.py', 'tests/metadata.py', 'tests/metadata_transport.c',
             'scripts/metadata-test-build.py')},
        'goose_revision': (ROOT / 'GOOSE_REVISION').read_text().strip(),
        'goose_version': command(str(Path(args.goose).resolve()), '--version'),
        'text_revision': '46059e30be79ece6efd4317eb0adc6d213c808ab',
        'compiler': command(args.cc, '--version'), 'flags': '-O2 --standalone',
        'libcurl': command('pkg-config', '--modversion', 'libcurl'),
        'libcurl_flags': command('pkg-config', '--cflags', '--libs', 'libcurl'),
        'platform': platform.platform(), 'cpu_model': cpu_model(),
        'original_allowed_cpus': allowed, 'affinity': [cpu],
        'fixture_bytes': len(body), 'fixture_sha256': digest,
        'body_limit': 262144, 'header_limit': 32768, 'transfer_timeout_ms': 5000,
        'shared_host': True, 'requests_per_sample': args.requests,
        'validation_iterations_per_sample': args.iterations,
        'tls_cli_elapsed_ns': [], 'validation_raw': [],
        'boundaries': [
            'TLS workload includes fresh CLI process/runtime startup, public buffer preparation, real local TLS, fixture-server scheduling/work, fetch, validation, escaping, output capture and Python decoding/assertions.',
            'Only a test-linked transport wrapper rewrites the fixed GitHub URL to localhost and trusts the ephemeral CA. Verification and request policy remain enabled.',
            'Fixture generation/certificate creation and two warmup fetches are excluded; every measured response is checked.',
            'Validation workload uses internal monotonic timing, excludes 100 warmups and file reading; includes parser validation, owned Info, JSON escaping and assertions.',
            'Parser input is synthetic GitHub-shaped metadata with ignored file/patch fields. No live-network timing or throughput ranking is claimed.',
            'CPU affinity is best effort on a shared machine, not exclusive isolation. No timing thresholds are used.',
        ],
    }
    with fixture.fixture() as (server, environment):
        server.reply = response(body)
        def request():
            outcome = fixture.invoke(binary, environment)
            assert outcome.returncode == 0 and outcome.stderr == b'', outcome.stderr
            parsed = json.loads(outcome.stdout)
            assert parsed == {'requested_revision': fixture.REVISION, 'sha': fixture.REVISION,
                'html_url': fixture.URL, 'subject': fixture.MESSAGE.split('\n', 1)[0]}
            return len(outcome.stdout.rstrip(b'\n'))
        rendered_bytes = request()
        assert request() == rendered_bytes
        for _ in range(args.samples):
            started = time.perf_counter_ns()
            for _ in range(args.requests):
                assert request() == rendered_bytes
            result['tls_cli_elapsed_ns'].append(time.perf_counter_ns() - started)
            measured = subprocess.run([parser_binary, str(path), str(args.iterations)],
                check=True, capture_output=True, text=True, timeout=60)
            parsed = json.loads(measured.stdout)
            assert parsed['iterations'] == args.iterations and parsed['warmup'] == 100
            assert parsed['elapsed_ns'] > 0
            assert parsed['checksum'] == rendered_bytes * (args.iterations + 100)
            result['validation_raw'].append(parsed)
        assert len(server.requests) == 2 + args.samples * args.requests and not server.errors
    output = ROOT / args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    # Only paths in executable metadata could be local; normalize them before publication.
    payload = json.dumps(result, indent=2).replace(str(ROOT), '<SOURCE>').replace(str(Path.home()), '<HOME>')
    output.write_text(payload + '\n')
    print('saved metadata benchmark:', output.relative_to(ROOT))


if __name__ == '__main__':
    main()
