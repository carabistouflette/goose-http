#!/usr/bin/env python3
"""Deterministic loopback HTTP/TLS fixtures; no external network or trust changes."""
# SPDX-License-Identifier: Apache-2.0
import contextlib
import gzip
import os
from pathlib import Path
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time

BINARY = b"goose\x00\xff\r\nbody"
GZIP = gzip.compress(BINARY, mtime=0)


def response(body, status=b"200 OK", extra=b""):
    return (b"HTTP/1.1 " + status + b"\r\nContent-Length: " + str(len(body)).encode()
            + b"\r\nConnection: close\r\n" + extra + b"\r\n" + body)


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(5)
        try:
            first = self.rfile.readline(16384)
            parts = first.split()
            if len(parts) != 3:
                return
            method, path, _ = parts
            headers = {}
            for _ in range(100):
                line = self.rfile.readline(131072)
                if line == b"\r\n":
                    break
                if b":" not in line:
                    return
                key, value = line.rstrip(b"\r\n").split(b":", 1)
                headers[key.lower()] = value.strip()
            else:
                return
            length = int(headers.get(b"content-length", b"0"))
            if not 0 <= length <= 1048576:
                return
            upload = self.rfile.read(length)
            if path == b"/binary":
                data = response(BINARY)
            elif path == b"/empty":
                data = response(b"")
            elif path == b"/echo":
                data = response(upload if method == b"POST" else b"wrong method")
            elif path == b"/empty-header":
                data = response(b"yes" if headers.get(b"x-empty") == b"" else b"no")
            elif path == b"/identity-encoding":
                data = response(b"yes" if headers.get(b"accept-encoding", b"").lower() == b"identity" else b"no")
            elif path == b"/status":
                data = response(b"missing", b"404 Not Found")
            elif path == b"/redirect":
                data = response(b"redirect", b"302 Found", b"Location: /binary\r\n")
            elif path == b"/partial":
                data = b"HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nabc"
            elif path == b"/truncated-status":
                data = b"HTTP/1.1 200 OK\r\n"
            elif path == b"/truncated-headers":
                data = b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\n"
            elif path == b"/truncated-after-informational":
                data = (b"HTTP/1.1 103 Early Hints\r\nLink: </hint>\r\n\r\n"
                        b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\n")
            elif path == b"/truncated-lowercase-status":
                data = (b"HTTP/1.1 103 Early Hints\r\n\r\n"
                        b"http/1.1 200 OK\r\nContent-Length: 1\r\n")
            elif path == b"/timeout":
                time.sleep(0.3)
                data = response(b"late")
            elif path == b"/trailers":
                data = (b"HTTP/1.1 103 Early Hints\r\nLink: </hint>\r\n\r\n"
                        b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                        b"Trailer: X-End\r\nConnection: close\r\n\r\n"
                        b"3\r\nabc\r\n0\r\nX-End: done\r\n\r\n")
            elif path == b"/gzip":
                data = response(GZIP, extra=b"Content-Encoding: gzip\r\n")
            elif path.startswith(b"/bytes/"):
                count = int(path[len(b"/bytes/"):])
                if not 0 <= count <= 16777217:
                    return
                data = response(b"x" * count)
            else:
                data = response(b"unknown", b"404 Not Found")
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, TimeoutError, ValueError, ssl.SSLError):
            pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 32


@contextlib.contextmanager
def certificate():
    """Create an ephemeral localhost-only certificate without changing system trust."""
    with tempfile.TemporaryDirectory(prefix="goose-http-test-") as folder:
        root = Path(folder)
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
            "-keyout", str(root / "key.pem"), "-out", str(root / "cert.pem"),
        ], check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        yield root


@contextlib.contextmanager
def fixtures(include_ca=False):
    """Yield HTTP and self-signed HTTPS origins, shutting down servers on failure."""
    with certificate() as root:
        with Server(("127.0.0.1", 0), Handler) as http, Server(("127.0.0.1", 0), Handler) as tls:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(root / "cert.pem", root / "key.pem")
            tls.socket = context.wrap_socket(tls.socket, server_side=True)
            threads = [threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
                       for server in (http, tls)]
            for thread in threads:
                thread.start()
            try:
                origins = (f"http://127.0.0.1:{http.server_address[1]}",
                           f"https://127.0.0.1:{tls.server_address[1]}")
                yield (*origins, str(root / "cert.pem")) if include_ca else origins
            finally:
                http.shutdown()
                tls.shutdown()
                for thread in threads:
                    thread.join(timeout=5)


def hostile_environment():
    """Invalid proxy settings must not affect direct loopback requests."""
    env = os.environ.copy()
    for key in ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"):
        env[key] = "http://127.0.0.1:1"
    env["no_proxy"] = env["NO_PROXY"] = ""
    return env


def main():
    command = sys.argv[1:]
    include_ca = bool(command and command[0] == "--with-ca")
    if include_ca:
        command = command[1:]
    if not command:
        raise SystemExit("usage: integration.py [--with-ca] COMMAND [ARG ...]")
    with fixtures(include_ca=include_ca) as arguments:
        subprocess.run([*command, *arguments],
                       check=True, timeout=60, env=hostile_environment())


if __name__ == "__main__":
    main()
