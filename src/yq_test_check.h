/*
 * yq_test_check.h — assertion macros for the yq-DB test binaries.
 *
 * The suite is built with -DCMAKE_BUILD_TYPE=Release in CI, and CMake adds
 * -DNDEBUG to the Release flags, which turns every assert() into a no-op: a
 * failing test still prints its OK lines and exits 0, so CI stays green.
 * That is exactly how a broken engine (see the reader-slot CAS fix) shipped.
 *
 * These macros do not depend on NDEBUG: they report the failing expression
 * with file and line, then exit non-zero so ctest sees a real failure in any
 * build type.
 */

#ifndef YQ_TEST_CHECK_H
#define YQ_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "\nFAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            fflush(stderr);                                                \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

/*
 * Compare two integer expressions and print both values on failure. Most
 * assertions in the suite compare a return code against an expected one, and
 * "rc == 0" is far less useful than "rc == 17 (reader slots full)".
 */
#define CHECK_EQ(actual, expected)                                         \
    do {                                                                   \
        int64_t a_ = (int64_t)(actual);                                    \
        int64_t e_ = (int64_t)(expected);                                  \
        if (a_ != e_) {                                                    \
            fprintf(stderr, "\nFAIL: %s:%d: %s == %s (%" PRId64 " != %" PRId64 ")\n", \
                    __FILE__, __LINE__, #actual, #expected, a_, e_);       \
            fflush(stderr);                                                \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

#endif /* YQ_TEST_CHECK_H */
