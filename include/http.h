/** @file http.h
 * @brief Minimal HTTP/1.1 subset: message parsing and serialisation.
 *
 * Supported: Content-Length framing, keep-alive, CRLF line endings,
 * case-insensitive header names, arbitrary extra headers, partial reads and
 * pipelined messages on one connection. Rejected as malformed: bare LF line
 * endings, obsolete line folding, oversized lines/headers, conflicting or
 * non-numeric Content-Length, any Transfer-Encoding, 1xx responses and
 * responses without Content-Length. This is deliberately not a complete
 * RFC 9112 implementation.
 */
#ifndef LLMGW_HTTP_H
#define LLMGW_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#include "buf.h"
#include "netio.h"

/** Longest accepted start line or header line, excluding CRLF. */
#define HTTP_MAX_LINE 8000
/** Most header fields accepted in one message. */
#define HTTP_MAX_HEADERS 100
/** Most header bytes (all lines together) accepted in one message. */
#define HTTP_MAX_HEADER_BYTES 32768

/** Parser and transport outcomes. */
typedef enum {
    HTTP_OK = 0,
    /** Orderly close before the first byte of a message. */
    HTTP_EOF,
    /** Syntax error, truncated message or unsupported framing. */
    HTTP_MALFORMED,
    HTTP_IO_ERROR,
    HTTP_TIMEOUT,
    HTTP_STOPPED,
    /** Response body larger than the caller's limit. */
    HTTP_TOO_LARGE,
    HTTP_NOMEM
} HttpStatus;

/** One header field (both strings owned). */
typedef struct {
    char* name;
    char* value;
} HttpHeader;

/** Parsed request line and headers; the body is read separately. */
typedef struct {
    char* method;
    char* target;
    HttpHeader* headers;
    size_t headerCount;
    /** Declared body length (0 when no Content-Length was sent). */
    unsigned long long contentLength;
    bool hasContentLength;
    /** True when the connection must be closed after the response. */
    bool connectionClose;
} HttpRequestHead;

/** Parsed response including its body. */
typedef struct {
    int status;
    char* reason;
    HttpHeader* headers;
    size_t headerCount;
    /** Body bytes, NUL-terminated for convenience; may contain NUL. */
    unsigned char* body;
    size_t bodyLen;
    bool connectionClose;
} HttpResponse;

/** Reads a request line and headers. */
HttpStatus http_read_request_head(Reader* reader, HttpRequestHead* head);

/** Frees a request head (safe on a zeroed struct). */
void http_request_head_free(HttpRequestHead* head);

/**
 * Reads exactly @p len body bytes into a new NUL-terminated buffer.
 * The caller must bound @p len before calling.
 */
HttpStatus http_read_body(Reader* reader, size_t len, unsigned char** body);

/** Reads and drops @p len body bytes without allocating. */
HttpStatus http_discard_body(Reader* reader, unsigned long long len);

/** Reads a full response whose body may be at most @p maxBody bytes. */
HttpStatus http_read_response(
        Reader* reader, size_t maxBody, HttpResponse* response);

/** Frees a response (safe on a zeroed struct). */
void http_response_free(HttpResponse* response);

/** Case-insensitive header lookup; returns the first value or NULL. */
const char* http_find_header(
        const HttpHeader* headers, size_t count, const char* name);

/** @return the standard reason phrase for the status codes used here. */
const char* http_reason(int status);

/** Serialises a response with Content-Type and byte-count Content-Length. */
int http_build_response(Buf* out, int status, const char* contentType,
        const void* body, size_t bodyLen, bool closeConnection);

/** Serialises a request with Host and Content-Length headers. */
int http_build_request(Buf* out, const char* method, const char* target,
        const char* host, const char* contentType, const void* body,
        size_t bodyLen);

/** @return a short description of a status for diagnostics. */
const char* http_status_name(HttpStatus status);

#endif
