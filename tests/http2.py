#!/usr/bin/env python3
"""Negotiate real HTTP/2 over local TLS, using hyper-h2 only as a test server."""
# SPDX-License-Identifier: Apache-2.0
import collections
import contextlib
import gzip
import socketserver
import ssl
import subprocess
import sys
import threading
import time

try:
    import h2
    import h2.config
    import h2.connection
    import h2.events
    import h2.exceptions
except ImportError as error:
    raise SystemExit('HTTP/2 tests require python3-h2 (test dependency only)') from error

from integration import BINARY, Server, certificate, hostile_environment


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        connection = h2.connection.H2Connection(config=h2.config.H2Configuration(
            client_side=False, header_encoding='utf-8'))
        requests, pending, reset_wait = {}, {}, {}
        try:
            if self.request.selected_alpn_protocol() != 'h2':
                raise AssertionError('client did not negotiate h2 through ALPN')
            connection.initiate_connection()
            self.request.sendall(connection.data_to_send())
            while data := self.request.recv(65536):
                for event in connection.receive_data(data):
                    if isinstance(event, h2.events.RequestReceived):
                        requests[event.stream_id] = [dict(event.headers), bytearray()]
                    elif isinstance(event, h2.events.DataReceived):
                        headers, body = requests[event.stream_id]
                        body.extend(event.data)
                        if len(body) > 1048576:
                            raise AssertionError('fixture upload exceeded its bound')
                        connection.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
                    elif isinstance(event, h2.events.StreamReset):
                        pending.pop(event.stream_id, None)
                        requests.pop(event.stream_id, None)
                    elif isinstance(event, h2.events.PingAckReceived):
                        stream = reset_wait.pop(event.ping_data)
                        connection.reset_stream(stream, error_code=2)
                        with self.server.lock:
                            self.server.resets += 1
                            self.server.reset_acks += 1
                    elif isinstance(event, h2.events.StreamEnded):
                        headers, upload = requests.pop(event.stream_id)
                        path = headers[':path']
                        with self.server.lock:
                            self.server.paths[path] += 1
                        if self.server.destination:
                            raise AssertionError('client followed a cross-origin redirect')
                        status, body, extra, trailers = '200', BINARY, [], []
                        if path == '/echo':
                            if headers[':method'] != 'POST' or headers.get('content-type') != '':
                                raise AssertionError('binary POST lost its explicit empty content-type')
                            body = bytes(upload)
                        elif path == '/empty':
                            status, body = '204', b''
                        elif path == '/empty-header':
                            if headers.get('x-empty') != '':
                                raise AssertionError('empty request field was suppressed')
                            body = b'yes'
                        elif path in ('/trailers', '/trailers-with-length'):
                            connection.send_headers(event.stream_id, [(':status', '103'), ('link', '</hint>')])
                            body, trailers = b'abc', [('x-end', 'done')]
                        elif path == '/redirect':
                            if headers.get('authorization') != 'Bearer fixture-only':
                                raise AssertionError('explicit credential header was lost')
                            status, body = '302', b'redirect'
                            extra = [('location', self.server.redirect_origin + '/destination')]
                        elif path in ('/unauthorized', '/proxy-auth'):
                            if headers.get('authorization') != 'Bearer fixture-only':
                                raise AssertionError('explicit credential header was lost')
                            status, body = ('401', b'auth') if path == '/unauthorized' else ('407', b'proxy')
                            extra = [('www-authenticate' if status == '401' else 'proxy-authenticate', 'Basic realm="fixture"')]
                        elif path == '/gzip':
                            body, extra = gzip.compress(BINARY, mtime=0), [('content-encoding', 'gzip')]
                        elif path == '/timeout':
                            time.sleep(0.3)
                        elif path in ('/reset', '/reset-immediate'):
                            connection.send_headers(event.stream_id, [(':status', '200'), ('content-length', '10')])
                            connection.send_data(event.stream_id, b'abc')
                            if path == '/reset':
                                # ACK separates the headers/data from the reset
                                # without a scheduling-dependent sleep.
                                token = b'rstguard'
                                reset_wait[token] = event.stream_id
                                connection.ping(token)
                            else:
                                connection.reset_stream(event.stream_id, error_code=2)
                                with self.server.lock:
                                    self.server.resets += 1
                            continue
                        elif path != '/binary':
                            raise AssertionError('unknown fixture path: ' + path)
                        fields = [(':status', status), ('x-protocol', 'h2')] + extra
                        if status != '204' and path != '/trailers':
                            fields.append(('content-length', str(len(body))))
                        connection.send_headers(event.stream_id, fields, end_stream=not body and not trailers)
                        if body or trailers:
                            pending[event.stream_id] = [body, 0, trailers]
                for stream, (body, offset, trailers) in list(pending.items()):
                    while offset < len(body):
                        amount = min(connection.local_flow_control_window(stream),
                                     connection.max_outbound_frame_size, len(body) - offset)
                        if amount <= 0:
                            break
                        end = offset + amount
                        connection.send_data(stream, body[offset:end], end_stream=end == len(body) and not trailers)
                        offset = end
                    if offset == len(body):
                        if trailers:
                            connection.send_headers(stream, trailers, end_stream=True)
                        del pending[stream]
                    else:
                        pending[stream][1] = offset
                self.request.sendall(connection.data_to_send())
        except (OSError, h2.exceptions.H2Error):
            # Client-side timeouts, bounded-output aborts and resets close TLS.
            pass
        except Exception as error:
            with self.server.lock:
                self.server.errors.append(str(error))


class TLSServer(Server):
    daemon_threads = False

    def __init__(self, context, destination=False):
        self.context = context
        self.destination = destination
        self.lock = threading.Lock()
        self.paths = collections.Counter()
        self.errors = []
        self.resets = 0
        self.reset_acks = 0
        self.redirect_origin = ''
        super().__init__(('127.0.0.1', 0), Handler)

    def get_request(self):
        sock, address = super().get_request()
        sock.settimeout(3)
        try:
            return self.context.wrap_socket(sock, server_side=True), address
        except Exception:
            sock.close()
            raise


@contextlib.contextmanager
def fixtures():
    with certificate() as root:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(root / 'cert.pem', root / 'key.pem')
        context.set_alpn_protocols(['h2'])
        with TLSServer(context) as source, TLSServer(context, destination=True) as destination:
            source.redirect_origin = f'https://localhost:{destination.server_address[1]}'
            threads = [threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.05})
                       for server in (source, destination)]
            for thread in threads:
                thread.start()
            try:
                yield (f'https://localhost:{source.server_address[1]}', str(root / 'cert.pem'),
                       source.redirect_origin)
            finally:
                source.shutdown()
                destination.shutdown()
                for thread in threads:
                    thread.join(timeout=5)
        # server_close joins every handler before checking assertions, including
        # delayed timeout handlers that may finish after serve_forever stops.
        if source.errors or destination.errors:
            raise AssertionError(source.errors + destination.errors)
        if destination.paths:
            raise AssertionError('redirect target received a request')
        if source.paths['/redirect'] != 1 or source.paths['/unauthorized'] != 1 or source.paths['/proxy-auth'] != 1:
            raise AssertionError('redirect/auth request was skipped or retried: ' + repr(source.paths))
        if source.paths['/reset'] != 1 or source.paths['/reset-immediate'] != 1 or source.resets != 2 or source.reset_acks != 1:
            raise AssertionError('reset did not reach HTTP/2 response headers and RST_STREAM')
        if source.paths['/timeout'] != 1:
            raise AssertionError('timeout did not reach the HTTP/2 response phase')


def main():
    if len(sys.argv) < 2:
        raise SystemExit('usage: http2.py COMMAND [ARG ...]')
    with fixtures() as arguments:
        subprocess.run([*sys.argv[1:], *arguments], check=True, timeout=60, env=hostile_environment())
    print(f'HTTP/2 fixture passed (hyper-h2 {h2.__version__}; no external network)')


if __name__ == '__main__':
    main()
