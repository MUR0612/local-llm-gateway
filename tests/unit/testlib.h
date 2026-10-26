/** @file testlib.h
 * @brief Tiny assertion helpers and allocation-failure injection for unit
 *        tests (linked with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc).
 */
#ifndef LLMGW_TESTLIB_H
#define LLMGW_TESTLIB_H

#include <stdio.h>
#include <string.h>

extern int g_failures;
extern int g_checks;

/**
 * Allocation number (1-based, counted from the last alloc_fail_reset())
 * that should fail; 0 disables injection.
 */
extern long g_alloc_fail_at;
/** Allocations seen since the last reset. */
extern long g_alloc_count;
/** Whether an injected failure has happened since the last reset. */
extern int g_alloc_failed;

void alloc_fail_reset(long failAt);

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
        }                                                                      \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                     \
    do {                                                                       \
        long long va_ = (long long)(a);                                        \
        long long vb_ = (long long)(b);                                        \
        g_checks++;                                                            \
        if (va_ != vb_) {                                                      \
            g_failures++;                                                      \
            fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__,    \
                    __LINE__, #a, va_, vb_);                                   \
        }                                                                      \
    } while (0)

#define CHECK_MEM(ptr, len, expected, expectedLen)                             \
    do {                                                                       \
        g_checks++;                                                            \
        if ((size_t)(len) != (size_t)(expectedLen)                             \
                || memcmp((ptr), (expected), (size_t)(len)) != 0) {            \
            g_failures++;                                                      \
            fprintf(stderr, "%s:%d: bytes differ for %s (len %zu vs %zu)\n",   \
                    __FILE__, __LINE__, #ptr, (size_t)(len),                   \
                    (size_t)(expectedLen));                                    \
        }                                                                      \
    } while (0)

#define CHECK_STR(actual, expected)                                            \
    CHECK_MEM((actual), strlen(actual), (expected), strlen(expected))

void test_buf_numparse(void);
void test_json(void);
void test_history(void);
void test_http(void);
void test_config(void);
void test_alloc_failures(void);

#endif
