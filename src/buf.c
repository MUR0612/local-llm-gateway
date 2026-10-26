/** @file buf.c
 * @brief Growable, length-tracked byte buffer.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"

#define BUF_MIN_CAP 64

void buf_init(Buf* buf)
{
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void buf_free(Buf* buf)
{
    free(buf->data);
    buf_init(buf);
}

void buf_clear(Buf* buf)
{
    buf->len = 0;
    if (buf->data != NULL) {
        buf->data[0] = '\0';
    }
}

int buf_reserve(Buf* buf, size_t extra)
{
    /* Need len + extra + 1 bytes; reject arithmetic overflow up front. */
    if (extra > SIZE_MAX - 1 - buf->len) {
        return -1;
    }
    size_t need = buf->len + extra + 1;
    if (need <= buf->cap) {
        return 0;
    }
    size_t newCap = buf->cap < BUF_MIN_CAP ? BUF_MIN_CAP : buf->cap;
    while (newCap < need) {
        if (newCap > SIZE_MAX / 2) {
            newCap = need;
            break;
        }
        newCap *= 2;
    }
    char* grown = realloc(buf->data, newCap);
    if (grown == NULL) {
        return -1;
    }
    if (buf->data == NULL) {
        grown[0] = '\0';
    }
    buf->data = grown;
    buf->cap = newCap;
    return 0;
}

int buf_append(Buf* buf, const void* bytes, size_t len)
{
    if (buf_reserve(buf, len) != 0) {
        return -1;
    }
    if (len > 0) {
        memcpy(buf->data + buf->len, bytes, len);
    }
    buf->len += len;
    buf->data[buf->len] = '\0';
    return 0;
}

int buf_append_str(Buf* buf, const char* text)
{
    return buf_append(buf, text, strlen(text));
}

int buf_printf(Buf* buf, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int needed = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (needed < 0 || buf_reserve(buf, (size_t)needed) != 0) {
        return -1;
    }
    va_start(args, format);
    vsnprintf(buf->data + buf->len, (size_t)needed + 1, format, args);
    va_end(args);
    buf->len += (size_t)needed;
    return 0;
}

char* mem_dup(const void* bytes, size_t len)
{
    if (len == SIZE_MAX) {
        return NULL;
    }
    char* copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(copy, bytes, len);
    }
    copy[len] = '\0';
    return copy;
}
