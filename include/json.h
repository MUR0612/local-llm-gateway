/** @file json.h
 * @brief JSON string encoding and completion-reply parsing.
 *
 * Encoding policy (gateway -> backend): bytes are emitted as JSON string
 * content. '"', '\\', '\n', '\r', '\t', '\b' and '\f' use short escapes,
 * other control bytes (including NUL) use \u00XX, valid UTF-8 passes through
 * unchanged and every byte of an invalid UTF-8 sequence becomes �.
 *
 * Parsing policy (backend -> gateway): the whole document must be valid
 * RFC 8259 JSON in UTF-8 with nesting depth <= JSON_MAX_DEPTH. Only the
 * top-level "content" (string) and "tokens_evaluated" (non-negative integer)
 * members are extracted; nested members with those names are ignored.
 * Escapes, including surrogate pairs, are decoded to UTF-8. A decoded
 * \u0000 is kept as a NUL byte because content is length-tracked.
 */
#ifndef LLMGW_JSON_H
#define LLMGW_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "buf.h"

/** Maximum nesting depth accepted by the parser. */
#define JSON_MAX_DEPTH 64

/** Parse outcomes. */
typedef enum {
    JSON_OK = 0,
    JSON_SYNTAX,
    JSON_TOO_DEEP,
    JSON_NOT_OBJECT,
    JSON_MISSING_CONTENT,
    JSON_BAD_TYPE,
    JSON_DUPLICATE,
    JSON_NOMEM
} JsonStatus;

/** Fields extracted from a completion reply. */
typedef struct {
    /** Decoded content bytes (NUL-terminated, may contain NUL). */
    char* content;
    size_t contentLen;
    /** Whether tokens_evaluated was present (absent counts as 0). */
    bool hasTokens;
    unsigned long long tokensEvaluated;
} CompletionReply;

/** Appends @p len bytes as escaped JSON string content (no quotes). */
int json_append_escaped(Buf* out, const void* bytes, size_t len);

/** Parses a completion reply document. */
JsonStatus json_parse_completion(
        const void* text, size_t len, CompletionReply* reply);

/** Frees a reply (safe on a zeroed struct). */
void completion_reply_free(CompletionReply* reply);

/** @return a short description for diagnostics. */
const char* json_status_name(JsonStatus status);

#endif
