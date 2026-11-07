/* SPDX-License-Identifier: Apache-2.0
 * Research-only libcurl probe. Never linked into goose-http. */
#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct guard { int enabled, calls; };
struct output { char bytes[1024]; size_t used; };
static int prereq(void *p, char *remote, char *local, int remote_port, int local_port) {
    struct guard *g = p;
    (void)remote; (void)local; (void)remote_port; (void)local_port;
    ++g->calls;
    return g->enabled && g->calls > 1 ? CURL_PREREQFUNC_ABORT : CURL_PREREQFUNC_OK;
}
static size_t receive(char *p, size_t size, size_t count, void *opaque) {
    struct output *out = opaque;
    if (size && count > (sizeof(out->bytes) - out->used) / size) return 0;
    size_t n = size * count;
    memcpy(out->bytes + out->used, p, n); out->used += n;
    return n;
}
#define SET(opt, val) do { if (curl_easy_setopt(curl, (opt), (val)) != CURLE_OK) return 3; } while (0)
int main(int argc, char **argv) {
    if (argc != 5 || strncmp(argv[1], "https://localhost:", 18) ||
        (strcmp(argv[3], "guarded") && strcmp(argv[3], "unguarded")) ||
        (strcmp(argv[4], "json") && strcmp(argv[4], "empty") && strcmp(argv[4], "binary"))) return 2;
    const char *port_text = argv[1] + 18;
    if (*port_text < '0' || *port_text > '9') return 2;
    errno = 0;
    char *end = NULL;
    long port = strtol(port_text, &end, 10);
    if (errno || port < 1 || port > 65535 || strcmp(end, "/rpc")) return 2;
    char address[80];
    if (snprintf(address, sizeof address, "localhost:%ld:127.0.0.1", port) >= (int)sizeof address) return 2;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 3;
    struct curl_slist *resolve = curl_slist_append(NULL, address);
    if (!resolve) return 3;
    CURL *curl = curl_easy_init();
    if (!curl) return 3;
    const unsigned char binary[] = {'A', 0, 'B'};
    const char *payloads[] = {"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_blockNumber\",\"params\":[]}",
                              "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"eth_blockNumber\",\"params\":[]}"};
    int result[2] = {0,0}, callbacks[2] = {0,0};
    long status[2] = {0,0}, connects[2] = {0,0};
    for (int i = 0; i < 2; ++i) {
        struct guard guard = { !strcmp(argv[3], "guarded"), 0 };
        struct output out = {{0},0};
        const char *body = payloads[i];
        size_t bodylen = strlen(body);
        if (i == 1 && !strcmp(argv[4], "empty")) { body = ""; bodylen = 0; }
        if (i == 1 && !strcmp(argv[4], "binary")) { body = (const char *)binary; bodylen = sizeof(binary); }
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, i ? "X-Request-ID: 2" : "X-Request-ID: 1");
        if (!headers) return 3;
        struct curl_slist *next = curl_slist_append(headers, "Expect:");
        if (!next) return 3;
        headers = next;
        next = curl_slist_append(headers, i == 1 && !strcmp(argv[4], "binary") ?
                                 "Content-Type: application/octet-stream" : "Content-Type: application/json");
        if (!next) return 3;
        headers = next;
        curl_easy_reset(curl);
        SET(CURLOPT_URL, argv[1]); SET(CURLOPT_PROTOCOLS_STR, "https"); SET(CURLOPT_RESOLVE, resolve);
        SET(CURLOPT_FOLLOWLOCATION, 0L); SET(CURLOPT_PROXY, ""); SET(CURLOPT_NETRC, (long)CURL_NETRC_IGNORED);
        SET(CURLOPT_SSL_VERIFYPEER, 1L); SET(CURLOPT_SSL_VERIFYHOST, 2L); SET(CURLOPT_CAINFO, argv[2]);
        SET(CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1); SET(CURLOPT_NOSIGNAL, 1L);
        SET(CURLOPT_TIMEOUT_MS, 3000L); SET(CURLOPT_HTTPHEADER, headers);
        SET(CURLOPT_POST, 1L); SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)bodylen); SET(CURLOPT_POSTFIELDS, body);
        SET(CURLOPT_WRITEFUNCTION, receive); SET(CURLOPT_WRITEDATA, &out);
        SET(CURLOPT_PREREQFUNCTION, prereq); SET(CURLOPT_PREREQDATA, &guard);
        result[i] = (int)curl_easy_perform(curl); callbacks[i] = guard.calls;
        if (result[i] == CURLE_OK) {
            char expected[80];
            int n = snprintf(expected, sizeof expected, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":\"0x1\"}", i + 1);
            if (n < 0 || (size_t)n != out.used || memcmp(expected, out.bytes, out.used)) return 4;
        }
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status[i]) != CURLE_OK ||
            curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &connects[i]) != CURLE_OK) return 3;
        curl_easy_reset(curl); /* detach borrowed pointers before releasing them */
        curl_slist_free_all(headers);
    }
    const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    if (!info || !info->version) return 3;
    printf("{\"curl_version\":\"%s\",\"tls_backend\":\"%s\",\"first_code\":%d,\"second_code\":%d,"
           "\"first_status\":%ld,\"second_status\":%ld,\"first_connects\":%ld,"
           "\"second_connects\":%ld,\"first_callbacks\":%d,\"second_callbacks\":%d}\n",
           info->version, info->ssl_version ? info->ssl_version : "none",
           result[0], result[1], status[0], status[1],
           connects[0], connects[1], callbacks[0], callbacks[1]);
    curl_easy_cleanup(curl); curl_slist_free_all(resolve); curl_global_cleanup();
    return 0;
}
