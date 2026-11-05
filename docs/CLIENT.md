# Reusable HTTPS GET client

The optional client reuses a private HTTP/1.1 connection for sequential buffered GET requests to one origin. It is on current main (`f81d12197c26f48ab61562f056aba0a1a19f0ff6`), after release `v0.1.0`. It does not change the one-shot API. See [the runnable example](../examples/client.goose).

```goose
var client = http::ClientHandle {};
let opened = http::open_client("https://example.com", client);
if opened.error == http::Error.None {
    defer { assert(http::close_client(client).error == http::Error.None); }
    let options = http::Options { body_limit: 65536, header_limit: 16384 };
    var body: u8[>..] = [];
    var headers: u8[>..] = [];
    let status = http::request(client, "https://example.com/item", options, body, headers);
}
```

## Contract

`open_client` accepts a lowercase `https://` origin with an authority and optional final slash. It does not connect or resolve DNS. Paths other than `/`, queries, fragments, userinfo (including an empty `@`), IPv6 zone IDs and ports outside 1–65535 are rejected. The URL limit is 8 KiB. The default HTTPS port is 443.

Each request must be HTTPS and parse to the same host, case-insensitively, and effective numeric port as the configured origin. Fragments, userinfo and IPv6 zone IDs are rejected. Accepted request bytes go unchanged to libcurl. Host spelling follows libcurl's parser; this is not DNS or address authorization. Apply any application URL policy before calling the client.

The client accepts GET only. It adds no retry loop, but libcurl may reconnect and replay a GET on a stale reused connection. Use it only when the request is safe to repeat; a transfer failure does not show whether the server received the request. POST remains on the one-shot API.

Use one sequential owner for a handle; different handles can be used concurrently. Copies alias the same resource. Up to 32 clients can be open in a process. Opening into a nonzero handle fails. Successful close clears the handle; closing zero succeeds. A busy close fails and leaves the live handle available to close after its request finishes. One client must stay within one authentication and trust context: connection and TLS-session state can be reused, and TLS verification is not repeated for every request on an established connection.

The client disables proxies, netrc, redirects, cookies, shared handles, HSTS, Alt-Svc and HTTP authentication negotiation. TLS peer and host verification remain on. Headers and callback pointers are cleared between calls. Invalid input keeps an idle connection; setup, transfer, timeout and callback-limit failures discard it. Complete 3xx/4xx/5xx responses are transport successes; check the HTTP status. Outputs are empty on any error.

Timeouts are transfer limits, not hard wall-clock deadlines. Threaded DNS cleanup can continue past the timeout. The API has no asynchronous cancellation or streaming reuse.

## Check and measurement

`make clients CC=gcc` and `CC=clang` exercise the client against local verified TLS. `make clients-sanitize SANITIZER_CC=gcc` and the Clang equivalent instrument the generated program and native adapter; system libcurl/TLS are not instrumented.

The [reuse benchmark report](../bench/REUSE_RESULTS.md) describes the workload and the exact source bridge to current main. It measures loopback HTTPS only; it is not a WAN or application-latency prediction.
