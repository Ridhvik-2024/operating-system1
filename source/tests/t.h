/* tests/t.h — minimal check harness for AI-OS tests. */
#ifndef AIOS_T_H
#define AIOS_T_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;
__attribute__((unused)) static int g_skips = 0;

#define SKIP(...) do { \
        g_skips++; \
        fprintf(stderr, "SKIP "); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
    } while (0)

#define CHECK(cond, msg) do {                                   \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_failed++;                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n",                 \
                    __FILE__, __LINE__, msg);                   \
        }                                                       \
    } while (0)

#define CHECK_STR_CONTAINS(hay, needle, msg) do {               \
        g_checks++;                                             \
        const char *hay_ = (const char *)(hay);                 \
        if (!hay_ || !strstr(hay_, (needle))) {                 \
            g_failed++;                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n",                 \
                    __FILE__, __LINE__, msg);                   \
        }                                                       \
    } while (0)

#define TEST_MAIN()                                             \
    int main(void) {                                            \
        run();                                                  \
        (void)g_skips;                                          \
        if (g_skips)                                            \
            printf("%s: %d checks, %d failed (%d skipped)\n",   \
                   g_failed ? "FAIL" : "PASS", g_checks,        \
                   g_failed, g_skips);                          \
        else                                                    \
            printf("%s: %d checks, %d failed\n",                \
                   g_failed ? "FAIL" : "PASS", g_checks, g_failed); \
        return g_failed ? 1 : 0;                                \
    }

#endif /* AIOS_T_H */
