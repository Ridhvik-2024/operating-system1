/* aios/types.h — M1 core types, status codes, small utilities.
 * AI-OS: AI-native operating system. License: project-internal. */
#ifndef AIOS_TYPES_H
#define AIOS_TYPES_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIOS_VERSION_MAJOR 1
#define AIOS_VERSION_MINOR 1
#define AIOS_VERSION_PATCH 0
#define AIOS_VERSION "1.1.0-m10"

/* Status codes used across the runtime layer. */
typedef enum {
    AIOS_OK          = 0,
    AIOS_ERR         = -1,   /* generic error                       */
    AIOS_ERR_ARG     = -2,   /* invalid argument                    */
    AIOS_ERR_NOMEM   = -3,
    AIOS_ERR_IO      = -4,
    AIOS_ERR_STATE   = -5,   /* object in wrong state               */
    AIOS_ERR_TIMEOUT = -6,
    AIOS_ERR_TOOL    = -7,   /* tool failed inside sandbox          */
    AIOS_ERR_NOTFOUND= -8,
    AIOS_ERR_LIMIT   = -9    /* budget/limit exhausted              */
} aios_status;

const char *aios_status_str(aios_status s);

/* Milliseconds since epoch (CLOCK_REALTIME). */
double aios_now_ms(void);
/* Monotonic milliseconds (CLOCK_MONOTONIC) — for durations. */
double aios_mono_ms(void);

/* FNV-1a 64-bit hash (used by embeddings + audit digests). */
uint64_t aios_fnv1a(const void *data, size_t len);

/* mkdir -p equivalent. Returns AIOS_OK/ERR. */
aios_status aios_mkdirs(const char *path);

/* Write whole buffer to file atomically (tmp + rename). */
aios_status aios_write_file(const char *path, const char *data, size_t len);

/* Read whole file; caller frees *out (NUL-terminated). */
aios_status aios_read_file(const char *path, char **out, size_t *out_len,
                           size_t max_len);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_TYPES_H */
