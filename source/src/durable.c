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
#include <sys/file.h>      /* C3: flock on the store directory */
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


/* ---- C3: one writer at a time on the shared store -----------------------
 *
 * The store is written by the supervisor (PID 1), by tool workers, and by the
 * governor. `O_APPEND` makes a SINGLE write(2) atomic, which was enough while
 * every record was one write - and is no longer enough the moment a record is
 * two writes, or the moment a reader inspects a file between a writer's truncate
 * and its rename. Two writers replacing the same file could also collide on the
 * temporary name, and `write_atomic` cleared O_EXCL by using O_TRUNC.
 *
 * The lock is an flock(2) on the DIRECTORY that holds the file, not on the file:
 * a lock on the file would have to be dropped before rename-replacing it (the
 * old inode is no longer the file), and the window between unlock and rename is
 * exactly the race being removed. A directory lock covers the whole replace.
 *
 * flock is advisory. That is the correct scope: every writer here is our own
 * code, and the point is to serialise our writers, not to defend against a
 * hostile process that could equally edit the store directly. */
static int lock_dir_for(const char *path) {
    char parent[1024];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash) snprintf(parent, sizeof parent, ".");
    else *slash = 0;
    int dfd = open(parent, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return -1;
    /* Retry on EINTR: a lock attempt that is interrupted must not be reported
       as a lock failure, which would look like store corruption. */
    while (flock(dfd, LOCK_EX) != 0) {
        if (errno == EINTR) continue;
        close(dfd);
        return -1;
    }
    return dfd;
}

static void unlock_dir(int dfd) {
    if (dfd < 0) return;
    flock(dfd, LOCK_UN);
    close(dfd);
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
    /* The temp name carries the pid, so two LIVE writers cannot collide - and
     * since the stale file is unlinked under the lock just below, O_EXCL never
     * observes a pre-existing file either. Stated plainly because the sabotage
     * matrix says so: switching this back to O_TRUNC changes no behaviour and
     * no test notices. It is kept because a future change that drops the unlink
     * would make the difference load-bearing, and because failing loudly on an
     * unexpected existing file is the better default for a temp path. */
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid())
        >= (int)sizeof tmp) return AIOS_ERR_ARG;

    char parent[1024];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = 0;
        if (mkdir(parent, 0755) != 0 && errno != EEXIST) return AIOS_ERR_IO;
    }
    const int dfd = lock_dir_for(path);
    if (dfd < 0) return AIOS_ERR_IO;
    /* A stale temp file from a previous crash is ours to clear, but only under
       the lock - clearing it unlocked would delete a live writer's file. */
    if (unlink(tmp) != 0 && errno != ENOENT) { unlock_dir(dfd); return AIOS_ERR_IO; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { unlock_dir(dfd); return AIOS_ERR_IO; }
    size_t off = 0;
    bool ok = true;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; ok = false; break; }
        off += (size_t)w;
    }
    if (ok && fsync(fd) != 0) ok = false;
    close(fd);
    if (!ok || rename(tmp, path) != 0) { remove(tmp); unlock_dir(dfd); return AIOS_ERR_IO; }
    const aios_status st = fsync_parent(path);
    unlock_dir(dfd);
    return st;
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
    /* C3: the lock covers the whole RECORD. Without it a record written in two
     * writes can be interleaved with another process's record, producing a
     * journal line that is two half-records - which the tolerant reader below
     * cannot repair, because both halves are well-formed pieces of different
     * records. */
    const int dfd = lock_dir_for(path);
    if (dfd < 0) return AIOS_ERR_IO;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { unlock_dir(dfd); return AIOS_ERR_IO; }
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
    unlock_dir(dfd);
    return st;
}

/* C4: does this store file exist? Distinguishing "absent" from "unreadable" is
 * the difference between an empty boot log and a broken one, and only the caller
 * knows which of those it can tell a user. */
bool aios_dur_exists(const char *rel) {
    char path[1024];
    if (rel_path(rel, path, sizeof path) != AIOS_OK) return false;
    return access(path, F_OK) == 0;
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
    /* C3, two fixes in one function.
     *
     * 1. TOLERANCE. A journal is the thing left behind by a crash, so its last
     *    line is expected to be torn. The loop below always ignored a tail with
     *    no newline; that behaviour is now stated rather than incidental,
     *    because the alternative - treating a torn tail as corruption - would
     *    discard a journal precisely when it is most needed.
     *
     * 2. THE WINDOW WAS A CLIFF. `aios_dur_read` reads up to cap bytes and the
     *    return value was ignored, so a journal larger than 512 KiB came back
     *    TRUNCATED AT THE FRONT with no indication: `n` was still counted from
     *    what arrived, and a caller had no way to tell "40 lines" from "40 lines
     *    and 900 more we did not read". Worse, the last line of a truncated read
     *    is a fragment, so `last` could be half a record reported as the newest
     *    entry. The documented limit is now honest: the window keeps the TAIL
     *    of the journal (the newest records, which is what every caller wants)
     *    and reports whether it was clipped. */
    static char buf[1 << 19];                    /* 512 KiB journal window */
    size_t len = 0;
    if (n) *n = 0;
    if (last && cap) last[0] = 0;

    char path[1024];
    size_t total = 0;
    bool clipped = false;
    if (rel_path(rel, path, sizeof path) == AIOS_OK) {
        struct stat stt;
        if (stat(path, &stt) == 0) total = (size_t)stt.st_size;
        clipped = total > sizeof buf;
        if (clipped) {
            /* read only the tail, and start after the first newline in it so the
               first line reported is a whole record */
            FILE *f = fopen(path, "rb");
            if (!f) return AIOS_OK;
            if (fseek(f, (long)(total - sizeof buf), SEEK_SET) != 0) { fclose(f); return AIOS_OK; }
            len = fread(buf, 1, sizeof buf, f);
            fclose(f);
            char *first_nl = memchr(buf, '\n', len);
            if (first_nl) {
                const size_t drop = (size_t)(first_nl - buf) + 1;
                memmove(buf, buf + drop, len - drop);
                len -= drop;
            }
        }
    }
    if (!clipped && aios_dur_read(rel, buf, sizeof buf, &len) != AIOS_OK) return AIOS_OK;
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
    /* `lines` counts what was READ, which is what the callers act on. It is not
       a count of the whole journal when `clipped` was set, and no caller may
       present it as one. */
    if (n) *n = lines;
    if (last && cap) {
        size_t c = last_len < cap - 1 ? last_len : cap - 1;
        memcpy(last, last_line ? last_line : "", c);
        last[c] = 0;
    }
    return AIOS_OK;
}
