/** @file json.c
 * @brief JSON string encoding and completion-reply parsing.
 *
 * A small recursive-descent validator written for this project. It replaces
 * the coursework's strstr()-based extraction, which matched nested keys,
 * mis-detected the closing quote after an escaped backslash and did not
 * decode \u escapes. Policy details are documented in json.h.
 */
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

/** Parser cursor. */
typedef struct {
    const unsigned char* p;
    const unsigned char* end;
    int depth;
} Parser;

/** @return length (1-4) of the valid UTF-8 sequence at @p s, or 0. */
static size_t utf8_sequence_len(const unsigned char* s, size_t avail)
{
    unsigned char c = s[0];
    size_t n;
    unsigned char lo = 0x80;
    unsigned char hi = 0xBF;
    if (c < 0x80) {
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        lo = c == 0xE0 ? 0xA0 : 0x80; /* Reject overlong forms. */
        hi = c == 0xED ? 0x9F : 0xBF; /* Reject UTF-16 surrogates. */
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        lo = c == 0xF0 ? 0x90 : 0x80;
        hi = c == 0xF4 ? 0x8F : 0xBF; /* Stay <= U+10FFFF. */
    } else {
        return 0;
    }
    if (avail < n || s[1] < lo || s[1] > hi) {
        return 0;
    }
    for (size_t i = 2; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
    }
    return n;
}

int json_append_escaped(Buf* out, const void* bytes, size_t len)
{
    const unsigned char* s = bytes;
    size_t i = 0;
    while (i < len) {
        unsigned char c = s[i];
        const char* esc = NULL;
        char hex[8];
        switch (c) {
        case '"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        case '\n':
            esc = "\\n";
            break;
        case '\r':
            esc = "\\r";
            break;
        case '\t':
            esc = "\\t";
            break;
        case '\b':
            esc = "\\b";
            break;
        case '\f':
            esc = "\\f";
            break;
        default:
            break;
        }
        if (esc == NULL && c < 0x20) {
            static const char digits[] = "0123456789abcdef";
            memcpy(hex, "\\u00", 4);
            hex[4] = digits[c >> 4];
            hex[5] = digits[c & 0xF];
            hex[6] = '\0';
            esc = hex;
        }
        if (esc != NULL) {
            if (buf_append_str(out, esc) != 0) {
                return -1;
            }
            i++;
            continue;
        }
        size_t n = utf8_sequence_len(s + i, len - i);
        if (n == 0) {
            if (buf_append_str(out, "\\ufffd") != 0) {
                return -1;
            }
            i++;
            continue;
        }
        if (buf_append(out, s + i, n) != 0) {
            return -1;
        }
        i += n;
    }
    return 0;
}

static void skip_ws(Parser* ps)
{
    while (ps->p < ps->end
            && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n'
                    || *ps->p == '\r')) {
        ps->p++;
    }
}

/** Appends a code point as UTF-8 (if @p out is non-NULL). */
static int put_code_point(Buf* out, unsigned long cp)
{
    unsigned char bytes[4];
    size_t n;
    if (out == NULL) {
        return 0;
    }
    if (cp < 0x80) {
        bytes[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800) {
        bytes[0] = (unsigned char)(0xC0 | (cp >> 6));
        bytes[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        bytes[0] = (unsigned char)(0xE0 | (cp >> 12));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        bytes[0] = (unsigned char)(0xF0 | (cp >> 18));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return buf_append(out, bytes, n);
}

/** Reads 4 hex digits after "\u". @return value or -1. */
static long read_hex4(Parser* ps)
{
    if (ps->end - ps->p < 4) {
        return -1;
    }
    long value = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = *ps->p++;
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return -1;
        }
        value = value * 16 + digit;
    }
    return value;
}

/** Decodes a \uXXXX escape (cursor after the 'u'), joining surrogates. */
static JsonStatus parse_unicode_escape(Parser* ps, Buf* out)
{
    long cp = read_hex4(ps);
    if (cp < 0 || (cp >= 0xDC00 && cp <= 0xDFFF)) {
        return JSON_SYNTAX; /* Bad hex or lone low surrogate. */
    }
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (ps->end - ps->p < 2 || ps->p[0] != '\\' || ps->p[1] != 'u') {
            return JSON_SYNTAX; /* High surrogate must be followed by low. */
        }
        ps->p += 2;
        long low = read_hex4(ps);
        if (low < 0xDC00 || low > 0xDFFF) {
            return JSON_SYNTAX;
        }
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
    }
    return put_code_point(out, (unsigned long)cp) == 0 ? JSON_OK : JSON_NOMEM;
}

/** Decodes a simple escape (cursor on the character after '\'). */
static JsonStatus parse_escape(Parser* ps, Buf* out)
{
    if (ps->p >= ps->end) {
        return JSON_SYNTAX;
    }
    unsigned char c = *ps->p++;
    const char* from = "\"\\/bfnrt";
    const char* to = "\"\\/\b\f\n\r\t";
    if (c == 'u') {
        return parse_unicode_escape(ps, out);
    }
    const char* hit = memchr(from, c, 8);
    if (hit == NULL) {
        return JSON_SYNTAX;
    }
    char decoded = to[hit - from];
    return (out == NULL || buf_append(out, &decoded, 1) == 0) ? JSON_OK
                                                              : JSON_NOMEM;
}

/** Parses a string (cursor on '"'), decoding into @p out if non-NULL. */
static JsonStatus parse_string(Parser* ps, Buf* out)
{
    ps->p++;
    while (ps->p < ps->end) {
        unsigned char c = *ps->p;
        if (c == '"') {
            ps->p++;
            return JSON_OK;
        }
        if (c < 0x20) {
            return JSON_SYNTAX; /* Raw control characters are not allowed. */
        }
        if (c == '\\') {
            ps->p++;
            JsonStatus rc = parse_escape(ps, out);
            if (rc != JSON_OK) {
                return rc;
            }
            continue;
        }
        size_t n = utf8_sequence_len(ps->p, (size_t)(ps->end - ps->p));
        if (n == 0) {
            return JSON_SYNTAX;
        }
        if (out != NULL && buf_append(out, ps->p, n) != 0) {
            return JSON_NOMEM;
        }
        ps->p += n;
    }
    return JSON_SYNTAX;
}

static bool is_digit(const Parser* ps)
{
    return ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9';
}

/**
 * Validates a number. When it is a plain non-negative integer that fits,
 * @p isUint is set and @p value receives it.
 */
static JsonStatus parse_number(
        Parser* ps, bool* isUint, unsigned long long* value)
{
    bool negative = false;
    bool plain = true;
    unsigned long long acc = 0;
    if (*ps->p == '-') {
        negative = true;
        ps->p++;
    }
    if (!is_digit(ps)) {
        return JSON_SYNTAX;
    }
    if (*ps->p == '0') {
        ps->p++;
    } else {
        while (is_digit(ps)) {
            unsigned digit = (unsigned)(*ps->p++ - '0');
            if (acc > (ULLONG_MAX - digit) / 10) {
                plain = false; /* Overflow: still valid JSON, wrong type. */
            }
            acc = acc * 10 + digit;
        }
    }
    if (ps->p < ps->end && *ps->p == '.') {
        plain = false;
        ps->p++;
        if (!is_digit(ps)) {
            return JSON_SYNTAX;
        }
        while (is_digit(ps)) {
            ps->p++;
        }
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        plain = false;
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) {
            ps->p++;
        }
        if (!is_digit(ps)) {
            return JSON_SYNTAX;
        }
        while (is_digit(ps)) {
            ps->p++;
        }
    }
    *isUint = plain && !negative;
    *value = acc;
    return JSON_OK;
}

static JsonStatus parse_value(Parser* ps);

static JsonStatus parse_literal(Parser* ps, const char* word)
{
    size_t n = strlen(word);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) {
        return JSON_SYNTAX;
    }
    ps->p += n;
    return JSON_OK;
}

/** Parses '[' ... ']' or '{' ... '}' generically (cursor on the opener). */
static JsonStatus parse_container(Parser* ps)
{
    bool isObject = *ps->p == '{';
    unsigned char closer = isObject ? '}' : ']';
    if (++ps->depth > JSON_MAX_DEPTH) {
        return JSON_TOO_DEEP;
    }
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == closer) {
        ps->p++;
        ps->depth--;
        return JSON_OK;
    }
    for (;;) {
        JsonStatus rc;
        skip_ws(ps);
        if (isObject) {
            if (ps->p >= ps->end || *ps->p != '"') {
                return JSON_SYNTAX;
            }
            if ((rc = parse_string(ps, NULL)) != JSON_OK) {
                return rc;
            }
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p++ != ':') {
                return JSON_SYNTAX;
            }
        }
        if ((rc = parse_value(ps)) != JSON_OK) {
            return rc;
        }
        skip_ws(ps);
        if (ps->p >= ps->end) {
            return JSON_SYNTAX;
        }
        unsigned char c = *ps->p++;
        if (c == closer) {
            ps->depth--;
            return JSON_OK;
        }
        if (c != ',') {
            return JSON_SYNTAX;
        }
    }
}

static JsonStatus parse_value(Parser* ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) {
        return JSON_SYNTAX;
    }
    bool isUint;
    unsigned long long ignored;
    switch (*ps->p) {
    case '{':
    case '[':
        return parse_container(ps);
    case '"':
        return parse_string(ps, NULL);
    case 't':
        return parse_literal(ps, "true");
    case 'f':
        return parse_literal(ps, "false");
    case 'n':
        return parse_literal(ps, "null");
    default:
        return parse_number(ps, &isUint, &ignored);
    }
}

/** Handles the value of one top-level member. */
static JsonStatus parse_member(Parser* ps, const Buf* key, Buf* content,
        bool* seenContent, CompletionReply* reply)
{
    skip_ws(ps);
    bool isContent = key->len == 7 && memcmp(key->data, "content", 7) == 0;
    bool isTokens
            = key->len == 16 && memcmp(key->data, "tokens_evaluated", 16) == 0;
    if (isContent) {
        if (*seenContent) {
            return JSON_DUPLICATE;
        }
        if (ps->p >= ps->end || *ps->p != '"') {
            return parse_value(ps) == JSON_OK ? JSON_BAD_TYPE : JSON_SYNTAX;
        }
        *seenContent = true;
        return parse_string(ps, content);
    }
    if (isTokens) {
        if (reply->hasTokens) {
            return JSON_DUPLICATE;
        }
        if (ps->p >= ps->end || !(*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9'))) {
            return parse_value(ps) == JSON_OK ? JSON_BAD_TYPE : JSON_SYNTAX;
        }
        bool isUint;
        JsonStatus rc = parse_number(ps, &isUint, &reply->tokensEvaluated);
        if (rc != JSON_OK) {
            return rc;
        }
        reply->hasTokens = true;
        return isUint ? JSON_OK : JSON_BAD_TYPE;
    }
    return parse_value(ps);
}

/** Parses the top-level object's members. */
static JsonStatus parse_top_object(
        Parser* ps, Buf* content, bool* seenContent, CompletionReply* reply)
{
    Buf key;
    buf_init(&key);
    JsonStatus rc = JSON_OK;
    ps->depth = 1;
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return JSON_OK;
    }
    for (;;) {
        skip_ws(ps);
        buf_clear(&key);
        if (ps->p >= ps->end || *ps->p != '"') {
            rc = JSON_SYNTAX;
            break;
        }
        if ((rc = parse_string(ps, &key)) != JSON_OK) {
            break;
        }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p++ != ':') {
            rc = JSON_SYNTAX;
            break;
        }
        if ((rc = parse_member(ps, &key, content, seenContent, reply))
                != JSON_OK) {
            break;
        }
        skip_ws(ps);
        if (ps->p >= ps->end) {
            rc = JSON_SYNTAX;
            break;
        }
        unsigned char c = *ps->p++;
        if (c == '}') {
            break;
        }
        if (c != ',') {
            rc = JSON_SYNTAX;
            break;
        }
    }
    buf_free(&key);
    return rc;
}

JsonStatus json_parse_completion(
        const void* text, size_t len, CompletionReply* reply)
{
    memset(reply, 0, sizeof(*reply));
    Parser ps = {text, (const unsigned char*)text + len, 0};
    Buf content;
    buf_init(&content);
    bool seenContent = false;
    JsonStatus rc;
    skip_ws(&ps);
    if (ps.p >= ps.end) {
        rc = JSON_SYNTAX;
    } else if (*ps.p != '{') {
        rc = parse_value(&ps) == JSON_OK ? JSON_NOT_OBJECT : JSON_SYNTAX;
    } else {
        rc = parse_top_object(&ps, &content, &seenContent, reply);
    }
    if (rc == JSON_OK) {
        skip_ws(&ps);
        if (ps.p != ps.end) {
            rc = JSON_SYNTAX; /* Trailing garbage. */
        } else if (!seenContent) {
            rc = JSON_MISSING_CONTENT;
        }
    }
    if (rc == JSON_OK && content.data == NULL && buf_reserve(&content, 0) != 0) {
        rc = JSON_NOMEM; /* Empty string: still hand back a valid pointer. */
    }
    if (rc != JSON_OK) {
        buf_free(&content);
        memset(reply, 0, sizeof(*reply));
        return rc;
    }
    reply->content = content.data;
    reply->contentLen = content.len;
    return JSON_OK;
}

void completion_reply_free(CompletionReply* reply)
{
    free(reply->content);
    memset(reply, 0, sizeof(*reply));
}

const char* json_status_name(JsonStatus status)
{
    static const char* const names[] = {"ok", "syntax", "too-deep",
            "not-object", "missing-content", "bad-type", "duplicate",
            "no-memory"};
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status]
                                                              : "unknown";
}
