/** @file buf.h
 * @brief Growable, length-tracked byte buffer.
 *
 * A Buf always keeps one spare byte so that data[len] == '\0'. The
 * terminator is a convenience for diagnostics only; every consumer uses the
 * explicit length, so embedded NUL bytes are preserved.
 */
#ifndef LLMGW_BUF_H
#define LLMGW_BUF_H

#include <stddef.h>

/** Growable byte buffer. */
typedef struct {
    /** Heap storage, or NULL while empty. */
    char* data;
    /** Number of meaningful bytes. */
    size_t len;
    /** Allocated bytes (always > len once data != NULL). */
    size_t cap;
} Buf;

/** Initialises an empty buffer without allocating. */
void buf_init(Buf* buf);

/** Releases storage and resets the buffer to empty. */
void buf_free(Buf* buf);

/** Discards contents but keeps the allocation. */
void buf_clear(Buf* buf);

/**
 * Ensures room for @p extra more bytes plus the terminator.
 * @return 0 on success, -1 on overflow or allocation failure (buffer
 * unchanged).
 */
int buf_reserve(Buf* buf, size_t extra);

/** Appends @p len bytes. @return 0 on success, -1 on failure (unchanged). */
int buf_append(Buf* buf, const void* bytes, size_t len);

/** Appends a NUL-terminated string. @return 0 or -1. */
int buf_append_str(Buf* buf, const char* text);

/** Appends printf-style formatted text. @return 0 or -1. */
int buf_printf(Buf* buf, const char* format, ...)
        __attribute__((format(printf, 2, 3)));

/**
 * Returns a NUL-terminated heap copy of @p len bytes, or NULL on failure.
 * The copy may contain embedded NUL bytes; callers must track the length.
 */
char* mem_dup(const void* bytes, size_t len);

#endif
