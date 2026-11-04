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
storage but are not an exact allocator or native TLS memory budget. The timeout
is not a hard real-time scheduling guarantee. See [the contract](docs/API.md).

CI exercises Goose run/release/debug, ownership rejection, local HTTP/TLS,
fault injection, GCC/Clang AddressSanitizer/UndefinedBehaviorSanitizer and a
network-disabled libFuzzer harness. These checks improve coverage; they do not
prove arbitrary server responses safe independently of libcurl's own guarantees.
