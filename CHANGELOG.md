# Changelog

## Unreleased

- Add an optional origin-bound HTTPS GET client for repeated buffered requests.
- Add a pull-based streaming API for bounded response chunks.
- Keep request, response and timeout limits explicit; TLS verification remains enabled.

## 0.1.0 — 2026-11-04

First public release of the synchronous HTTP client. The API is experimental before 1.0. Linux x86-64 is the validated platform.

This release has one-shot GET and binary POST, caller-owned response buffers, body/header limits, transfer timeouts and verified system TLS. HTTP requires explicit opt-in. It does not follow redirects, use environment proxies, store cookies, retry requests, or decode compressed bodies.

The fixed-origin source-metadata example is an application example; it does not authenticate executable downloads or install software.

See the [API contract](docs/API.md), [security policy](SECURITY.md) and [third-party notices](THIRD_PARTY_NOTICES.md).
