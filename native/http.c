/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

#if LIBCURL_VERSION_NUM < 0x075500
#error "goose-http requires libcurl >= 7.85.0"
#endif

static pthread_once_t initialized = PTHREAD_ONCE_INIT;
static CURLcode init_code = CURLE_FAILED_INIT;
static void initialize(void) { init_code = curl_global_init(CURL_GLOBAL_DEFAULT); }

int gh_ensure_initialized(void) {
    if (pthread_once(&initialized, initialize) != 0 || init_code != CURLE_OK) return -1;
    return 0;
}

typedef struct { uint8_t *data; size_t limit, used; bool exceeded; } buffer;
static size_t receive(char *data, size_t size, size_t count, void *opaque) {
    buffer *b = opaque;
    if (size && count > SIZE_MAX / size) { b->exceeded = true; return 0; }
    size_t n = size * count;
    if (n > b->limit - b->used) { b->exceeded = true; return 0; }
    if (n) memcpy(b->data + b->used, data, n);
    b->used += n;
    return n;
}

bool gh_valid_slice(const void *p, int64_t n, int64_t max) {
    return n >= 0 && n <= max && (n == 0 || p != NULL);
}

bool gh_overlap(const void *a, int64_t an, const void *b, int64_t bn) {
    if (!an || !bn) return false;
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < (uint64_t)an : x - y < (uint64_t)bn;
}

bool gh_token(uint8_t c) {
    return c != 0 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

bool gh_equal_name(const uint8_t *p, size_t n, const char *s) {
    if (n != strlen(s)) return false;
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = p[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (uint8_t)s[i]) return false;
    }
    return true;
}

bool gh_valid_header(const uint8_t *p, size_t n) {
    size_t colon = 0;
    while (colon < n && p[colon] != ':') {
        if (!p[colon] || !gh_token(p[colon])) return false;
        ++colon;
    }
    if (!colon || colon == n) return false;
    if (gh_equal_name(p, colon, "accept-encoding")) {
        size_t first = colon + 1;
        while (first < n && (p[first] == ' ' || p[first] == '\t')) ++first;
        return gh_equal_name(p + first, n - first, "identity");
    }
    static const char *const forbidden[] = {"host", "content-length", "transfer-encoding",
        "connection", "proxy-connection", "te", "trailer", "upgrade", "expect"};
    for (size_t i = 0; i < sizeof forbidden / sizeof *forbidden; ++i)
        if (gh_equal_name(p, colon, forbidden[i])) return false;
    for (size_t i = colon + 1; i < n; ++i)
        if (p[i] != '\t' && (p[i] < 32 || p[i] > 126)) return false;
    return true;
}

/* Header callbacks include informational status blocks and trailers. Only the
 * final status block must be complete; a prior 1xx separator is insufficient.
 * Delegate parsing to libcurl and track only its status-line/blank-line events. */
typedef struct { buffer bytes; bool started, complete; } header_buffer;
static size_t receive_header(char *data, size_t size, size_t count, void *opaque) {
    header_buffer *headers = opaque;
    size_t n = receive(data, size, count, &headers->bytes);
    if (headers->bytes.exceeded) return n;
    if (n >= 5 && gh_equal_name((const uint8_t *)data, 5, "http/")) {
        headers->started = true;
        headers->complete = false;
    } else if (headers->started &&
               ((n == 2 && data[0] == '\r' && data[1] == '\n') || (n == 1 && data[0] == '\n'))) {
        headers->complete = true;
    }
    return n;
}

static int64_t transport_ready(void) {
    if (gh_ensure_initialized() != 0) return GH_SETUP;
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    if (version->version_num < 0x075500 || !(version->features & CURL_VERSION_SSL) ||
        !(version->features & CURL_VERSION_ASYNCHDNS) || !(version->features & CURL_VERSION_THREADSAFE))
        return GH_SETUP;
    return GH_OK;
}

/* CURLUPART_HOST excludes zone IDs; reject scoped-interface URLs for clients. */
static int64_t client_port(CURLU *parsed, int64_t *port) {
    char *text = NULL;
    CURLUcode code = curl_url_get(parsed, CURLUPART_ZONEID, &text, 0);
    curl_free(text);
    if (code == CURLUE_OUT_OF_MEMORY) return GH_SETUP;
    if (code != CURLUE_NO_ZONEID) return GH_INVALID;
    text = NULL;
    code = curl_url_get(parsed, CURLUPART_PORT, &text, CURLU_DEFAULT_PORT);
    if (code != CURLUE_OK) { curl_free(text); return code == CURLUE_OUT_OF_MEMORY ? GH_SETUP : GH_INVALID; }
    int64_t value = 0;
    bool valid = text && *text;
    if (valid) for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') { valid = false; break; }
        value = value * 10 + *p - '0';
        if (value > 65535) { valid = false; break; }
    }
    curl_free(text);
    if (!valid || value < 1) return GH_INVALID;
    *port = value;
    return GH_OK;
}

static bool same_host(const char *left, const char *right) {
    for (; *left && *right; ++left, ++right) {
        unsigned char a = (unsigned char)*left, b = (unsigned char)*right;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return *left == *right;
}

int64_t gh_parse_client_origin(gh_input origin, char **host, int64_t *port) {
    *host = NULL;
    *port = 0;
    if (!gh_valid_slice(origin.data, origin.len, 8192) || origin.len <= 8 ||
        memcmp(origin.data, "https://", 8)) return GH_INVALID;
    for (int64_t i = 0; i < origin.len; ++i) {
        uint8_t c = origin.data[i];
        if (c <= 32 || c >= 127 || c == '?' || c == '#' ||
            (i >= 8 && c == '/' && i != origin.len - 1)) return GH_INVALID;
    }
    int64_t ready = transport_ready();
    if (ready != GH_OK) return ready;
    char address[8193];
    memcpy(address, origin.data, (size_t)origin.len); address[origin.len] = 0;
    CURLU *parsed = curl_url();
    if (!parsed) return GH_SETUP;
    CURLUcode code = curl_url_set(parsed, CURLUPART_URL, address, CURLU_DISALLOW_USER);
    if (code == CURLUE_OK) code = curl_url_get(parsed, CURLUPART_HOST, host, 0);
    int64_t result = code == CURLUE_OUT_OF_MEMORY ? GH_SETUP :
        code != CURLUE_OK || !*host || !**host ? GH_INVALID : client_port(parsed, port);
    if (result == GH_OK) for (const unsigned char *p = (const unsigned char *)*host; *p; ++p)
        if (*p <= 32 || *p >= 127) { result = GH_INVALID; break; }
    curl_url_cleanup(parsed);
    if (result != GH_OK) { curl_free(*host); *host = NULL; *port = 0; }
    return result;
}

int64_t gh_request_engine(CURL **storage, const char *bound_host, int64_t bound_port,
                   gh_input url, int64_t method, gh_input packed, gh_input upload,
                   int64_t timeout_ms, int64_t allow_http, gh_output out,
                   gh_output headers, gh_metadata meta) {
    if (meta.len != 4 || !meta.data ||
        !gh_valid_slice(url.data, url.len, 8192) ||
        !gh_valid_slice(packed.data, packed.len, 65536) ||
        !gh_valid_slice(upload.data, upload.len, 1048576) ||
        !gh_valid_slice(out.data, out.len, 16777216) ||
        !gh_valid_slice(headers.data, headers.len, 262144)) return GH_INVALID;
    const void *ptrs[] = {url.data, packed.data, upload.data, out.data, headers.data, meta.data};
    int64_t lens[] = {url.len, packed.len, upload.len, out.len, headers.len, 32};
    /* Outputs cannot alias any other region; read-only inputs may alias. */
    for (size_t i = 3; i < 6; ++i)
        for (size_t j = 0; j < i; ++j)
            if (gh_overlap(ptrs[i], lens[i], ptrs[j], lens[j])) return GH_INVALID;
    memset(meta.data, 0, 32);
    if (!url.len || method < 0 || method > 1 || (!method && upload.len) ||
        timeout_ms < 1 || timeout_ms > 86400000 || allow_http < 0 || allow_http > 1) return GH_INVALID;
    for (int64_t i = 0; i < url.len; ++i)
        if (url.data[i] <= 32 || url.data[i] >= 127 || url.data[i] == '#') return GH_INVALID;
    bool https = url.len >= 8 && !memcmp(url.data, "https://", 8);
    bool http = url.len >= 7 && !memcmp(url.data, "http://", 7);
    if (!https && !(http && allow_http)) return GH_INVALID;
    if (bound_host && (!https || method != 0 || upload.len)) return GH_INVALID;
    int64_t authority = https ? 8 : 7;
    if (url.len <= authority || url.data[authority] == '/' || url.data[authority] == '?') return GH_INVALID;
    char address[8193];
    memcpy(address, url.data, (size_t)url.len); address[url.len] = 0;
    /* Validate all packed lines BEFORE allocating curl header nodes. */
    size_t pos = 0, count = 0, wire_bytes = 0;
    while (pos < (size_t)packed.len) {
        const uint8_t *end = memchr(packed.data + pos, 0, (size_t)packed.len - pos);
        if (!end) return GH_INVALID;
        size_t n = (size_t)(end - packed.data - pos);
        if (++count > 64 || !gh_valid_header(packed.data + pos, n)) return GH_INVALID;
        wire_bytes += n + 2;
        if (wire_bytes > 65536) return GH_INVALID;
        pos += n + 1;
    }
    int64_t ready = transport_ready();
    if (ready != GH_OK) return ready;
    CURLU *parsed = curl_url();
    if (!parsed) return GH_SETUP;
    CURLUcode uc = curl_url_set(parsed, CURLUPART_URL, address, CURLU_DISALLOW_USER);
    char *host = NULL;
    if (uc == CURLUE_OK) uc = curl_url_get(parsed, CURLUPART_HOST, &host, 0);
    bool valid = uc == CURLUE_OK && host && *host;
    int64_t origin_result = GH_OK;
    if (valid && bound_host) {
        int64_t port = 0;
        origin_result = client_port(parsed, &port);
        if (origin_result == GH_OK && (!same_host(host, bound_host) || port != bound_port)) origin_result = GH_INVALID;
    }
    curl_free(host); curl_url_cleanup(parsed);
    if (uc == CURLUE_OUT_OF_MEMORY) return GH_SETUP;
    if (!valid) return GH_INVALID;
    if (origin_result != GH_OK) return origin_result;

    CURL *curl = *storage;
    if (!curl) {
        curl = curl_easy_init();
        if (!curl) return GH_SETUP;
        *storage = curl;
    }
    struct curl_slist *list = NULL, *tail = NULL;
    int64_t result = GH_SETUP;
    CURLcode code = CURLE_OK;
    buffer body = {out.data, (size_t)out.len, 0, false};
    header_buffer head = {{headers.data, (size_t)headers.len, 0, false}, false, false};
    for (pos = 0; pos < (size_t)packed.len;) {
        /* libcurl treats 'Name:' as suppression; ';' explicitly sends an empty value. */
        const char *line = (const char *)packed.data + pos;
        size_t length = strlen(line);
        const char *colon = strchr(line, ':');
        const char *value = colon + 1;
        while (*value == ' ' || *value == '\t') ++value;
        const char *wire = line;
        char *empty_line = NULL;
        if (!*value) {
            size_t name_length = (size_t)(colon - line);
            empty_line = malloc(name_length + 2);
            if (!empty_line) goto done;
            memcpy(empty_line, line, name_length);
            empty_line[name_length] = ';'; empty_line[name_length + 1] = 0;
            wire = empty_line;
        }
        /* The tail is itself a one-node list: libcurl owns all link updates. */
        struct curl_slist *next = curl_slist_append(tail, wire);
        free(empty_line);
        if (!next) goto done;
        if (!list) list = next;
        tail = tail ? tail->next : next;
        pos += length + 1;
    }
    struct curl_slist *next = curl_slist_append(tail, "Expect:");
    if (!next) goto done;
    if (!list) list = next;
#define SET(option, value) do { code = curl_easy_setopt(curl, option, value); if (code != CURLE_OK) goto done; } while (0)
    SET(CURLOPT_URL, address);
    SET(CURLOPT_PROTOCOLS_STR, !bound_host && allow_http ? "http,https" : "https");
    SET(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_NETRC, (long)CURL_NETRC_IGNORED);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    SET(CURLOPT_HTTP_CONTENT_DECODING, 0L);
    SET(CURLOPT_HTTPHEADER, list);
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_WRITEDATA, &body);
    SET(CURLOPT_HEADERFUNCTION, receive_header);
    SET(CURLOPT_HEADERDATA, &head);
    if (bound_host) {
        SET(CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
        SET(CURLOPT_MAXCONNECTS, 1L);
        SET(CURLOPT_HTTPAUTH, (long)CURLAUTH_NONE);
        SET(CURLOPT_PROXYAUTH, (long)CURLAUTH_NONE);
    }
    if (method) {
        SET(CURLOPT_POST, 1L);
        SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)upload.len);
        SET(CURLOPT_POSTFIELDS, upload.len ? (const char *)upload.data : "");
    }
    code = curl_easy_perform(curl);
    long status = 0;
    CURLcode info = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (info != CURLE_OK && code == CURLE_OK) code = info;
    if (code == CURLE_OK && !head.complete) code = CURLE_PARTIAL_FILE;
    meta.data[1] = status;
    result = body.exceeded ? GH_BODY : head.bytes.exceeded ? GH_HEADERS :
             code == CURLE_OPERATION_TIMEDOUT ? GH_TIMEOUT : code != CURLE_OK ? GH_TRANSPORT : GH_OK;
    if (result == GH_OK) { meta.data[2] = (int64_t)body.used; meta.data[3] = (int64_t)head.bytes.used; }
done:
    meta.data[0] = code;
    /* Remove borrowed callbacks/header/body pointers before returning a cached
     * handle. Completed requests retain only backend connection/cache state. */
    if (bound_host) curl_easy_reset(curl);
    if (!bound_host || result != GH_OK) { curl_easy_cleanup(curl); *storage = NULL; }
    curl_slist_free_all(list);
    return result;
#undef SET
}

int64_t gh_request(gh_input url, int64_t method, gh_input packed, gh_input upload,
                   int64_t timeout_ms, int64_t allow_http, gh_output out,
                   gh_output headers, gh_metadata meta) {
    CURL *storage = NULL;
    return gh_request_engine(&storage, NULL, 0, url, method, packed, upload,
        timeout_ms, allow_http, out, headers, meta);
}
