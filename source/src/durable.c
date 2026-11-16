/* aios/durable.c — M8: crash-consistent durable state on the ext2 side disk.
 * See aios/durable.h for the store layout and the crash-consistency rules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include "aios/durable.h"

static char g_root[512];

static void default_root(void) {
    const char *env = getenv("AIOS_DUR_ROOT");
    snprintf(g_root, sizeof g_root, "%s",
             env && *env ? env : AIOS_DUR_DEFAULT_ROOT);
}

void aios_dur_set_root(const char *root) {
    if (root && *root) {
        snprintf(g_root, sizeof g_root, "%s", root);
    } else {
        g_root[0] = 0;
        default_root();
    }
}

const char *aios_dur_root(void) {
    if (!g_root[0]) default_root();
    return g_root;
}

/* resolve <root>/<rel> into out (rel must not escape the root) */
static aios_status rel_path(const char *rel, char *out, size_t cap) {
    if (!rel || !*rel || rel[0] == '/' || strstr(rel, "..") == rel) {
        return AIOS_ERR_ARG;
    }
    if (snprintf(out, cap, "%s/%s", aios_dur_root(), rel) >= (int)cap) {
        return AIOS_ERR_ARG;
    }
    return AIOS_OK;
}

bool aios_dur_available(void) {
    struct stat st;
    if (stat(aios_dur_root(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (access(aios_dur_root(), W_OK) != 0) return false;
    return true;
}

aios_status aios_dur_fsync_path(const char *path) {
    if (!path) return AIOS_ERR_ARG;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return AIOS_ERR_IO;
    int rc = fsync(fd);
    close(fd);
    return rc == 0 ? AIOS_OK : AIOS_ERR_IO;
}

/* fsync the directory that holds path (rename/creation durability) */
static aios_status fsync_parent(const char *path) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return AIOS_ERR_ARG;
    *slash = 0;
    if (!*dir) strcpy(dir, "/");
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return AIOS_ERR_IO;
    int rc = fsync(fd);
    close(fd);
    return rc == 0 ? AIOS_OK : AIOS_ERR_IO;
}

aios_status aios_dur_write_atomic(const char *rel, const char *data,
                                  size_t len) {
    char path[1024], tmp[1100];
    if (rel_path(rel, path, sizeof path) != AIOS_OK) return AIOS_ERR_ARG;
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid())
        >= (int)sizeof tmp) return AIOS_ERR_ARG;

    char parent[1024];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = 0;
        if (mkdir(parent, 0755) != 0 && errno != EEXIST) return AIOS_ERR_IO;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return AIOS_ERR_IO;
    size_t off = 0;
    bool ok = true;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; ok = false; break; }
        off += (size_t)w;
    }
    if (ok && fsync(fd) != 0) ok = false;
    close(fd);
    if (!ok || rename(tmp, path) != 0) { remove(tmp); return AIOS_ERR_IO; }
    return fsync_parent(path);
}

aios_status aios_dur_append(const char *rel, const char *data, size_t len) {
    char path[1024];
    if (rel_path(rel, path, sizeof path) != AIOS_OK) return AIOS_ERR_ARG;
    bool existed = access(path, F_OK) == 0;
    char parent[1024];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = 0;
        if (mkdir(parent, 0755) != 0 && errno != EEXIST) return AIOS_ERR_IO;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return AIOS_ERR_IO;
    size_t off = 0;
    aios_status st = AIOS_OK;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            st = AIOS_ERR_IO;
            break;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) st = AIOS_ERR_IO;
    close(fd);
    /* a freshly created journal must be durable as a directory entry too */
    if (!existed && fsync_parent(path) != AIOS_OK) st = AIOS_ERR_IO;
    return st;
}

aios_status aios_dur_read(const char *rel, char *out, size_t cap,
                          size_t *out_len) {
    char path[1024];
    if (rel_path(rel, path, sizeof path) != AIOS_OK) return AIOS_ERR_ARG;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return AIOS_ERR_IO;
    size_t got = 0;
    while (got < cap) {
        ssize_t r = read(fd, out + got, cap - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return AIOS_ERR_IO;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (out_len) *out_len = got;
    return AIOS_OK;
}

aios_status aios_dur_mirror(const char *live_path, const char *rel) {
    if (!live_path) return AIOS_ERR_ARG;
    FILE *f = fopen(live_path, "rb");
    if (!f) return AIOS_ERR_IO;
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    bool truncated = ferror(f) || (n == sizeof buf && fgetc(f) != EOF);
    fclose(f);
    if (truncated) return AIOS_ERR_IO;
    return aios_dur_write_atomic(rel, buf, n);
}

/* copy any file src -> dest with full fsync discipline (atomic at dest) */
static aios_status copy_file(const char *src, const char *dest) {
    FILE *in = fopen(src, "rb");
    if (!in) return AIOS_ERR_IO;
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, in);
    bool bad = ferror(in) || (n == sizeof buf && fgetc(in) != EOF);
    fclose(in);
    if (bad) return AIOS_ERR_IO;
    FILE *f = fopen(dest, "wb");
    if (!f) return AIOS_ERR_IO;
    bool ok = fwrite(buf, 1, n, f) == n;
    if (ok) ok = fflush(f) == 0;
    if (ok) ok = fsync(fileno(f)) == 0;
    fclose(f);
    if (!ok) { remove(dest); return AIOS_ERR_IO; }
    return AIOS_OK;
}

aios_status aios_dur_restore_file(const char *rel, const char *dest) {
    char path[1024];
    if (rel_path(rel, path, sizeof path) != AIOS_OK) return AIOS_ERR_ARG;
    return copy_file(path, dest);
}

int aios_dur_restore_dir(const char *rel, const char *dest) {
    char sdir[1024];
    if (rel_path(rel, sdir, sizeof sdir) != AIOS_OK) return -1;
    DIR *d = opendir(sdir);
    if (!d) return -1;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char spath[1100], dpath[1100];
        if (snprintf(spath, sizeof spath, "%s/%s", sdir, e->d_name)
            >= (int)sizeof spath) continue;
        struct stat st;
        if (stat(spath, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (snprintf(dpath, sizeof dpath, "%s/%s", dest, e->d_name)
            >= (int)sizeof dpath) continue;
        if (copy_file(spath, dpath) == AIOS_OK) count++;
    }
    closedir(d);
    return count;
}

aios_status aios_dur_lines(const char *rel, size_t *n,
                           char *last, size_t cap) {
    static char buf[1 << 19];                    /* 512 KiB journal window */
    size_t len = 0;
    if (n) *n = 0;
    if (last && cap) last[0] = 0;
    if (aios_dur_read(rel, buf, sizeof buf, &len) != AIOS_OK) return AIOS_OK;
    size_t lines = 0;
    const char *last_line = NULL;
    size_t last_len = 0;
    size_t pos = 0;
    while (pos < len) {
        char *nl = (char *)memchr(buf + pos, '\n', len - pos);
        if (!nl) break;                          /* torn tail — ignore */
        last_line = buf + pos;
        last_len = (size_t)(nl - (buf + pos));
        pos = (size_t)(nl - buf) + 1;
        lines++;
    }
    if (n) *n = lines;
    if (last && cap) {
        size_t c = last_len < cap - 1 ? last_len : cap - 1;
        memcpy(last, last_line ? last_line : "", c);
        last[c] = 0;
    }
    return AIOS_OK;
}
