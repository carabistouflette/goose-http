// SPDX-License-Identifier: Apache-2.0
import http from "..";
import text from "carabistouflette/goose-text@46059e30be79ece6efd4317eb0adc6d213c808ab";
import honk_metadata from "../examples";
import std;
import json;
const REVISION = "eadca409f68210893f366e60d0c2f0dc29a339be";
const HEADERS = "HTTP/1.1 200 OK\r\n\r\n";
fn document(extra: const u8[:] = "0", message: const u8[:] = "subject\nbody") -> u8[>..] {
    var out = str("{\"sha\":\"", REVISION, "\",\"html_url\":\"https://example.invalid/metadata\",\"commit\":{\"message\":");
    json::format_string(out, message); out.append("},\"extra\":"); out.append(extra); out.push('}'); return out;
}
fn rejects(input: const u8[:], headers: const u8[:] = HEADERS) {
    var body = str(input);
    let info, error = honk_metadata::validate(REVISION, http::Status { status_code: 200 }, body, headers);
    assert(error.len > 0 && info.requested_revision.len == 0 && info.sha.len == 0 && info.html_url.len == 0 && info.subject.len == 0);
}
fn accepts(input: u8[:]) -> honk_metadata::Info {
    var info, error = honk_metadata::validate(REVISION, http::Status { status_code: 200 }, input, HEADERS);
    if error.len > 0 { print(error); }
    assert(error.len == 0 && info.sha == REVISION && info.requested_revision == REVISION);
    return info;
}
fn main() {
    var body = document();
    var info = accepts(body); body.clear();
    assert(info.sha == REVISION && info.subject == "subject");
    var control = document("null", "quotes\"\\\x00\x1b\t雪\nignored");
    var escaped = accepts(control); control.clear();
    assert(escaped.subject == "quotes\"\\\x00\x1b\t雪");
    let output = honk_metadata::render(escaped);
    for c in output { assert(c >= 32); }
    var parsed: json::Node[>..] = []; var decoded: u8[>..] = [];
    let root, error = json::parse(output, parsed, decoded);
    let subject, found = json::as_string(json::member(root, "subject"));
    assert(error.len == 0 && found && subject == escaped.subject);
    var paired = str("{\"sha\":\"", REVISION, "\",\"html_url\":\"x\",\"commit\":{\"message\":\"\\ud83d\\ude00\"}}");
    assert(accepts(paired).subject == "😀");
    rejects(document("\"\\ud800\"")); rejects(document("\"\\udfff\"")); rejects(document("\"\xff\""));
    rejects(str(document(), " trailing"));
    rejects(str("{\"sha\":\"", REVISION, "\",\"sh\\u0061\":\"", REVISION, "\",\"html_url\":\"x\",\"commit\":{\"message\":\"x\"}}"));
    rejects(str("{\"sha\":\"", REVISION, "\",\"html_url\":\"x\",\"commit\":{\"message\":\"x\",\"messa\\u0067e\":\"y\"}}"));
    var nested: u8[>..] = [];
    for i in honk_metadata::MAX_DEPTH - 1 { nested.push('['); }
    nested.push('0'); for i in honk_metadata::MAX_DEPTH - 1 { nested.push(']'); }
    var boundary = document(nested); let nested_ok = accepts(boundary); assert(nested_ok.sha == REVISION);
    rejects(document(str("[", nested, "]")));
    var number = str("1"); number.resize(honk_metadata::MAX_NUMBER_BYTES, '0');
    var numeric = document(number); let numeric_ok = accepts(numeric); assert(numeric_ok.sha == REVISION);
    number.push('0'); rejects(document(number));
    var extreme = document("1e999999999999999999999999999"); let ignored = accepts(extreme); assert(ignored.sha == REVISION);
    var oversized: u8[>..] = []; oversized.resize(honk_metadata::MAX_BODY + 1, ' '); rejects(oversized);
    for revision in ["", "main", "HEAD", "000000000000000000000000000000000000000A"] {
        let empty, bad = honk_metadata::fetch(revision); assert(bad.len > 0 && empty.sha.len == 0);
    }
    print("Honk metadata validation passed");
}
