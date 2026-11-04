/* SPDX-License-Identifier: Apache-2.0 */
/* Linked only into the compiled consumer fixture, never into the library. */
#include <curl/curl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "metadata fixture transport: %s\n", message);
        exit(90);
    }
}

static int saw_timeout;
CURLcode __real_curl_easy_setopt(CURL *handle, CURLoption option, ...);
CURLcode __wrap_curl_easy_setopt(CURL *handle, CURLoption option, ...) {
    va_list args;
    va_start(args, option);
    CURLcode code;
    if (option < CURLOPTTYPE_OBJECTPOINT) {
        long value = va_arg(args, long);
        if (option == CURLOPT_TIMEOUT_MS) {
            require(value == 5000, "consumer changed transfer timeout"); saw_timeout = 1;
        }
        code = __real_curl_easy_setopt(handle, option, value);
    } else if (option < CURLOPTTYPE_FUNCTIONPOINT) {
        void *value = va_arg(args, void *);
        code = __real_curl_easy_setopt(handle, option, value);
    } else if (option < CURLOPTTYPE_OFF_T) {
        require(option == CURLOPT_WRITEFUNCTION || option == CURLOPT_HEADERFUNCTION, "unexpected callback option");
        curl_write_callback value = va_arg(args, curl_write_callback);
        code = __real_curl_easy_setopt(handle, option, value);
    } else {
        require(option < CURLOPTTYPE_BLOB, "unexpected blob option");
        curl_off_t value = va_arg(args, curl_off_t);
        code = __real_curl_easy_setopt(handle, option, value);
    }
    va_end(args); return code;
}
CURLcode __real_curl_easy_perform(CURL *handle);
CURLcode __wrap_curl_easy_perform(CURL *handle) {
    require(saw_timeout, "consumer did not configure transfer timeout");
    static const char endpoint[] = "https://api.github.com/repos/aardappel/goose/commits/";
    const char *revision = getenv("GOOSE_METADATA_FIXTURE_REVISION");
    const char *origin = getenv("GOOSE_METADATA_FIXTURE_ORIGIN");
    const char *ca = getenv("GOOSE_METADATA_FIXTURE_CA");
    const char *trust = getenv("GOOSE_METADATA_FIXTURE_TRUST");
    require(revision && origin && ca && trust, "missing fixture environment");
    require(strlen(revision) == 40, "bad fixture revision");
    for (size_t i = 0; i < 40; ++i)
        require((revision[i] >= '0' && revision[i] <= '9') ||
                (revision[i] >= 'a' && revision[i] <= 'f'), "bad fixture revision");
    static const char local[] = "https://localhost:";
    require(strncmp(origin, local, sizeof local - 1) == 0, "nonlocal fixture origin");
    const char *port = origin + sizeof local - 1;
    require(*port != '\0' && strlen(port) <= 5, "bad fixture port");
    unsigned long port_number = 0;
    for (const char *p = port; *p; ++p) {
        require(*p >= '0' && *p <= '9', "bad fixture port");
        port_number = port_number * 10 + (unsigned)(*p - '0');
    }
    require(port_number > 0 && port_number <= 65535, "bad fixture port");
    char expected[sizeof endpoint + 40];
    require(snprintf(expected, sizeof expected, "%s%s", endpoint, revision) < (int)sizeof expected,
            "expected URL overflow");
    char *original = NULL;
    require(curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &original) == CURLE_OK &&
            original && strcmp(original, expected) == 0, "consumer changed fixed endpoint");
    char rewritten[256];
    require(strcmp(trust, "trusted") == 0 || strcmp(trust, "untrusted") == 0 ||
            strcmp(trust, "wrong-host") == 0, "bad fixture trust mode");
    const char *host = strcmp(trust, "wrong-host") == 0 ? "127.0.0.1" : "localhost";
    require(snprintf(rewritten, sizeof rewritten, "https://%s:%s/repos/aardappel/goose/commits/%s",
                     host, port, revision) < (int)sizeof rewritten, "fixture URL overflow");
    require(curl_easy_setopt(handle, CURLOPT_URL, rewritten) == CURLE_OK, "rewriting URL failed");
    if (strcmp(trust, "untrusted") != 0)
        require(curl_easy_setopt(handle, CURLOPT_CAINFO, ca) == CURLE_OK, "fixture CA failed");
    /* Peer/hostname verification, request policy, timeouts and callbacks remain real. */
    return __real_curl_easy_perform(handle);
}
