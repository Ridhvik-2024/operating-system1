/* aios/types.c — M1 core utilities. */
#include "aios/types.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <libgen.h>

const char *aios_status_str(aios_status s) {
    switch (s) {
    case AIOS_OK:          return "ok";
    case AIOS_ERR:         return "error";
    case AIOS_ERR_ARG:     return "invalid-argument";
    case AIOS_ERR_NOMEM:   return "out-of-memory";
    case AIOS_ERR_IO:      return "io-error";
    case AIOS_ERR_STATE:   return "bad-state";
    case AIOS_ERR_TIMEOUT: return "timeout";
    case AIOS_ERR_TOOL:    return "tool-error";
    case AIOS_ERR_NOTFOUND:return "not-found";
    case AIOS_ERR_LIMIT:   return "limit-exceeded";
    }
    return "unknown";
}

double aios_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

double aios_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

uint64_t aios_fnv1a(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

aios_status aios_mkdirs(const char *path) {
    if (!path || !*path) return AIOS_ERR_ARG;
    char tmp[1024];
    if (strlen(path) >= sizeof tmp) return AIOS_ERR_ARG;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return AIOS_ERR_IO;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return AIOS_ERR_IO;
    return AIOS_OK;
}

aios_status aios_write_file(const char *path, const char *data, size_t len) {
    if (!path || !data) return AIOS_ERR_ARG;
    char tmp[1200];
    if (strlen(path) + 8 >= sizeof tmp) return AIOS_ERR_ARG;
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return AIOS_ERR_IO;
    bool ok = fwrite(data, 1, len, f) == len;
    if (fflush(f) != 0) ok = false;
    if (fsync(fileno(f)) != 0) ok = false;
    fclose(f);
    if (!ok) { remove(tmp); return AIOS_ERR_IO; }
    if (rename(tmp, path) != 0) { remove(tmp); return AIOS_ERR_IO; }
    return AIOS_OK;
}

aios_status aios_read_file(const char *path, char **out, size_t *out_len,
                           size_t max_len) {
    if (!path || !out) return AIOS_ERR_ARG;
    *out = NULL;
    if (out_len) *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? AIOS_ERR_NOTFOUND : AIOS_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return AIOS_ERR_IO; }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > max_len) { fclose(f); return AIOS_ERR_LIMIT; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return AIOS_ERR_NOMEM; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return AIOS_ERR_IO;
    }
    buf[sz] = 0;
    fclose(f);
    *out = buf;
    if (out_len) *out_len = (size_t)sz;
    return AIOS_OK;
}
