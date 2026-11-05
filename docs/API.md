# HTTP API and limits

The current main revision is `f81d12197c26f48ab61562f056aba0a1a19f0ff6`. Release `v0.1.0` at `bb30d4c893efbf41fc7306d541c332074aa9ea35` has the one-shot API; the client and streaming APIs below are on main only.

## Buffered requests

`request(url, options, body, headers)` sends GET by default. GET rejects a nonempty request body. Set `Options.method` to `Post` and `Options.body` to send binary or empty data. Pass a named `Options` value. `Status` reports the library error, libcurl transport code and HTTP status code; the HTTP status may be available after a transfer error.

`Error` is `None`, `InvalidInput`, `SetupFailed`, `Transport`, `TimedOut`, `BodyLimit` or `HeaderLimit`. `success(status)` is true only for `Error.None` and an HTTP status from 200 through 299.

| Option | Default | Allowed values |
| --- | ---: | --- |
| `timeout_ms` | 30,000 ms | 1–86,400,000 ms |
| `body_limit` | 1 MiB | 0–16 MiB |
| `header_limit` | 64 KiB | 0–256 KiB |
| `allow_http` | false | Set true to allow `http://` |

URLs are limited to 8 KiB. A request can have up to 64 headers and 64 KiB combined across all names, values and four separator bytes per field. Upload bodies are limited to 1 MiB. URLs must be printable ASCII, use lowercase `https://` (or opted-in `http://`), and contain no userinfo or fragment. Header names must be valid tokens; values must be printable ASCII or tab. The library rejects `Host`, `Content-Length`, `Transfer-Encoding`, `Connection`, `Proxy-Connection`, `TE`, `Trailer`, `Upgrade` and `Expect`. `Accept-Encoding` must be exactly `identity` (ASCII case-insensitive, with no surrounding whitespace, list or parameter). Empty or whitespace-only header values send an empty field.

The output buffers must be separate and must not overlap inputs. They are cleared on any error. A completed HTTP 4xx or 5xx response has `Error.None`; use `success(status)` to require status 200–299. A body-limit or header-limit error does not return a usable partial response. A successful transfer also requires a complete final response-header section.

One-shot calls create a new libcurl handle; they do not reuse connections. Response body bytes have transfer framing removed, but are not decompressed. Header output is the bytes libcurl supplies to its callback, including informational responses and trailers. These bytes may not match wire bytes. libcurl 8.5 can omit HTTP/2 trailers on known-length responses. Check the presence and value of every security-critical field.

## Transport and safety

Certificate and hostname verification use the system TLS trust store. The API has no insecure or custom-CA option. Redirects, environment proxies, netrc, cookies, authentication negotiation, content decoding and application-level retries are disabled. An explicit caller-supplied `Authorization` header is supported. The library does not write files or log requests.

This is not an SSRF boundary. Applications must authorize destinations and any redirect they choose to follow. Private and local addresses are allowed. A DNS name can resolve to a private address.

`timeout_ms` sets libcurl's transfer timeout; it does not bound total call time. With a threaded resolver, cleanup can wait for a system DNS call after the timeout. Use a cancellation-capable resolver or supervise the call in another process if a hard deadline is required. The library does not install signal handlers; some TLS backends may require an application SIGPIPE policy.

## Reusable HTTPS GET client (main only)

`open_client(origin, handle)`, `request(handle, url, options, body, headers)`, and `close_client(handle)` provide sequential requests to one HTTPS origin. See the [client contract](CLIENT.md) for origin matching, cache lifetime and GET replay behavior.

## Streaming transfer (main only)

- `open_stream(url, options, handle)` validates the request and opens a handle without network I/O.
- `read_chunk(handle, chunk, headers, step_timeout_ms)` returns `Chunk`, `IdleTimeout`, `Done`, `Error` or `Cancelled`. Only `Chunk` contains body bytes; other steps clear the chunk output.
- `close_stream(handle)` releases the transfer and clears the handle ID. Closing an empty handle succeeds. Call it after `Done` or `Error` to release the handle.

Defaults are 30 s transfer timeout, 10 s connect timeout, 64 KiB chunks, 64 MiB body limit and 64 KiB header limit. The transfer timeout is 1–86,400,000 ms; connect timeout is 1 ms through the transfer timeout; chunks are 1 byte–1 MiB; body limit is nonnegative; header limit is 0–256 KiB. Streams use the same URL and request-header rules as buffered calls. Up to 32 streams can be open per process. A handle has one sequential logical reader; separate handles can be used concurrently. `total_bytes` counts bytes delivered in chunks. `step_timeout_ms` must be nonnegative and only limits one call to `read_chunk`; it does not cancel the transfer. DNS and cleanup can still exceed the transfer timeout.

Streaming uses HTTP/1.1 and disables response decompression. Response headers have the same callback-byte caveats as buffered calls.

See [tests/README.md](../tests/README.md) for tested boundaries.
