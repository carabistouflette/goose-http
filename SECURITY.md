# Security

Report suspected vulnerabilities through [GitHub private vulnerability reporting](https://github.com/carabistouflette/goose-http/security/advisories/new). Include the affected revision, Goose/libcurl/TLS versions, platform and a small reproducer.

The native adapter relies on maintained system libcurl for HTTP and TLS. Certificate and hostname verification stay enabled. Keep libcurl and its TLS dependencies updated.

This client is not an SSRF boundary. Applications must authorize untrusted URLs and decide whether to follow redirects. Private and local addresses are allowed. Redirects, environment proxies, netrc and cookies are disabled by default.

Response and header limits bound library buffers, not all memory used by libcurl or TLS. A transfer timeout is not a hard call deadline: threaded DNS cleanup can wait for the system resolver. Header callback bytes may not match wire bytes, and some libcurl versions omit HTTP/2 trailers. Validate all fields needed for an application decision.

See the [API contract](docs/API.md) for limits, DNS cleanup and signal notes. Test coverage does not prove the system libraries are free from defects.
