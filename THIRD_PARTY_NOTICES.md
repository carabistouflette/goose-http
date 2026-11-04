# Third-party components

No third-party implementation source is vendored in goose-http. Its own sources
are licensed under Apache-2.0; see [LICENSE](LICENSE).

The private native adapter links to the installed **libcurl** (>=7.85), copyright
Daniel Stenberg and contributors, under the [curl license](https://curl.se/docs/copyright.html).
libcurl may depend on a TLS implementation and other libraries supplied by your
system. Their licenses and notices remain applicable to redistributed binaries;
consult your distribution's libcurl package and linked dependency licenses.

Goose is built from the exact revision in [GOOSE_REVISION](GOOSE_REVISION) under
Apache-2.0. `make setup` downloads its source into ignored `.toolchain/`, including
upstream third-party components and their notices. Those sources/build artifacts
are not part of this library's source distribution. Keep upstream notices if
redistributing the compiler or its dependencies.

The optional HTTP/2 test server uses installed **hyper-h2**, **hpack** and
**hyperframe**, each under the MIT license. These packages are used only by the
test fixture and are not linked into applications or vendored here. See their
upstream notices: [h2](https://github.com/python-hyper/h2/blob/master/LICENSE),
[hpack](https://github.com/python-hyper/hpack/blob/master/LICENSE), and
[hyperframe](https://github.com/python-hyper/hyperframe/blob/master/LICENSE).
