/** @file http.c
 * @brief Minimal HTTP/1.1 subset: message parsing and serialisation.
 *
 * Written for this project to replace the course-provided HTTP helper
 * library, whose source was never available. See http.h for the supported
 * subset and docs/requirements.md for the framing policy.
 */
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http.h"

/** Header block being collected. */
typedef struct {
    HttpHeader* items;
    size_t count;
    size_t cap;
    size_t bytes;
} HeaderList;

/** Framing facts derived from the headers. */
typedef struct {
    bool hasLength;
    unsigned long long length;
    bool close;
    bool keepAlive;
} Framing;

static HttpStatus from_io(IoStatus io, bool midMessage)
{
    switch (io) {
    case IO_OK:
        return HTTP_OK;
    case IO_EOF:
        return midMessage ? HTTP_MALFORMED : HTTP_EOF;
    case IO_TIMEOUT:
        return HTTP_TIMEOUT;
    case IO_STOPPED:
        return HTTP_STOPPED;
    default:
        return HTTP_IO_ERROR;
    }
}

/**
 * Reads one CRLF-terminated line (without the CRLF) into a new string.
 * @param first true when no byte of the message has been consumed yet, so a
 * clean EOF can be distinguished from truncation.
 */
static HttpStatus read_line(Reader* reader, bool first, char** line, size_t* len)
{
    size_t scanned = 0;
    for (;;) {
        char* start = reader->buf + reader->pos;
        size_t avail = reader_available(reader);
        char* newline = memchr(start + scanned, '\n', avail - scanned);
        if (newline != NULL) {
            size_t lineLen = (size_t)(newline - start);
            if (lineLen == 0 || start[lineLen - 1] != '\r'
                    || lineLen - 1 > HTTP_MAX_LINE) {
                return HTTP_MALFORMED; /* Bare LF or oversized line. */
            }
            lineLen--;
            if (memchr(start, '\r', lineLen) != NULL
                    || memchr(start, '\0', lineLen) != NULL) {
                return HTTP_MALFORMED;
            }
            *line = mem_dup(start, lineLen);
            if (*line == NULL) {
                return HTTP_NOMEM;
            }
            *len = lineLen;
            reader->pos += lineLen + 2;
            return HTTP_OK;
        }
        scanned = avail;
        if (avail > HTTP_MAX_LINE + 1) {
            return HTTP_MALFORMED;
        }
        IoStatus io = reader_fill(reader);
        if (io != IO_OK) {
            return from_io(io, !(first && avail == 0));
        }
    }
}

static bool is_tchar(unsigned char c)
{
    return isalnum(c) || (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static bool is_token(const char* text, size_t len)
{
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!is_tchar((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

static void free_headers(HttpHeader* headers, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(headers[i].name);
        free(headers[i].value);
    }
    free(headers);
}

/** Splits "Name: value" and appends it to the list. */
static HttpStatus add_header(HeaderList* list, const char* line, size_t len)
{
    if (line[0] == ' ' || line[0] == '\t') {
        return HTTP_MALFORMED; /* Obsolete line folding. */
    }
    const char* colon = memchr(line, ':', len);
    if (colon == NULL || !is_token(line, (size_t)(colon - line))) {
        return HTTP_MALFORMED;
    }
    const char* value = colon + 1;
    const char* end = line + len;
    while (value < end && (*value == ' ' || *value == '\t')) {
        value++;
    }
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    for (const char* p = value; p < end; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            return HTTP_MALFORMED;
        }
    }
    if (list->count == HTTP_MAX_HEADERS) {
        return HTTP_MALFORMED;
    }
    if (list->count == list->cap) {
        size_t newCap = list->cap == 0 ? 8 : list->cap * 2;
        HttpHeader* grown = realloc(list->items, newCap * sizeof(*grown));
        if (grown == NULL) {
            return HTTP_NOMEM;
        }
        list->items = grown;
        list->cap = newCap;
    }
    HttpHeader header;
    header.name = mem_dup(line, (size_t)(colon - line));
    header.value = mem_dup(value, (size_t)(end - value));
    if (header.name == NULL || header.value == NULL) {
        free(header.name);
        free(header.value);
        return HTTP_NOMEM;
    }
    list->items[list->count++] = header;
    return HTTP_OK;
}

/** Reads header lines up to and including the empty line. */
static HttpStatus read_headers(Reader* reader, HeaderList* list)
{
    for (;;) {
        char* line = NULL;
        size_t len = 0;
        HttpStatus rc = read_line(reader, false, &line, &len);
        if (rc != HTTP_OK) {
            return rc;
        }
        if (len == 0) {
            free(line);
            return HTTP_OK;
        }
        list->bytes += len + 2;
        rc = list->bytes > HTTP_MAX_HEADER_BYTES ? HTTP_MALFORMED
                                                 : add_header(list, line, len);
        free(line);
        if (rc != HTTP_OK) {
            return rc;
        }
    }
}

/** Parses a Content-Length value made only of digits, without overflow. */
static bool parse_length(const char* text, unsigned long long* value)
{
    if (*text == '\0') {
        return false;
    }
    unsigned long long result = 0;
    for (const char* p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        unsigned digit = (unsigned)(*p - '0');
        if (result > (ULLONG_MAX - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

/** Checks a comma-separated Connection header for a token. */
static bool has_connection_token(const char* value, const char* token)
{
    size_t tokenLen = strlen(token);
    const char* p = value;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        const char* start = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        const char* end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        if ((size_t)(end - start) == tokenLen
                && strncasecmp(start, token, tokenLen) == 0) {
            return true;
        }
    }
    return false;
}

/** Derives body framing; rejects ambiguous or unsupported framing. */
static HttpStatus analyse_framing(const HeaderList* list, Framing* framing)
{
    memset(framing, 0, sizeof(*framing));
    for (size_t i = 0; i < list->count; i++) {
        const HttpHeader* h = &list->items[i];
        if (strcasecmp(h->name, "Transfer-Encoding") == 0) {
            return HTTP_MALFORMED;
        }
        if (strcasecmp(h->name, "Content-Length") == 0) {
            unsigned long long value;
            if (!parse_length(h->value, &value)
                    || (framing->hasLength && value != framing->length)) {
                return HTTP_MALFORMED;
            }
            framing->hasLength = true;
            framing->length = value;
        } else if (strcasecmp(h->name, "Connection") == 0) {
            framing->close |= has_connection_token(h->value, "close");
            framing->keepAlive |= has_connection_token(h->value, "keep-alive");
        }
    }
    return HTTP_OK;
}

/** Parses "HTTP/1.0" or "HTTP/1.1"; @return minor version or -1. */
static int parse_version(const char* text, size_t len)
{
    if (len == 8 && memcmp(text, "HTTP/1.", 7) == 0
            && (text[7] == '0' || text[7] == '1')) {
        return text[7] - '0';
    }
    return -1;
}

/** Splits "METHOD SP target SP HTTP/1.x". */
static HttpStatus parse_request_line(
        const char* line, size_t len, HttpRequestHead* head, int* minor)
{
    const char* sp1 = memchr(line, ' ', len);
    if (sp1 == NULL) {
        return HTTP_MALFORMED;
    }
    const char* targetStart = sp1 + 1;
    const char* sp2 = memchr(targetStart, ' ', len - (size_t)(targetStart - line));
    if (sp2 == NULL || sp2 == targetStart
            || !is_token(line, (size_t)(sp1 - line))) {
        return HTTP_MALFORMED;
    }
    for (const char* p = targetStart; p < sp2; p++) {
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7f) {
            return HTTP_MALFORMED;
        }
    }
    const char* version = sp2 + 1;
    *minor = parse_version(version, len - (size_t)(version - line));
    if (*minor < 0) {
        return HTTP_MALFORMED;
    }
    head->method = mem_dup(line, (size_t)(sp1 - line));
    head->target = mem_dup(targetStart, (size_t)(sp2 - targetStart));
    return (head->method == NULL || head->target == NULL) ? HTTP_NOMEM : HTTP_OK;
}

HttpStatus http_read_request_head(Reader* reader, HttpRequestHead* head)
{
    memset(head, 0, sizeof(*head));
    char* line = NULL;
    size_t len = 0;
    HttpStatus rc = read_line(reader, true, &line, &len);
    if (rc != HTTP_OK) {
        return rc;
    }
    int minor = -1;
    rc = parse_request_line(line, len, head, &minor);
    free(line);
    HeaderList list = {NULL, 0, 0, 0};
    Framing framing;
    if (rc == HTTP_OK) {
        rc = read_headers(reader, &list);
    }
    if (rc == HTTP_OK) {
        rc = analyse_framing(&list, &framing);
    }
    head->headers = list.items;
    head->headerCount = list.count;
    if (rc != HTTP_OK) {
        http_request_head_free(head);
        return rc;
    }
    head->hasContentLength = framing.hasLength;
    head->contentLength = framing.hasLength ? framing.length : 0;
    head->connectionClose
            = framing.close || (minor == 0 && !framing.keepAlive);
    return HTTP_OK;
}

void http_request_head_free(HttpRequestHead* head)
{
    free(head->method);
    free(head->target);
    free_headers(head->headers, head->headerCount);
    memset(head, 0, sizeof(*head));
}

/** Copies or drops @p len bytes from the reader. @p out may be NULL. */
static HttpStatus transfer_body(
        Reader* reader, unsigned long long len, unsigned char* out)
{
    unsigned long long done = 0;
    while (done < len) {
        size_t avail = reader_available(reader);
        if (avail == 0) {
            IoStatus io = reader_fill(reader);
            if (io != IO_OK) {
                return from_io(io, true);
            }
            continue;
        }
        unsigned long long want = len - done;
        size_t chunk = want < avail ? (size_t)want : avail;
        if (out != NULL) {
            memcpy(out + done, reader->buf + reader->pos, chunk);
        }
        reader->pos += chunk;
        done += chunk;
    }
    return HTTP_OK;
}

HttpStatus http_read_body(Reader* reader, size_t len, unsigned char** body)
{
    *body = NULL;
    if (len == (size_t)-1) {
        return HTTP_NOMEM;
    }
    unsigned char* data = malloc(len + 1);
    if (data == NULL) {
        return HTTP_NOMEM;
    }
    HttpStatus rc = transfer_body(reader, len, data);
    if (rc != HTTP_OK) {
        free(data);
        return rc;
    }
    data[len] = '\0';
    *body = data;
    return HTTP_OK;
}

HttpStatus http_discard_body(Reader* reader, unsigned long long len)
{
    return transfer_body(reader, len, NULL);
}

/** Parses "HTTP/1.x SP 3DIGIT [SP reason]". */
static HttpStatus parse_status_line(
        const char* line, size_t len, HttpResponse* response)
{
    if (len < 12 || parse_version(line, 8) < 0 || line[8] != ' ') {
        return HTTP_MALFORMED;
    }
    for (int i = 9; i < 12; i++) {
        if (line[i] < '0' || line[i] > '9') {
            return HTTP_MALFORMED;
        }
    }
    if (len > 12 && line[12] != ' ') {
        return HTTP_MALFORMED;
    }
    response->status = (line[9] - '0') * 100 + (line[10] - '0') * 10
            + (line[11] - '0');
    if (response->status < 200 || response->status > 599) {
        return HTTP_MALFORMED; /* 1xx interim responses are unsupported. */
    }
    response->reason = len > 12 ? mem_dup(line + 13, len - 13) : mem_dup("", 0);
    return response->reason == NULL ? HTTP_NOMEM : HTTP_OK;
}

/** Reads the body announced by the headers of a response. */
static HttpStatus read_response_body(Reader* reader, const Framing* framing,
        size_t maxBody, HttpResponse* response)
{
    bool noBody = response->status == 204 || response->status == 304;
    if (noBody) {
        response->body = (unsigned char*)mem_dup("", 0);
        return response->body == NULL ? HTTP_NOMEM : HTTP_OK;
    }
    if (!framing->hasLength) {
        return HTTP_MALFORMED; /* Read-until-close framing unsupported. */
    }
    if (framing->length > maxBody) {
        return HTTP_TOO_LARGE;
    }
    response->bodyLen = (size_t)framing->length;
    return http_read_body(reader, response->bodyLen, &response->body);
}

HttpStatus http_read_response(
        Reader* reader, size_t maxBody, HttpResponse* response)
{
    memset(response, 0, sizeof(*response));
    char* line = NULL;
    size_t len = 0;
    HttpStatus rc = read_line(reader, true, &line, &len);
    if (rc != HTTP_OK) {
        return rc;
    }
    rc = parse_status_line(line, len, response);
    free(line);
    HeaderList list = {NULL, 0, 0, 0};
    Framing framing;
    if (rc == HTTP_OK) {
        rc = read_headers(reader, &list);
    }
    if (rc == HTTP_OK) {
        rc = analyse_framing(&list, &framing);
    }
    response->headers = list.items;
    response->headerCount = list.count;
    if (rc == HTTP_OK) {
        rc = read_response_body(reader, &framing, maxBody, response);
    }
    if (rc != HTTP_OK) {
        http_response_free(response);
        return rc;
    }
    response->connectionClose = framing.close;
    return HTTP_OK;
}

void http_response_free(HttpResponse* response)
{
    free(response->reason);
    free_headers(response->headers, response->headerCount);
    free(response->body);
    memset(response, 0, sizeof(*response));
}

const char* http_find_header(
        const HttpHeader* headers, size_t count, const char* name)
{
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(headers[i].name, name) == 0) {
            return headers[i].value;
        }
    }
    return NULL;
}

const char* http_reason(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Content Too Large";
    case 500:
        return "Internal Server Error";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    default:
        return "Unknown";
    }
}

int http_build_response(Buf* out, int status, const char* contentType,
        const void* body, size_t bodyLen, bool closeConnection)
{
    buf_clear(out);
    if (buf_printf(out,
                "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                "%s\r\n",
                status, http_reason(status), contentType, bodyLen,
                closeConnection ? "Connection: close\r\n" : "")
            != 0) {
        return -1;
    }
    return buf_append(out, body, bodyLen);
}

int http_build_request(Buf* out, const char* method, const char* target,
        const char* host, const char* contentType, const void* body,
        size_t bodyLen)
{
    buf_clear(out);
    if (buf_printf(out, "%s %s HTTP/1.1\r\nHost: %s\r\n", method, target, host)
                    != 0
            || (contentType != NULL
                    && buf_printf(out, "Content-Type: %s\r\n", contentType)
                            != 0)
            || buf_printf(out, "Content-Length: %zu\r\n\r\n", bodyLen) != 0) {
        return -1;
    }
    return buf_append(out, body, bodyLen);
}

const char* http_status_name(HttpStatus status)
{
    static const char* const names[] = {"ok", "eof", "malformed", "io-error",
            "timeout", "stopped", "too-large", "no-memory"};
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status]
                                                              : "unknown";
}
