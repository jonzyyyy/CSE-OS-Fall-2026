/*
 * framework.h - a minimal, dependency-free unit test runner.
 *
 * Each test runs in its own forked process, inside its own temporary
 * directory, with a timeout. This means:
 *   - a crash (segfault, SIGPIPE, ...) fails only that test, not the suite
 *   - a deadlock is reported as a timeout instead of hanging forever
 *   - files a test creates (e.g. conversation_*.jsonl) never leak between tests
 *
 * Tests can be marked as "expected to fail" (XFAIL) to document known bugs
 * without making the suite red. If an XFAIL test starts passing, the runner
 * reports XPASS and fails, as a reminder to remove the marker.
 */
#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Seconds before a single test is killed and reported as a timeout. */
#define TEST_TIMEOUT_SEC 10

typedef struct {
    const char *name;
    void (*fn)(void);
    const char *xfail_reason; /* NULL if the test is expected to pass */
} test_case_t;

#define TEST(fn)               { #fn, fn, NULL }
#define TEST_XFAIL(fn, reason) { #fn, fn, reason }

/* Fail the current test if cond is false. Safe to use from helper functions. */
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "    %s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #cond);                             \
            exit(1);                                                        \
        }                                                                   \
    } while (0)

/*
 * Run every test whose name contains argv[1] (or all tests if no argument).
 * Returns 0 if all results were as expected, 1 otherwise.
 */
int run_tests(const test_case_t *tests, size_t count, int argc, char **argv);

#endif
