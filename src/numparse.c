/** @file numparse.c
 * @brief Strict unsigned decimal parsing for CLI and environment values.
 *
 * Replaces the coursework strtol() check, which accepted leading
 * whitespace and '-' signs before the range check.
 */
#include <limits.h>

#include "numparse.h"

bool parse_bounded_uint(
        const char* text, unsigned long min, unsigned long max, unsigned long* out)
{
    const char* p = text;
    if (*p == '+') {
        p++;
    }
    if (*p == '\0') {
        return false;
    }
    unsigned long value = 0;
    for (; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        unsigned long digit = (unsigned long)(*p - '0');
        if (value > (ULONG_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    if (value < min || value > max) {
        return false;
    }
    *out = value;
    return true;
}
