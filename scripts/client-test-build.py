#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the public client API fixture with a test-only CA link wrapper."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    subprocess.run([str(x) for x in command], check=True, timeout=600, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', type=Path, required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if not compiler:
        raise RuntimeError('C compiler not found')
    folder = ROOT / '.build' / ('client-api-' + Path(compiler).name + ('-sanitize' if args.sanitize else '-release'))
    folder.mkdir(parents=True, exist_ok=True)
    compiler = str(Path(compiler).resolve())
    flags = ['-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'] if args.sanitize else ['-O2']
    curl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'libcurl'], text=True))
    command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', *flags, *curl,
               '-c', ROOT / 'tests/client_transport.c', '-o', folder / 'transport.o']
    run(command)
    log = folder / 'compiler-commands.jsonl'
    log.write_text(json.dumps([str(x) for x in command]) + '\n')
    wrapper = folder / 'cc'
    extra = [str(folder / 'transport.o'), '-Wl,--wrap=curl_easy_perform', '-lcurl']
    wrapper.write_text('#!/usr/bin/env python3\nimport json,os,sys\n'
        f'compiler={compiler!r}\nflags={flags!r}\nlog={str(log)!r}\n'
        'args=sys.argv[1:]\n'
        "probe=any(a in args for a in ('-v','--version','-dumpversion','-dumpmachine'))\n"
        "link=not probe and not any(a in args for a in ('-c','-E','-S'))\n"
        "program=link and any(a.startswith('@') and a.endswith(('/link-cc.rsp','/link-cxx.rsp')) for a in args)\n"
        f'extra={extra!r} if program else []\n'
        + ("if link: extra.append('-no-pie')\n" if args.sanitize else '')
        + 'command=[compiler,*args,*([] if probe else flags),*extra]\n'
        "with open(log,'a') as f: f.write(json.dumps(command)+'\\n')\n"
        'os.execv(compiler,command)\n')
    wrapper.chmod(0o755)
    binary = folder / 'api'
    run([args.goose.resolve(), 'build', ROOT / 'tests/client_api.goose', '--standalone', '--cc', wrapper, '-o', binary], cwd=ROOT)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    run(['python3', ROOT / 'tests/clients.py', '--api', binary], cwd=ROOT, env=env)


if __name__ == '__main__':
    main()
