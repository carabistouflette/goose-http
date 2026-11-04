// SPDX-License-Identifier: Apache-2.0
// Reference consumer for Honk's opt-in source-metadata command, not core HTTP API.
namespace honk_metadata;
import http;
import text;
import std;
import json;

const MAX_BODY = 262144;
const MAX_HEADERS = 32768;
const MAX_DEPTH = 32;
const MAX_NUMBER_BYTES = 128;
const TIMEOUT_MS = 5000;
const ORIGIN = "https://api.github.com/repos/aardappel/goose/commits/";
struct Info {
    requested_revision: u8[] = [],
    sha:                u8[] = [],
    html_url:           u8[] = [],
    subject:            u8[] = [],
}
fn revision_valid(revision: const u8[:]) -> bool {
    if revision.len != 40 { return false; }
    for c in revision { if !(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') { return false; } }
    return true;
}
fn same_ascii(a: const u8[:], lower: const u8[:]) -> bool {
    if a.len != lower.len { return false; }
    for i in a.len {
        let c = if a[i] >= 'A' && a[i] <= 'Z' { a[i] + 32 } else { a[i] };
        if c != lower[i] { return false; }
    }
    return true;
}
fn strip_ows(value: const u8[:]) -> const u8[:] {
    var first = 0; var last = value.len;
    while first < last && (value[first] == ' ' || value[first] == '\t') { first++; }
    while last > first && (value[last - 1] == ' ' || value[last - 1] == '\t') { last--; }
    return value[first..last];
}
// Conservative fixed-endpoint policy: any advertised nonidentity encoding,
// duplicate encoding or observed folded line fails closed across all blocks.
fn identity_response(headers: const u8[:]) -> bool {
    if headers.len > MAX_HEADERS { return false; }
    var seen = false;
    var start = 0;
    for c, i in headers {
        if c == 0 { return false; }
        if c != '\n' { continue; }
        let end = if i > start && headers[i - 1] == '\r' { i - 1 } else { i };
        let line = headers[start..end]; start = i + 1;
        if line.len == 0 { continue; }
        if line[0] == ' ' || line[0] == '\t' { return false; }
        var colon = 0;
        while colon < line.len && line[colon] != ':' { colon++; }
        if colon == line.len { continue; } // libcurl's status lines have no colon.
        let name = strip_ows(line[..colon]);
        if same_ascii(name, "content-encoding") {
            if seen || name.len != colon || !same_ascii(strip_ows(line[colon + 1..]), "identity") { return false; }
            seen = true;
        }
    }
    return start == headers.len;
}
// Bound upstream parser work before constructing its iterative document tree.
// This is a resource preflight; json::parse still validates all JSON grammar.
fn preflight(body: u8[:]) -> bool {
    if body.len > MAX_BODY || !text::valid_utf8(body) { return false; }
    var at = 0; var depth = 0;
    while at < body.len {
        let c = body[at];
        if c == '"' {
            let end, _ = json::scan_string(body, at + 1);
            if end < 0 { return false; }
            at = end + 1;
        } else if c == '{' || c == '[' {
            depth++; if depth > MAX_DEPTH { return false; } at++;
        } else if c == '}' || c == ']' {
            depth--; if depth < 0 { return false; } at++;
        } else if c == '-' || (c >= '0' && c <= '9') {
            let start = at;
            while at < body.len {
                let n = body[at];
                if !((n >= '0' && n <= '9') || n == '-' || n == '+' || n == '.' || n == 'e' || n == 'E') { break; }
                at++; if at - start > MAX_NUMBER_BYTES { return false; }
            }
        } else { at++; }
    }
    return depth == 0;
}
fn object(node: json::Node?) -> bool {
    if !node { return false; }
    match node.val { Obj => { return true; }, _ => { return false; } }
}
fn unique_member(parent: json::Node?, key: const u8[:]) -> json::Node? {
    if !parent { return null; }
    if !object(parent) { return null; }
    var found: json::Node? = null;
    var child = parent.first;
    while child {
        if child.key == key { if found { return null; } found .= child; }
        child .= child.next;
    }
    return found;
}
fn validate(revision: const u8[:], status: http::Status, body: u8[:], headers: const u8[:]) -> Info, u8[>..] {
    if !revision_valid(revision) { return Info { .. }, str("source metadata: expected a full lowercase commit hash"); }
    if status.error != http::Error.None { return Info { .. }, str("source metadata: request failed (", status.error, ", libcurl ", status.transport_code, ")"); }
    if status.status_code != 200 { return Info { .. }, str("source metadata: expected HTTP 200, got ", status.status_code); }
    if !identity_response(headers) { return Info { .. }, str("source metadata: unsupported or ambiguous content encoding"); }
    if !preflight(body) { return Info { .. }, str("source metadata: invalid UTF-8 or JSON resource limit"); }
    var doc:     json::Node[>..] = [];
    var decoded: u8[>..]         = [];
    let root, error = json::parse(body, doc, decoded);
    if error.len > 0 || !root || !text::valid_utf8(decoded) { return Info { .. }, str("source metadata: invalid JSON or Unicode escape"); }
    let sha_node = unique_member(root, "sha");
    let url_node = unique_member(root, "html_url");
    let commit_node = unique_member(root, "commit");
    let message_node = unique_member(commit_node, "message");
    let sha, sha_ok = json::as_string(sha_node);
    let url, url_ok = json::as_string(url_node);
    let message, message_ok = json::as_string(message_node);
    if !sha_ok || !url_ok || !message_ok || url.len == 0 { return Info { .. }, str("source metadata: missing, duplicate or invalid required field"); }
    if sha != revision { return Info { .. }, str("source metadata: response commit does not match requested revision"); }
    var end = 0;
    while end < message.len && message[end] != '\n' && message[end] != '\r' { end++; }
    return Info { requested_revision: str(revision), sha: str(sha), html_url: str(url), subject: str(message[..end]) }, str("");
}
fn fetch(revision: const u8[:]) -> Info, u8[>..] {
    if !revision_valid(revision) { return Info { .. }, str("source metadata: expected a full lowercase commit hash"); }
    let fields = [http::Header { name: "User-Agent", value: "honk-toolchain-metadata/0.1" },
                  http::Header { name: "Accept", value: "application/vnd.github+json" },
                  http::Header { name: "Accept-Encoding", value: "identity" }];
    let options = http::Options { method: http::Method.Get, allow_http: false, headers: fields, body_limit: MAX_BODY, header_limit: MAX_HEADERS, timeout_ms: TIMEOUT_MS };
    var body:    u8[>..] = [];
    var headers: u8[>..] = [];
    let status = http::request(str(ORIGIN, revision), options, body, headers);
    return validate(revision, status, body, headers);
}
fn render(info: Info&) -> u8[>..] {
    var out: u8[>..] = [];
    out.push('{'); json::format_key(out, "requested_revision"); json::format_string(out, info.requested_revision);
    out.push(','); json::format_key(out, "sha"); json::format_string(out, info.sha);
    out.push(','); json::format_key(out, "html_url"); json::format_string(out, info.html_url);
    out.push(','); json::format_key(out, "subject"); json::format_string(out, info.subject);
    out.push('}'); return out;
}
