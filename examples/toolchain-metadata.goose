// SPDX-License-Identifier: Apache-2.0
import http from "..";
import text from "carabistouflette/goose-text@46059e30be79ece6efd4317eb0adc6d213c808ab";
import .honk_metadata;
import std;
import os;
fn main() {
    let argv = args();
    if argv.len != 2 { write_stderr("usage: toolchain-metadata FULL40HEXREVISION\n"); exit(2); }
    var info, error = honk_metadata::fetch(argv[1]);
    if error.len > 0 { write_stderr(str(error, "\n")); exit(1); }
    print(honk_metadata::render(info));
}
