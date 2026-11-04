#!/usr/bin/env python3
"""Exercise the compiled reference consumer over real loopback HTTPS, without GitHub."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import contextlib
import gzip
import json
from pathlib import Path
import socketserver
import ssl
import subprocess
import threading
import time

from integration import Server, certificate, hostile_environment, response

REVISION = "0123456789abcdef0123456789abcdef01234567"
PATH = f"/repos/aardappel/goose/commits/{REVISION}".encode()
URL = 'https://example.invalid/"<script>&?x=\\\t\x00'
MESSAGE = 'subject "<script>\\\t\x00\x1b\u2028\nignored second line'
SENTINEL = "FIXTURE_UNTRUSTED_DO_NOT_LOG"
BODY_LIMIT = 262144
HEADER_LIMIT = 32768


def document(**changes):
    result = {"sha": REVISION, "html_url": URL, "commit": {"message": MESSAGE}}
    result.update(changes)
    return json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode()


def sized_headers(body, size):
    prefix = (b"HTTP/1.1 200 OK\r\nContent-Length: " + str(len(body)).encode()
              + b"\r\nConnection: close\r\nX-Padding: ")
    padding = size - len(prefix) - 4
    assert padding >= 0
    return prefix + b"x" * padding + b"\r\n\r\n" + body


def cases():
    good = document()
    values = [("success", response(good), True),
              ("identity", response(good, extra=b"Content-Encoding: identity\r\n"), True),
              ("identity-case-ows", response(good, extra=b"cOnTeNt-EnCoDiNg:\tIDENTITY \t\r\n"), True),
              ("body-at-limit", response(good + b" " * (BODY_LIMIT - len(good))), True),
              ("headers-at-limit", sized_headers(good, HEADER_LIMIT), True),
              ("body-over-limit", response(good + b" " * (BODY_LIMIT + 1 - len(good))), False),
              ("headers-over-limit", sized_headers(good, HEADER_LIMIT + 1), False),
              ("wrong-sha", response(document(sha="f" * 40)), False),
              ("uppercase-sha", response(document(sha=REVISION.upper())), False),
              ("wrong-sha-type", response(document(sha=42)), False),
              ("wrong-url-type", response(document(html_url=None)), False),
              ("empty-url", response(document(html_url="")), False),
              ("wrong-commit-type", response(document(commit=[])), False),
              ("wrong-message-type", response(document(commit={"message": []})), False),
              ("missing-message", response(document(commit={})), False),
              ("empty-message", response(document(commit={"message": ""})), True),
              ("cr-subject", response(document(commit={"message": "first\rsecond"})), True),
              ("invalid-utf8", response(good[:-1] + b',"other":"\xff"}'), False),
              ("lone-high-surrogate", response(good[:-1] + b',"other":"\\ud800"}'), False),
              ("lone-low-surrogate", response(good[:-1] + b',"other":"\\udfff"}'), False),
              ("malformed", response(good[:-1]), False),
              ("trailing-data", response(good + b" {}"), False),
              ("root-array", response(b"[" + good + b"]"), False),
              ("too-deep", response(good[:-1] + b',"extra":' + b"[" * 33 + b"0" + b"]" * 33 + b"}"), False),
              ("long-number", response(good[:-1] + b',"extra":1' + b"0" * 128 + b"}"), False),
              ("gzip", response(gzip.compress(good, mtime=0), extra=b"Content-Encoding: gzip\r\n"), False),
              ("encoding-duplicate", response(good, extra=b"Content-Encoding: identity\r\nContent-Encoding: identity\r\n"), False),
              ("encoding-folded", response(good, extra=b"Content-Encoding: identity\r\n identity\r\n"), False),
              ("encoding-list", response(good, extra=b"Content-Encoding: identity, identity\r\n"), False)]
    for key in ("sha", "html_url", "commit"):
        obj = json.loads(good)
        del obj[key]
        values.append((f"missing-{key}", response(json.dumps(obj).encode()), False))
        original = json.dumps(json.loads(good)[key], ensure_ascii=False).encode()
        extra = b',"' + key.encode() + b'":' + original + b"}"
        values.append((f"duplicate-{key}", response(good[:-1] + extra), False))
        escaped = "\\u%04x%s" % (ord(key[0]), key[1:])
        extra = b',"' + escaped.encode() + b'":' + original + b"}"
        values.append((f"escaped-duplicate-{key}", response(good[:-1] + extra), False))
    values.append(("duplicate-message", response(document(commit={"message": "x"}).replace(
        b'"message":"x"', b'"message":"x","message":"y"')), False))
    values.append(("escaped-duplicate-message", response(document(commit={"message": "x"}).replace(
        b'"message":"x"', b'"message":"x","\\u006dessage":"y"')), False))
    for status in (201, 403, 404, 429):
        values.append((f"status-{status}", response(SENTINEL.encode(), f"{status} Fixture".encode()), False))
    values.append(("redirect", response(SENTINEL.encode(), b"302 Found", b"Location: /redirect-target\r\n"), False))
    return values


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(8)
        try:
            first = self.rfile.readline(16384)
            if not first:
                return
            headers = []
            for _ in range(100):
                line = self.rfile.readline(65536)
                if line == b"\r\n":
                    break
                if b":" not in line:
                    raise AssertionError(f"bad request header {line!r}")
                key, value = line.rstrip(b"\r\n").split(b":", 1)
                headers.append((key.lower(), value.strip()))
            else:
                raise AssertionError("too many request headers")
            self.server.requests.append((first, headers))
            assert first == b"GET " + PATH + b" HTTP/1.1\r\n", first
            expected = {b"user-agent": b"honk-toolchain-metadata/0.1",
                        b"accept": b"application/vnd.github+json", b"accept-encoding": b"identity"}
            assert len(headers) == 4, headers  # The three application fields plus libcurl's Host.
            for key, value in expected.items():
                assert [v for k, v in headers if k == key] == [value], headers
            assert [v for k, v in headers if k == b"host"] == [
                f"localhost:{self.server.server_address[1]}".encode()], headers
            assert not any(k in (b"authorization", b"cookie", b"proxy-authorization") for k, _ in headers)
            if self.server.delay:
                time.sleep(self.server.delay)
            self.wfile.write(self.server.reply)
        except AssertionError as error:
            self.server.errors.append(str(error))
        except (BrokenPipeError, ConnectionResetError, TimeoutError, ssl.SSLError):
            pass


class MetadataServer(Server):
    daemon_threads = False

    def __init__(self, context):
        self.context = context
        super().__init__(("127.0.0.1", 0), Handler)

    def get_request(self):
        sock, address = super().get_request()
        sock.settimeout(8)
        try:
            return self.context.wrap_socket(sock, server_side=True), address
        except Exception:
            sock.close()
            raise


@contextlib.contextmanager
def fixture():
    with certificate() as root:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(root / "cert.pem", root / "key.pem")
        with MetadataServer(context) as server:
            server.requests, server.errors, server.delay = [], [], 0
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
            thread.start()
            environment = hostile_environment()
            environment.update(GOOSE_METADATA_FIXTURE_REVISION=REVISION,
                               GOOSE_METADATA_FIXTURE_ORIGIN=f"https://localhost:{server.server_address[1]}",
                               GOOSE_METADATA_FIXTURE_CA=str(root / "cert.pem"),
                               GOOSE_METADATA_FIXTURE_TRUST="trusted")
            try:
                yield server, environment
            finally:
                server.shutdown()
                thread.join(timeout=5)
        assert not server.errors, server.errors


def invoke(binary, environment, revision=REVISION):
    return subprocess.run([binary, revision], env=environment, capture_output=True, timeout=12)


def failure(result, name):
    assert result.returncode == 1, (name, result.returncode, result.stdout, result.stderr)
    assert result.stdout == b"", (name, result.stdout)
    assert result.stderr.startswith(b"source metadata: "), (name, result.stderr)
    assert SENTINEL.encode() not in result.stderr, (name, result.stderr)
    assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr, result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    arguments = parser.parse_args()
    binary = str(Path(arguments.binary).resolve())
    count = 0
    with fixture() as (server, environment):
        for name, reply, success in cases():
            server.reply = reply
            before = len(server.requests)
            result = invoke(binary, environment)
            assert not server.errors, (name, server.errors)
            assert len(server.requests) == before + 1, (name, server.requests[before:])
            if success:
                assert result.returncode == 0 and result.stderr == b"", (name, result.stderr)
                rendered = json.loads(result.stdout)
                subject = "" if name == "empty-message" else "first" if name == "cr-subject" else MESSAGE.split("\n")[0]
                assert rendered == {"requested_revision": REVISION, "sha": REVISION,
                                    "html_url": URL, "subject": subject}, (name, result.stdout)
                assert not any(c < 32 for c in result.stdout.rstrip(b"\n")), (name, result.stdout)
            else:
                failure(result, name)
            count += 1
        for revision in ("", "a" * 39, "a" * 41, "g" * 40, REVISION.upper(), "../" + "a" * 37,
                         "https://example.invalid/" + "a" * 40):
            before = len(server.requests)
            failure(invoke(binary, environment, revision), "invalid revision")
            assert len(server.requests) == before, "invalid revision reached transport"
            count += 1
        for trust in ("untrusted", "wrong-host"):
            before = len(server.requests)
            failure(invoke(binary, {**environment, "GOOSE_METADATA_FIXTURE_TRUST": trust}), trust)
            assert len(server.requests) == before, "TLS verification allowed the request"
            count += 1
        server.reply, server.delay = response(document()), 6
        before = len(server.requests)
        failure(invoke(binary, environment), "timeout")
        assert len(server.requests) == before + 1, "timeout retried"
        count += 1
        # Missing fixture configuration must fail before libcurl can contact GitHub.
        result = invoke(binary, {k: v for k, v in environment.items()
                                 if not k.startswith("GOOSE_METADATA_FIXTURE_")})
        assert result.returncode == 90 and b"missing fixture environment" in result.stderr, result
        count += 1
        usage = subprocess.run([binary], env=environment, capture_output=True, timeout=12)
        assert usage.returncode == 2 and usage.stdout == b"" and usage.stderr.startswith(b"usage:"), usage
        count += 1
    print(f"{count} compiled metadata consumer HTTPS cases passed")


if __name__ == "__main__":
    main()
