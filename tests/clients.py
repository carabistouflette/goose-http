#!/usr/bin/env python3
"""Verify reusable clients against independently counted persistent TLS sockets."""
# SPDX-License-Identifier: Apache-2.0
import hashlib
import json
from pathlib import Path
import socketserver
import ssl
import subprocess
import sys
import threading
import time

from integration import certificate, hostile_environment

if not __debug__:
    raise RuntimeError("client fixture requires Python assertions")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 32

    def __init__(self, address, context):
        super().__init__(address, Handler)
        self.context = context
        self.lock = threading.Lock()
        self.accepted = 0
        self.connections = {}
        self.requests = []
        self.errors = []

    def get_request(self):
        connection, address = super().get_request()
        with self.lock:
            self.accepted += 1
            connection_id = self.accepted
        try:
            connection = self.context.wrap_socket(connection, server_side=True)
        except BaseException:
            connection.close()
            raise
        with self.lock:
            self.connections[connection] = connection_id
        return connection, address


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(5)
        with self.server.lock:
            connection = self.server.connections[self.connection]
        try:
            while first := self.rfile.readline(16384):
                method, path, version = first.rstrip(b"\r\n").split(b" ")
                headers = {}
                for _ in range(100):
                    line = self.rfile.readline(65536)
                    if line == b"\r\n":
                        break
                    key, value = line.rstrip(b"\r\n").split(b":", 1)
                    headers[key.lower()] = value.strip()
                else:
                    raise AssertionError("unbounded request headers")
                assert method == b"GET" and version == b"HTTP/1.1"
                assert headers.get(b"content-length", b"0") == b"0"
                assert b"cookie" not in headers and b"proxy-authorization" not in headers
                if path == b"/auth":
                    assert headers.get(b"authorization") == b"Bearer private"
                    assert headers.get(b"x-custom") == b"once"
                else:
                    assert b"authorization" not in headers and b"x-custom" not in headers
                assert path not in (b"/forbidden", b"/redirect-target", b"/untrusted")
                with self.server.lock:
                    self.server.requests.append((connection, path.decode()))
                body, status, extra = b"ok", b"200 OK", b""
                if path == b"/big":
                    body = b"x" * 2048
                elif path == b"/headers":
                    extra = b"X-Large: " + b"x" * 2048 + b"\r\n"
                elif path == b"/status":
                    status = b"404 Not Found"
                elif path == b"/redirect":
                    status, extra = b"302 Found", b"Location: /redirect-target\r\n"
                elif path == b"/slow":
                    time.sleep(0.15)
                elif path == b"/partial":
                    self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort")
                    return
                data = (b"HTTP/1.1 " + status + b"\r\nContent-Length: " + str(len(body)).encode()
                        + b"\r\nX-Connection-Id: " + str(connection).encode()
                        + b"\r\nSet-Cookie: private=must-not-persist\r\n" + extra + b"\r\n" + body)
                self.wfile.write(data)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, TimeoutError, ssl.SSLError):
            pass
        except Exception as error:
            with self.server.lock:
                self.server.errors.append(repr(error))
        finally:
            with self.server.lock:
                self.server.connections.pop(self.connection, None)


def hostname_negative(command, root):
    """Trust the issuing certificate while rejecting its non-localhost SAN."""
    cert, key = root / "wrong-cert.pem", root / "wrong-key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-subj", "/CN=wrong.invalid", "-addext", "subjectAltName=DNS:wrong.invalid",
                    "-keyout", str(key), "-out", str(cert)], check=True, timeout=30,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    with Server(("127.0.0.1", 0), context) as server:
        worker = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02})
        worker.start()
        try:
            origin = f"https://localhost:{server.server_address[1]}"
            result = subprocess.run([*command, origin, str(cert), "--wrong-hostname"],
                                    env=hostile_environment(), capture_output=True, text=True, timeout=10)
            assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
            assert json.loads(result.stdout) == {"hostname_verification": "passed"}
            with server.lock:
                assert not server.requests and not server.errors, (server.requests, server.errors)
        finally:
            server.shutdown()
            worker.join(timeout=5)


def main():
    command = sys.argv[1:]
    api = bool(command and command[0] == "--api")
    if api:
        command = command[1:]
    if not command:
        raise SystemExit("usage: clients.py COMMAND [ARG ...]")
    with certificate() as root:
        hostname_negative(command, root)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(root / "cert.pem", root / "key.pem")
        context.set_alpn_protocols(["http/1.1"])
        with Server(("127.0.0.1", 0), context) as server:
            worker = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02})
            worker.start()
            try:
                origin = f"https://localhost:{server.server_address[1]}"
                env = hostile_environment()
                (root / ".netrc").write_text("machine localhost login unwanted password secret\n")
                env["HOME"] = str(root)
                result = subprocess.run([*command, origin, str(root / "cert.pem")],
                                        env=env, capture_output=True, text=True, timeout=30)
                if result.returncode:
                    raise AssertionError(f"client failed ({result.returncode})\n{result.stdout}{result.stderr}")
                native = json.loads(result.stdout)
                assert native["public_clients" if api else "native_clients"] == "passed", native
                with server.lock:
                    requests = list(server.requests)
                    accepted, errors = server.accepted, list(server.errors)
                assert not errors, errors
                initial = ["/id", "/auth", "/clean"] if api else ["/id", "/auth", "/clean", "/status", "/redirect", "/id", "/after-invalid"]
                assert [path for _, path in requests[:len(initial)]] == initial, requests
                assert len({connection for connection, _ in requests[:len(initial)]}) == 1, requests
                assert len(requests) == (3 if api else 18), requests
                # Count raw TCP accepts, including the connection rejected by
                # the client trust check before it writes an HTTP request.
                assert accepted == (2 if api else 7), (accepted, requests)
                assert len({connection for connection, _ in requests}) == (1 if api else 6), requests
                for index, failed in enumerate(() if api else ("/big", "/headers", "/slow", "/partial")):
                    position = 7 + index * 2
                    assert requests[position][1] == failed and requests[position + 1][1] == "/recovered"
                    assert requests[position][0] != requests[position + 1][0], requests
                assert api or [path for _, path in requests[15:]] == ["/parallel", "/hold", "/after-busy"], requests
                assert api or requests[16][0] == requests[17][0] != requests[15][0], requests
                source_root = Path(__file__).resolve().parents[1]
                fingerprints = {str(path.relative_to(source_root)): hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in [source_root / name for name in ("native/http.c", "native/client.c", "http.goose", "tests/clients.py", *( ("tests/client_api.goose", "tests/client_transport.c") if api else ("tests/clients.c",) ))]}
                print(json.dumps({"native": native, "accepted_tls_connections": accepted,
                                  "requests": len(requests), "initial_requests_per_connection": len(initial),
                                  "hostname_verification": "passed",
                                  "source_sha256": fingerprints}, sort_keys=True))
            finally:
                server.shutdown()
                worker.join(timeout=5)


if __name__ == "__main__":
    main()
