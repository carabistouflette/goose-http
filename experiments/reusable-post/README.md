# Reusable POST Replay Experiment (`libcurl` Probe)

## Overview & Motivation

This experiment investigates `libcurl` HTTP/1.1 connection reuse behavior for `POST` requests and evaluates callback guard mechanisms (`CURLOPT_PREREQFUNCTION`) to prevent unintended request replays upon connection drops.

In HTTP/1.1, connection reuse (keep-alive) allows subsequent requests on the same connection. However, if a server or intermediate proxy closes or resets a reused connection during or immediately after request dispatch, client libraries may attempt an automatic reconnect and re-issue the request. Because `POST` requests are typically non-idempotent (e.g., JSON-RPC state transitions or financial transactions), silent automatic replay can lead to duplicate executions.

This experiment measures:
1. Whether default `libcurl` connection reuse triggers request replays after connection termination.
2. Whether a prereq callback guard (`CURLOPT_PREREQFUNCTION`) reliably halts replay attempts.
3. How `goose-http`'s current one-shot connection model behaves as a baseline.

## Methodology

The test harness consists of three components:
- [probe.c](probe.c): Direct C probe interacting with `libcurl` easy handles. Configures sequential requests on a single connection handle with either:
  - `unguarded`: Standard libcurl easy transfer.
  - `guarded`: Registers a `CURLOPT_PREREQFUNCTION` callback that tracks invocations per transfer and returns `CURL_PREREQFUNC_ABORT` if invoked more than once for a single transfer (i.e. on reconnection/replay).
- [one_shot.goose](one_shot.goose): Baseline consumer using `goose-http`'s existing public `http::request` API.
- [run.py](run.py): Test orchestrator managing TLS loopback servers (`localhost:0`), self-signed certificates, and drop injection:
  - `after_headers`: Drops connection after reading request headers (before body read).
  - `after_body`: Drops connection after reading full request body (before response).
  - `partial_response`: Sends incomplete response bytes then drops connection.
  - Multiple payload types: standard JSON-RPC, empty body, and binary payload containing null bytes (`A\x00B`).
  - Validation controls: Untrusted CA rejection (`CURLE_PEER_FAILED_VERIFICATION`, code 60) and malformed URL rejection.

## Observed Results

Environment: Linux 7.2.11-arch1-1 x86_64, `libcurl` 8.23.0 (OpenSSL/3.6.5), GCC 16.2.1.
Full recorded artifact: `.build/reusable-post-observation.json`.

| Failure Scenario | Payload | Unguarded Behavior | Guarded Behavior (`CURLOPT_PREREQFUNCTION`) |
| :--- | :--- | :--- | :--- |
| `none` (clean keep-alive) | JSON | 2 requests / 1 TCP conn, 200 OK | 2 requests / 1 TCP conn, 200 OK (guard not triggered) |
| `after_body` | JSON | **Replayed**: 3 server arrivals, code 0 (200 OK) | **Replay Aborted**: 2 server arrivals, code 42 (`CURLE_ABORTED_BY_CALLBACK`) |
| `after_body` | Empty | **Replayed**: 3 server arrivals, code 0 (200 OK) | **Replay Aborted**: 2 server arrivals, code 42 (`CURLE_ABORTED_BY_CALLBACK`) |
| `after_body` | Binary | **Replayed**: 3 server arrivals, code 0 (200 OK) | **Replay Aborted**: 2 server arrivals, code 42 (`CURLE_ABORTED_BY_CALLBACK`) |
| `after_headers` | JSON | **Replayed**: 3 server arrivals, code 0 (200 OK) | **Replay Aborted**: 2 server arrivals, code 42 (`CURLE_ABORTED_BY_CALLBACK`) |
| `partial_response` | JSON | **No replay**: code 56 (`CURLE_RECV_ERROR`) | **No replay**: code 56 (`CURLE_RECV_ERROR`, guard not exercised) |

### Key Observations:
- **Unguarded Replay Risk:** On connection teardowns occurring after headers or body transmission, `libcurl` automatically established a second TCP connection and re-sent the second `POST` request. The server observed duplicate receipt of request #2.
- **Guarded Effectiveness:** The `CURLOPT_PREREQFUNCTION` callback executed before the second connection transfer began. Returning `CURL_PREREQFUNC_ABORT` prevented any bytes from being dispatched over the new connection, cleanly terminating the transfer with error code 42 (`CURLE_ABORTED_BY_CALLBACK`).
- **Partial Responses:** When the server closed connection after sending initial response bytes, `libcurl` did not attempt replay and surfaced `CURLE_RECV_ERROR` directly.
- **One-Shot Baseline:** `goose-http::request` completed two sequential requests with 2 separate TCP accepts (`tcp_accepts: 2`), confirming that the current one-shot architecture is naturally immune to connection reuse replay risks, albeit without keep-alive performance benefits.

## Boundaries & Scope

- **Isolated Research Probe:** This experiment is isolated under `experiments/` and is **not** part of the `goose-http` library core code or runtime distribution.
- **Specific Environment:** Observations reflect Linux x86_64 loopback TLS with `libcurl` 8.23.0 and OpenSSL 3.6.5.
- **Production Bounds:** These findings do not guarantee identical retry behaviors across older/newer libcurl versions, alternative TLS backends (e.g. GnuTLS, Schannel, SecureTransport), intermediate HTTP/1.1 proxies, or HTTP/2 and HTTP/3 multiplexing layers.
- **Implication for Future Design:** Should `goose-http` introduce connection pooling or persistent HTTP connections in the future, connection reuse for `POST` requests must either be disabled or strictly guarded (via prereq hooks or request idempotency policies) to preserve safety guarantees.
