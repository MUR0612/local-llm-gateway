/** @file test_main.c
 * @brief Unit test runner and malloc/calloc/realloc failure injection.
 */
#include <stdlib.h>

#include "testlib.h"

int g_failures = 0;
int g_checks = 0;
long g_alloc_fail_at = 0;
long g_alloc_count = 0;
int g_alloc_failed = 0;

void* __real_malloc(size_t size);
void* __real_calloc(size_t count, size_t size);
void* __real_realloc(void* ptr, size_t size);
void* __wrap_malloc(size_t size);
void* __wrap_calloc(size_t count, size_t size);
void* __wrap_realloc(void* ptr, size_t size);

void alloc_fail_reset(long failAt)
{
    g_alloc_fail_at = failAt;
    g_alloc_count = 0;
    g_alloc_failed = 0;
}

/** @return true when this allocation should fail. */
static int should_fail(void)
{
    g_alloc_count++;
    if (g_alloc_fail_at > 0 && g_alloc_count == g_alloc_fail_at) {
        g_alloc_failed = 1;
        return 1;
    }
    return 0;
}

void* __wrap_malloc(size_t size)
{
    return should_fail() ? NULL : __real_malloc(size);
}

void* __wrap_calloc(size_t count, size_t size)
{
    return should_fail() ? NULL : __real_calloc(count, size);
}

void* __wrap_realloc(void* ptr, size_t size)
{
    return should_fail() ? NULL : __real_realloc(ptr, size);
}

int main(void)
{
    test_buf_numparse();
    test_json();
    test_history();
    test_http();
    test_config();
    test_alloc_failures();
    printf("unit tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
