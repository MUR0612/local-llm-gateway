/** @file numparse.h
 * @brief Strict unsigned decimal parsing for CLI and environment values.
 */
#ifndef LLMGW_NUMPARSE_H
#define LLMGW_NUMPARSE_H

#include <stdbool.h>

/**
 * Parses an optional '+' followed by decimal digits only, rejecting the
 * empty string, whitespace, '-', trailing garbage, overflow and values
 * outside [min, max]. Leading zeros are accepted.
 * @return true when valid (and stores the value in @p out).
 */
bool parse_bounded_uint(
        const char* text, unsigned long min, unsigned long max, unsigned long* out);

#endif
