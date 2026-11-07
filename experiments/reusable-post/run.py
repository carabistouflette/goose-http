#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run a research-only, verified-TLS libcurl POST replay probe on loopback."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import socket
import socketserver
import ssl
import subprocess
import tempfile
import threading
import time

if not __debug__:
    raise RuntimeError("probe requires Python assertions")

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = ROOT / '.build' / 'reusable-post-research'
BUILD.mkdir(parents=True, exist_ok=True)
JSON_BODIES = [b'{"jsonrpc":"2.0","id":1,"method":"eth_blockNumber","params":[]}',
               b'{"jsonrpc":"2.0","id":2,"method":"eth_blockNumber","params":[]}']
SECOND = {'json': JSON_BODIES[1], 'empty': b'', 'binary': b'A\x00B'}


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, context, drop):
        super().__init__(('127.0.0.1', 0), Handler)
        self.context, self.drop = context, drop
        self.lock = threading.Lock()
        self.drained = threading.Condition(self.lock)
        self.pending = 0
        self.accepts, self.events, self.errors = 0, [], []

    def process_request(self, request, address):
        with self.drained:
            self.pending += 1
        try:
            super().process_request(request, address)
        except BaseException:
            with self.drained:
                self.pending -= 1
                self.drained.notify_all()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            with self.drained:
                self.pending -= 1
                self.drained.notify_all()

    def get_request(self):
        sock, address = super().get_request()
        with self.lock:
            self.accepts += 1
            conn = self.accepts
        if self.context is None:
            return sock, (address, conn)
        try:
            wrapped = self.context.wrap_socket(sock, server_side=True)
        except Exception:
            sock.close()
            raise
        return wrapped, (address, conn)


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(5)
        conn = self.client_address[1]
        try:
            while line := self.rfile.readline(16384):
                # Log request lines even if headers or body are incomplete.
                event = {'connection': conn, 'line_hex': line.hex(), 'id': None,
                         'declared': None, 'body_hex': None}
                with self.server.lock:
                    self.server.events.append(event)
                method, path, version = line.rstrip(b'\r\n').split(b' ')
                headers = {}
                for _ in range(100):
                    line = self.rfile.readline(65536)
                    if line == b'\r\n':
                        break
                    name, value = line.rstrip(b'\r\n').split(b':', 1)
                    headers[name.lower()] = value.strip()
                else:
                    raise AssertionError('too many headers')
                assert method == b'POST' and path == b'/rpc' and version == b'HTTP/1.1'
                reqid = headers[b'x-request-id'].decode('ascii')
                expected_type = b'application/octet-stream' if reqid == '2' and self.server.payload == 'binary' else b'application/json'
                assert headers.get(b'content-type') == expected_type
                n = int(headers.get(b'content-length', b'0'))
                assert 0 <= n <= 1024
                with self.server.lock:
                    event['id'], event['declared'] = reqid, n
                    second_count = sum(e['id'] == '2' for e in self.server.events)
                if self.server.drop == 'after_headers' and reqid == '2' and second_count == 1:
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return
                body = self.rfile.read(n)
                with self.server.lock:
                    event['body_hex'] = body.hex()
                    second_count = sum(e['id'] == '2' for e in self.server.events)
                assert len(body) == n
                if self.server.drop == 'after_body' and reqid == '2' and second_count == 1:
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return
                if self.server.drop == 'partial_response' and reqid == '2' and second_count == 1:
                    self.wfile.write(b'HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc')
                    self.wfile.flush()
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return
                response = b'{"jsonrpc":"2.0","id":' + reqid.encode() + b',"result":"0x1"}'
                self.wfile.write(b'HTTP/1.1 200 OK\r\nContent-Length: ' + str(len(response)).encode() +
                                 b'\r\nConnection: keep-alive\r\n\r\n' + response)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, TimeoutError, ssl.SSLError):
            pass
        except Exception as exc:
            with self.server.lock:
                self.server.errors.append(repr(exc))


def run_case(binary, cert, context, guard, payload, drop):
    with Server(context, drop) as server:
        server.payload = payload
        thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.02})
        thread.start()
        try:
            url = f'https://localhost:{server.server_address[1]}/rpc'
            child = subprocess.run([str(binary), url, str(cert), guard, payload], capture_output=True,
                                   text=True, timeout=15, check=True)
            result = json.loads(child.stdout)
        finally:
            server.shutdown()
            thread.join(timeout=5)
            assert not thread.is_alive()
            deadline = time.monotonic() + 7
            with server.drained:
                while server.pending:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise AssertionError('fixture handler did not drain')
                    server.drained.wait(remaining)
        with server.lock:
            events = [dict(event) for event in server.events]
            errors, accepts = list(server.errors), server.accepts
        assert not errors, errors
        assert [e['id'] for e in events[:2]] == ['1', '2'], events
        assert events[0]['body_hex'] == JSON_BODIES[0].hex(), events
        if drop == 'after_headers':
            assert events[1]['body_hex'] is None and events[1]['declared'] == len(SECOND[payload]), events
        else:
            assert events[1]['body_hex'] == SECOND[payload].hex(), events
        assert result['first_code'] == 0 and result['first_status'] == 200, result
        assert events[0]['connection'] == events[1]['connection'], events
        if not drop:
            assert len(events) == 2 and result['second_code'] == 0 and result['second_status'] == 200
            assert result['second_connects'] == 0 and result['second_callbacks'] == 1
        elif guard == 'guarded':
            assert len(events) == 2 and result['second_code'] != 0, (events, result)
        else:
            # libcurl versions can differ; report observed replay instead of requiring it.
            assert len(events) in (2, 3), events
            if len(events) == 3:
                assert events[2]['id'] == '2' and events[2]['body_hex'] == SECOND[payload].hex(), events
                assert result['second_code'] == 0 and result['second_status'] == 200, result
            else:
                assert result['second_code'] != 0, result
        return {'guard': guard, 'payload': payload, 'failure': drop if drop else 'none',
                'tcp_accepts': accepts, 'requests': events, 'client': result}


def untrusted_case(binary, bad_ca, context):
    with Server(context, False) as server:
        server.payload = 'json'
        thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.02})
        thread.start()
        try:
            url = f'https://localhost:{server.server_address[1]}/rpc'
            child = subprocess.run([str(binary), url, str(bad_ca), 'guarded', 'json'],
                                   capture_output=True, text=True, timeout=15, check=True)
            result = json.loads(child.stdout)
        finally:
            server.shutdown()
            thread.join(timeout=5)
            assert not thread.is_alive()
            deadline = time.monotonic() + 7
            with server.drained:
                while server.pending:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise AssertionError('untrusted fixture did not drain')
                    server.drained.wait(remaining)
        with server.lock:
            events = [dict(event) for event in server.events]
            errors = list(server.errors)
        assert result['first_code'] == 60 and result['second_code'] == 60, result
        assert not events and not errors, (events, errors)
        return {'client': result, 'requests': events}


def one_shot_case(binary):
    with Server(None, False) as server:
        server.payload = 'json'
        thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.02})
        thread.start()
        try:
            port = str(server.server_address[1])
            child = subprocess.run([str(binary), port], capture_output=True, text=True, timeout=15, check=True)
            assert json.loads(child.stdout) == {'one_shot': 'passed'}
        finally:
            server.shutdown()
            thread.join(timeout=5)
            assert not thread.is_alive()
            deadline = time.monotonic() + 7
            with server.drained:
                while server.pending:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise AssertionError('one-shot fixture did not drain')
                    server.drained.wait(remaining)
        with server.lock:
            events = [dict(event) for event in server.events]
            errors, accepts = list(server.errors), server.accepts
        assert not errors and [e['id'] for e in events] == ['1', '2'], (events, errors)
        assert [e['body_hex'] for e in events] == [body.hex() for body in JSON_BODIES], events
        assert events[0]['connection'] != events[1]['connection'] and accepts == 2, events
        return {'mode': 'goose-http one-shot HTTP loopback', 'tcp_accepts': accepts, 'requests': events}


def main():
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libcurl'], text=True))
    binary = BUILD / 'probe'
    compile_env = os.environ.copy()
    compile_env['TMPDIR'] = str(BUILD)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', str(HERE / 'probe.c'),
                    '-o', str(binary), *flags], check=True, env=compile_env)
    goose = Path(os.environ.get('GOOSE', ROOT / '.toolchain/goose/build/goose')).resolve()
    baseline = BUILD / 'one-shot'
    subprocess.run([str(goose), 'build', str(HERE / 'one_shot.goose'), '-o', str(baseline)],
                   check=True, env=compile_env, capture_output=True, text=True)
    one_shot = one_shot_case(baseline)
    with tempfile.TemporaryDirectory(prefix='tls-', dir=BUILD) as temp:
        root = Path(temp)
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                        '-keyout', str(root / 'key.pem'), '-out', str(root / 'cert.pem')],
                       check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=unrelated-ca', '-keyout', str(root / 'other-key.pem'),
                        '-out', str(root / 'other-cert.pem')], check=True, timeout=30,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(root / 'cert.pem', root / 'key.pem')
        context.set_alpn_protocols(['http/1.1'])
        negative = untrusted_case(binary, root / 'other-cert.pem', context)
        invalid_url = subprocess.run([str(binary), 'https://localhost:443@other.invalid/rpc',
                                      str(root / 'cert.pem'), 'guarded', 'json'], timeout=5)
        assert invalid_url.returncode == 2
        scenarios = [run_case(binary, root / 'cert.pem', context, 'guarded', 'json', False)]
        for payload in ('json', 'empty', 'binary'):
            plain = run_case(binary, root / 'cert.pem', context, 'unguarded', payload, 'after_body')
            guarded = run_case(binary, root / 'cert.pem', context, 'guarded', payload, 'after_body')
            if len(plain['requests']) == 3:
                assert guarded['client']['second_code'] == 42 and guarded['client']['second_callbacks'] == 2, guarded
                guarded['guard_exercised'] = True
            else:
                guarded['guard_exercised'] = False
            scenarios.extend((plain, guarded))
        for failure in ('after_headers', 'partial_response'):
            plain = run_case(binary, root / 'cert.pem', context, 'unguarded', 'json', failure)
            guarded = run_case(binary, root / 'cert.pem', context, 'guarded', 'json', failure)
            if len(plain['requests']) == 3:
                assert guarded['client']['second_code'] == 42 and guarded['client']['second_callbacks'] == 2, guarded
                guarded['guard_exercised'] = True
            else:
                guarded['guard_exercised'] = False
            scenarios.extend((plain, guarded))
    report = {'scope': 'direct libcurl loopback TLS probe + separate cleartext HTTP one-shot baseline (not goose-http library core)',
              'compiler': subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
              'goose_version': subprocess.check_output([str(goose), '--version'], text=True).strip(),
              'goose_binary_sha256': hashlib.sha256(goose.read_bytes()).hexdigest(),
              'configured_goose_revision': (ROOT / 'GOOSE_REVISION').read_text().strip(),
              'base_source_revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'pkg_config_libcurl': subprocess.check_output(['pkg-config', '--modversion', 'libcurl'], text=True).strip(),
              'sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (HERE / 'probe.c', HERE / 'run.py', HERE / 'one_shot.goose')},
              'one_shot': one_shot, 'untrusted_ca': negative, 'invalid_url_rejected': True,
              'scenarios': scenarios}
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
