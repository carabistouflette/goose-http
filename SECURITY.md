# Security

Report suspected vulnerabilities through
[GitHub private vulnerability reporting](https://github.com/carabistouflette/goose-http/security/advisories/new).
Include the affected commit, Goose/libcurl/TLS versions, platform and a minimal
reproducer. Keep exploit details out of public issues until a fix is available.

The client validates URLs, headers and numeric bounds, disables environment
proxies and automatic redirects, and preserves libcurl certificate/hostname
verification. TLS and HTTP parsing are delegated to a maintained system libcurl.
Update system native dependencies for their security fixes; they are not vendored.

This is not an SSRF boundary: callers must authorize destinations, including
resolved private/local addresses, when URLs are untrusted. The API permits local
network access. Applications own redirect, credential and signal policy. Response
buffers are empty on any error. Fixed input/output caps bound library work and
storage but are not an exact allocator or native TLS memory budget. The timeout does not bound call wall time: libcurl's threaded DNS cleanup can
wait for a system resolver beyond the requested transfer timeout. Asynchronous
DNS support alone does not guarantee cancellation. Do not rely on this API for
a hard cancellation deadline. See the contract for resolver/build choices. See [the contract](docs/API.md).

CI exercises Goose run/release/debug, ownership rejection, local HTTP/1 and HTTP/2 TLS,
stalled-DNS cleanup, incomplete final headers, startup/runtime and allocation fault injection, GCC/Clang AddressSanitizer/UndefinedBehaviorSanitizer and a
network-disabled libFuzzer harness. These checks improve coverage; they do not
prove arbitrary server responses safe independently of libcurl's own guarantees.

Header output is the installed libcurl's callback data, not proof of complete
wire metadata. Older backends can omit HTTP/2 trailers on known-length bodies.
Callers relying on a digest or other security-relevant field must reject its
absence and verify its value; transport success alone is insufficient. See the
[API contract](docs/API.md) and [protocol coverage](tests/README.md).
