/*
 * yq_test_apis.c — contract tests for the version and error-code API.
 *
 * These are pure ABI/contract checks that need no database:
 *
 *   1. yq_version() must agree with the YQ_VERSION_* macros, and must tolerate
 *      NULL outputs (the header promises "either argument may be NULL").
 *   2. yq_strerror() must return a real, non-empty description for every
 *      released error code (YQ_OK .. YQ_ERR_PANIC). Error codes are frozen in
 *      yq.h, so a missing entry is a contract break, not a typo — it would
 *      silently turn a real failure into an "unknown" string at the call site.
 *   3. Out-of-range codes must fall back to the "unknown error" placeholder.
 *
 * The binary returns 0 only when every check passes; CTest treats non-zero as
 * failure.
 */

#include <stdio.h>
#include <string.h>

#include "yq.h"

static int g_failures = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("  FAIL: ");                                            \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

int main(void) {
    /* --- yq_version() agrees with the published macros --- */
    int major = -1, minor = -1, patch = -1;
    CHECK(yq_version(&major, &minor, &patch) == YQ_OK,
          "yq_version() should return YQ_OK");
    CHECK(major == YQ_VERSION_MAJOR,
          "major %d != YQ_VERSION_MAJOR %d", major, YQ_VERSION_MAJOR);
    CHECK(minor == YQ_VERSION_MINOR,
          "minor %d != YQ_VERSION_MINOR %d", minor, YQ_VERSION_MINOR);
    CHECK(patch == YQ_VERSION_PATCH,
          "patch %d != YQ_VERSION_PATCH %d", patch, YQ_VERSION_PATCH);

    /* Null outputs are tolerated and the others are still filled in. */
    int minor_only = -1;
    CHECK(yq_version(NULL, &minor_only, NULL) == YQ_OK,
          "yq_version() must accept NULL arguments");
    CHECK(minor_only == YQ_VERSION_MINOR,
          "yq_version(NULL, &minor, NULL) left minor = %d (expected %d)",
          minor_only, YQ_VERSION_MINOR);

    /* --- yq_strerror() covers every released code 0..YQ_ERR_PANIC --- */
    for (int rc = 0; rc <= YQ_ERR_PANIC; rc++) {
        const char *msg = yq_strerror(rc);
        CHECK(msg != NULL, "yq_strerror(%d) must not be NULL", rc);
        CHECK(msg != NULL && msg[0] != '\0',
              "yq_strerror(%d) must not be empty", rc);
        CHECK(msg != NULL && strcmp(msg, "unknown error") != 0,
              "yq_strerror(%d) must not be the unknown placeholder", rc);
    }

    /* --- out-of-range codes fall back to "unknown error" --- */
    CHECK(strcmp(yq_strerror(-1), "unknown error") == 0,
          "yq_strerror(-1) should be the unknown placeholder");
    CHECK(strcmp(yq_strerror(YQ_ERR_PANIC + 1), "unknown error") == 0,
          "yq_strerror(%d) should be the unknown placeholder", YQ_ERR_PANIC + 1);
    CHECK(strcmp(yq_strerror(999), "unknown error") == 0,
          "yq_strerror(999) should be the unknown placeholder");

    if (g_failures == 0) {
        printf("apis: all version / error-code contract checks passed "
               "(version %d.%d.%d, %d error codes)\n",
               YQ_VERSION_MAJOR, YQ_VERSION_MINOR, YQ_VERSION_PATCH,
               YQ_ERR_PANIC + 1);
        return 0;
    }
    printf("apis: %d contract check(s) FAILED\n", g_failures);
    return 1;
}
