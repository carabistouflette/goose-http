#!/usr/bin/env python3
"""Require lifetime rejection of request inputs borrowed from response storage."""
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
HEADER = '''import http from "..";
import std;
fn main() {
    var out = str("https://example.invalid/");
    var err: u8[>..] = [];
    let view: const u8[:] = out[..];
'''
CASES = {
    'url-body': 'let options = http::Options { .. };\nlet s = http::request(view, options, out, err);',
    'url-headers': 'let options = http::Options { .. };\nlet s = http::request(view, options, err, out);',
    'upload-body': 'let options = http::Options { method: http::Method.Post, body: view };\nlet s = http::request("https://example.invalid", options, out, err);',
    'upload-headers': 'let options = http::Options { method: http::Method.Post, body: view };\nlet s = http::request("https://example.invalid", options, err, out);',
    'header-name': 'let fields = [http::Header { name: view, value: "value" }];\nlet options = http::Options { headers: fields };\nlet s = http::request("https://example.invalid", options, out, err);',
    'header-value': 'let fields = [http::Header { name: "X-Test", value: view }];\nlet options = http::Options { headers: fields };\nlet s = http::request("https://example.invalid", options, out, err);',
}
(ROOT / '.build').mkdir(exist_ok=True)
for name, call in CASES.items():
    path = ROOT / '.build' / f'reject-{name}.goose'
    path.write_text(HEADER + call + '\n print(s.error);\n}\n')
    result = subprocess.run([sys.argv[1], 'check', str(path)], capture_output=True, text=True, timeout=30)
    assert result.returncode != 0, f'{name}: aliasing unexpectedly accepted'
    assert '§5.1' in result.stderr and ('shrinks' in result.stderr or 'cannot clear' in result.stderr), result.stderr
print(f'{len(CASES)} ownership rejection cases passed')
