/* src/snapshot.c — M5: snapshots and rollback.
 *
 * The design decisions are in include/aios/snapshot.h. What is worth repeating
 * here is the ordering, because every failure mode of this file is an ordering
 * failure:
 *
 *   take:    copy into <id>.tmp/ -> fsync each file -> fsync dirs
 *            -> write manifest.json -> rename <id>.tmp -> <id> -> write DONE
 *
 * A snapshot directory is therefore either not there, or there and incomplete
 * (no DONE), or complete. The rename is what makes "there" atomic; the DONE is
 * what makes "complete" atomic. Without the rename a reader can see a half-built
 * directory; without DONE it can see a full one whose last file is missing.
 *
 *   restore: copy the snapshot out to <jail>.restore and <store>.restore
 *            -> rename the live trees to snapshots/.replaced/<ts>/
 *            -> rename the copies into place
 *
 * Both renames are on the same filesystem by construction (the .restore copies
 * are siblings of their targets), so each is atomic, and the window between them
 * is a crash away from "current state is in .replaced" -- recoverable, visible,
 * and not silent.
 */
#include "aios/snapshot.h"
#include "aios/types.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PATHBUF 1024

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}


/* Join two path components. Returns false -- and leaves out EMPTY rather than
 * truncated -- when the result does not fit.
 *
 * This exists because the alternative, snprintf("%s/%s", a, b) into a fixed
 * buffer, silently produces a truncated path, and a truncated path is worse than
 * a failed one: it names a DIFFERENT file that may well exist. Every function
 * here treats a false return as "this path cannot be used", which is the honest
 * answer.
 */
static bool jpath(char *out, size_t cap, const char *a, const char *b) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    if (la + 1 + lb + 1 > cap) return false;
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb + 1);
    return true;
}


/* A path NEXT TO `path`, named `path` + suffix -- e.g. "/a/jail" + ".restore"
 * is "/a/jail.restore", a sibling, not "/a/jail/.restore" which is a child and
 * therefore dragged along when the parent is renamed. */
static bool sibling_path(char *out, size_t cap, const char *path, const char *suffix) {
    if (!out || cap == 0 || !path || !suffix) return false;
    size_t lp = strlen(path), ls = strlen(suffix);
    if (lp + ls + 1 > cap) return false;
    memcpy(out, path, lp);
    memcpy(out + lp, suffix, ls + 1);
    return true;
}

static void fsync_dir(const char *path) {
    char buf[PATHBUF];
    if (strlen(path) >= sizeof buf) return;
    snprintf(buf, sizeof buf, "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash) return;
    *slash = 0;
    int fd = open(buf, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return;
    (void)fsync(fd);
    close(fd);
}

/* Copy one regular file. Returns false on any failure -- including a short read
 * or a short write, which is the case a naive fread/fwrite loop turns into a
 * silently truncated backup. */
static bool copy_file(const char *src, const char *dst, unsigned long long *bytes,
                      char *err, size_t ecap) {
    int in = open(src, O_RDONLY);
    if (in < 0) {
        set_err(err, ecap, "open %.400s: %s", src, strerror(errno));
        return false;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        set_err(err, ecap, "create %.400s: %s", dst, strerror(errno));
        return false;
    }
    char buf[65536];
    bool ok = true;
    unsigned long long total = 0;
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) continue;
            set_err(err, ecap, "read %.400s: %s", src, strerror(errno));
            ok = false;
            break;
        }
        if (r == 0) break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                set_err(err, ecap, "write %.400s: %s", dst, strerror(errno));
                ok = false;
                break;
            }
            off += w;
        }
        if (!ok) break;
        total += (unsigned long long)r;
    }
    if (ok && fsync(out) != 0) {
        set_err(err, ecap, "fsync %.400s: %s", dst, strerror(errno));
        ok = false;
    }
    close(in);
    close(out);
    if (!ok) unlink(dst);
    if (bytes) *bytes += total;
    return ok;
}

/* Recursively copy a directory. Skips any entry named in `skip_top` when it is
 * at the top level, which is how the store's own snapshots/ and staging/ are
 * excluded -- a snapshot that contains the snapshots directory recurses until
 * the disk fills. */
static bool copy_tree(const char *src, const char *dst,
                      const char *const *skip_top, size_t *n_files,
                      unsigned long long *bytes, char *err, size_t ecap) {
    if (aios_mkdirs(dst) != AIOS_OK) {
        set_err(err, ecap, "mkdir %.400s: %s", dst, strerror(errno));
        return false;
    }
    DIR *d = opendir(src);
    if (!d) {
        set_err(err, ecap, "opendir %.400s: %s", src, strerror(errno));
        return false;
    }
    bool ok = true;
    struct dirent *de;
    while (ok && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        bool skip = false;
        for (size_t i = 0; skip_top && skip_top[i]; i++)
            if (strcmp(de->d_name, skip_top[i]) == 0) skip = true;
        if (skip) continue;

        char s[PATHBUF], t[PATHBUF];
        int sn = snprintf(s, sizeof s, "%s/%s", src, de->d_name);
        int tn = snprintf(t, sizeof t, "%s/%s", dst, de->d_name);
        if (sn < 0 || (size_t)sn >= sizeof s || tn < 0 || (size_t)tn >= sizeof t) {
            set_err(err, ecap, "path too long under %.300s", src);
            ok = false;
            break;
        }

        struct stat st;
        if (lstat(s, &st) != 0) {
            set_err(err, ecap, "stat %.400s: %s", s, strerror(errno));
            ok = false;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            ok = copy_tree(s, t, NULL, n_files, bytes, err, ecap);
        } else if (S_ISLNK(st.st_mode)) {
            /* Copied as a link, never followed: following would pull the target
             * into the snapshot and could escape the tree entirely. */
            char target[PATHBUF];
            ssize_t len = readlink(s, target, sizeof target - 1);
            if (len < 0) {
                set_err(err, ecap, "readlink %.400s: %s", s, strerror(errno));
                ok = false;
                break;
            }
            target[len] = 0;
            unlink(t);
            if (symlink(target, t) != 0) {
                set_err(err, ecap, "symlink %.400s: %s", t, strerror(errno));
                ok = false;
                break;
            }
            if (n_files) (*n_files)++;
        } else if (S_ISREG(st.st_mode)) {
            if (!copy_file(s, t, bytes, err, ecap)) { ok = false; break; }
            if (n_files) (*n_files)++;
        }
        /* Sockets, fifos and devices are not state: skipped, deliberately. */
    }
    closedir(d);
    if (ok) fsync_dir(dst);
    return ok;
}


/* Copy every entry of `src` into `dst`, replacing same-named entries and leaving
 * anything already in `dst` that `src` does not mention. This is how a restored
 * store is merged: the store's own bookkeeping (snapshots/, .replaced/) must
 * survive a restore, because it is the record of the restore itself. */
static bool merge_dir(const char *src, const char *dst, char *err, size_t ecap) {
    DIR *d = opendir(src);
    if (!d) {
        set_err(err, ecap, "opendir %.400s: %s", src, strerror(errno));
        return false;
    }
    bool ok = true;
    struct dirent *de;
    while (ok && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char s[PATHBUF], t[PATHBUF];
        if (!jpath(s, sizeof s, src, de->d_name) || !jpath(t, sizeof t, dst, de->d_name)) {
            set_err(err, ecap, "path too long under %.300s", src);
            ok = false;
            break;
        }
        struct stat st;
        if (lstat(s, &st) != 0) { ok = false; break; }
        if (S_ISDIR(st.st_mode)) {
            ok = merge_dir(s, t, err, ecap);
        } else {
            unlink(t);
            if (rename(s, t) != 0) {
                set_err(err, ecap, "cannot restore %.400s: %s", t, strerror(errno));
                ok = false;
            }
        }
    }
    closedir(d);
    return ok;
}

/* Remove a tree. Used on failure paths and by prune. */
static void rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
                char child[PATHBUF];
                int n = snprintf(child, sizeof child, "%s/%s", path, de->d_name);
                if (n < 0 || (size_t)n >= sizeof child) continue;
                rm_rf(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* ---------------- tree digest ---------------- */

typedef struct { char *path; } dpath;

static int cmp_path(const void *a, const void *b) {
    return strcmp(((const dpath *)a)->path, ((const dpath *)b)->path);
}

static bool collect_paths(const char *root, const char *rel, dpath **v,
                          size_t *n, size_t *cap) {
    char dir[PATHBUF];
    int dn = rel && *rel ? snprintf(dir, sizeof dir, "%s/%s", root, rel)
                         : snprintf(dir, sizeof dir, "%s", root);
    if (dn < 0 || (size_t)dn >= sizeof dir) return false;

    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *de;
    bool ok = true;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char childrel[PATHBUF], full[PATHBUF];
        int rn = (rel && *rel) ? snprintf(childrel, sizeof childrel, "%s/%s", rel, de->d_name)
                               : snprintf(childrel, sizeof childrel, "%s", de->d_name);
        int fn = snprintf(full, sizeof full, "%s/%s", root, childrel);
        if (rn < 0 || (size_t)rn >= sizeof childrel || fn < 0 || (size_t)fn >= sizeof full)
            continue;

        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!collect_paths(root, childrel, v, n, cap)) { ok = false; break; }
        } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            if (*n == *cap) {
                size_t ncap = *cap ? *cap * 2 : 32;
                dpath *nv = realloc(*v, ncap * sizeof **v);
                if (!nv) { ok = false; break; }
                *v = nv;
                *cap = ncap;
            }
            (*v)[*n].path = strdup(childrel);
            if (!(*v)[*n].path) { ok = false; break; }
            (*n)++;
        }
    }
    closedir(d);
    return ok;
}

bool snap_tree_digest(const char *root, unsigned long long *digest,
                      size_t *n_files, unsigned long long *n_bytes,
                      char *err, size_t ecap) {
    if (!root || !digest) return false;
    dpath *v = NULL;
    size_t n = 0, cap = 0;
    struct stat rs;
    if (stat(root, &rs) != 0) {
        set_err(err, ecap, "no tree at %.400s", root);
        free(v);
        return false;
    }
    if (!collect_paths(root, "", &v, &n, &cap)) {
        set_err(err, ecap, "cannot walk %.400s", root);
        for (size_t i = 0; i < n; i++) free(v[i].path);
        free(v);
        return false;
    }
    qsort(v, n, sizeof *v, cmp_path);

    /* Path, then content, sequentially, so a rename-only change still moves the
     * digest -- and a file that moves from one directory to another is a change
     * of state even though the bytes are the same. */
    uint64_t h = 1469598103934665603ULL;   /* FNV offset basis, same as aios_fnv1a */
    unsigned long long total = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < n; i++) {
        h = (h ^ 0xff) * 1099511628211ULL;            /* separator */
        uint64_t ph = aios_fnv1a(v[i].path, strlen(v[i].path));
        for (int b = 0; b < 8; b++) {
            h = (h ^ ((ph >> (b * 8)) & 0xff)) * 1099511628211ULL;
        }

        char full[PATHBUF];
        snprintf(full, sizeof full, "%s/%s", root, v[i].path);
        struct stat st;
        if (lstat(full, &st) != 0) { ok = false; break; }

        if (S_ISLNK(st.st_mode)) {
            char target[PATHBUF];
            ssize_t len = readlink(full, target, sizeof target - 1);
            if (len < 0) { ok = false; break; }
            uint64_t th = aios_fnv1a(target, (size_t)len);
            for (int b = 0; b < 8; b++)
                h = (h ^ ((th >> (b * 8)) & 0xff)) * 1099511628211ULL;
            continue;
        }

        int fd = open(full, O_RDONLY);
        if (fd < 0) { ok = false; break; }
        char buf[65536];
        ssize_t r;
        while ((r = read(fd, buf, sizeof buf)) > 0) {
            for (ssize_t i2 = 0; i2 < r; i2++)
                h = (h ^ (unsigned char)buf[i2]) * 1099511628211ULL;
            total += (unsigned long long)r;
        }
        close(fd);
        if (r < 0) { ok = false; break; }
    }
    for (size_t i = 0; i < n; i++) free(v[i].path);
    free(v);
    if (!ok) {
        set_err(err, ecap, "cannot read a file under %.300s", root);
        return false;
    }
    *digest = h;
    if (n_files) *n_files = n;
    if (n_bytes) *n_bytes = total;
    return true;
}

/* ---------------- manifest ---------------- */

static bool write_manifest(const char *dir, const snap_info *si,
                           char *err, size_t ecap) {
    char path[PATHBUF], tmp[PATHBUF];
    jpath(path, sizeof path, dir, "manifest.json");
    jpath(tmp, sizeof tmp, dir, "manifest.json.tmp");

    char body[1024];
    int n = snprintf(body, sizeof body,
                     "{\"version\":1,\"id\":\"%s\",\"label\":\"%s\","
                     "\"taken_ms\":%lld,\"files\":%zu,\"bytes\":%llu,"
                     "\"jail_digest\":\"%016llx\",\"store_digest\":\"%016llx\"}\n",
                     si->id, si->label, si->taken_ms, si->files, si->bytes,
                     si->jail_digest, si->store_digest);
    if (n < 0 || (size_t)n >= sizeof body) {
        set_err(err, ecap, "manifest too large");
        return false;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        set_err(err, ecap, "write manifest: %s", strerror(errno));
        return false;
    }
    bool ok = fwrite(body, 1, (size_t)n, f) == (size_t)n;
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!ok) {
        set_err(err, ecap, "write manifest: short write");
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        set_err(err, ecap, "rename manifest: %s", strerror(errno));
        unlink(tmp);
        return false;
    }
    fsync_dir(dir);
    return true;
}

/* Read one field out of a manifest without a JSON dependency: the file is
 * written by this file and has exactly one shape, and a full parser here would
 * be a second implementation of a thing that already has one. */
static bool manifest_field(const char *text, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(text, pat);
    if (!p) return false;
    p += strlen(pat);
    if (*p == '"') {
        p++;
        const char *e = strchr(p, '"');
        if (!e) return false;
        size_t len = (size_t)(e - p);
        if (len >= cap) len = cap - 1;
        memcpy(out, p, len);
        out[len] = 0;
        return true;
    }
    const char *e = p;
    while (*e && *e != ',' && *e != '}') e++;
    size_t len = (size_t)(e - p);
    if (len >= cap) len = cap - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

static bool read_manifest(const char *dir, snap_info *si) {
    char path[PATHBUF];
    jpath(path, sizeof path, dir, "manifest.json");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;

    char v[128];
    if (manifest_field(buf, "id", si->id, sizeof si->id) == false) return false;
    manifest_field(buf, "label", si->label, sizeof si->label);
    if (manifest_field(buf, "taken_ms", v, sizeof v)) si->taken_ms = atoll(v);
    if (manifest_field(buf, "files", v, sizeof v)) si->files = (size_t)atoll(v);
    if (manifest_field(buf, "bytes", v, sizeof v)) si->bytes = strtoull(v, NULL, 10);
    if (manifest_field(buf, "jail_digest", v, sizeof v))
        si->jail_digest = strtoull(v, NULL, 16);
    if (manifest_field(buf, "store_digest", v, sizeof v))
        si->store_digest = strtoull(v, NULL, 16);

    char done[PATHBUF];
    jpath(done, sizeof done, dir, "DONE");
    si->complete = (access(done, F_OK) == 0);
    return true;
}

/* ---------------- take ---------------- */

/* Store children that must never be copied into a snapshot: the snapshots
 * themselves, the replaced trees, and the transient staging area. */
static const char *STORE_SKIP[] = {
    "snapshots", "staging", "current.jail", NULL
};

bool snap_take(const char *jail, const char *store, const char *label,
               char *id_out, size_t id_cap, char *err, size_t ecap) {
    if (!jail || !store || !id_out || id_cap == 0) return false;
    id_out[0] = 0;

    char snaps[PATHBUF], tmp[PATHBUF], final[PATHBUF];
    jpath(snaps, sizeof snaps, store, "snapshots");
    if (aios_mkdirs(snaps) != AIOS_OK) {
        set_err(err, ecap, "cannot create %.400s: %s", snaps, strerror(errno));
        return false;
    }

    snap_info si;
    memset(&si, 0, sizeof si);
    si.taken_ms = (long long)aios_now_ms();
    if (label && *label) snprintf(si.label, sizeof si.label, "%s", label);
    /* Timestamp + pid keeps two snapshots in the same millisecond distinct, and
     * a pid is one of the few things guaranteed to differ between two shells. */
    snprintf(si.id, sizeof si.id, "%lld-%d", si.taken_ms, (int)getpid());
    snprintf(id_out, id_cap, "%s", si.id);

    char tmpname[AIOS_SNAP_ID_MAX + 8];
    snprintf(tmpname, sizeof tmpname, "%s.tmp", si.id);
    jpath(tmp, sizeof tmp, snaps, tmpname);
    jpath(final, sizeof final, snaps, si.id);
    rm_rf(tmp);

    char jailcopy[PATHBUF], storecopy[PATHBUF];
    jpath(jailcopy, sizeof jailcopy, tmp, "jail");
    jpath(storecopy, sizeof storecopy, tmp, "store");

    size_t files = 0;
    unsigned long long bytes = 0;

    if (!copy_tree(jail, jailcopy, NULL, &files, &bytes, err, ecap)) {
        rm_rf(tmp);
        return false;
    }
    if (!copy_tree(store, storecopy, STORE_SKIP, &files, &bytes, err, ecap)) {
        rm_rf(tmp);
        return false;
    }

    si.files = files;
    si.bytes = bytes;
    /* Digest the copies, not the originals: the manifest must describe what was
     * actually captured. If the tree changed during the copy, the digest says so
     * and a later compare catches it. */
    char derr[192] = {0};
    if (!snap_tree_digest(jailcopy, &si.jail_digest, NULL, NULL, derr, sizeof derr)) {
        set_err(err, ecap, "digest jail copy: %s", derr);
        rm_rf(tmp);
        return false;
    }
    if (!snap_tree_digest(storecopy, &si.store_digest, NULL, NULL, derr, sizeof derr)) {
        set_err(err, ecap, "digest store copy: %s", derr);
        rm_rf(tmp);
        return false;
    }
    si.complete = true;

    if (!write_manifest(tmp, &si, err, ecap)) {
        rm_rf(tmp);
        return false;
    }
    fsync_dir(tmp);

    /* The rename is what makes the snapshot appear atomically. */
    if (rename(tmp, final) != 0) {
        set_err(err, ecap, "rename snapshot: %s", strerror(errno));
        rm_rf(tmp);
        return false;
    }
    fsync_dir(snaps);

    /* DONE last. Between the rename and this write the snapshot is visible and
     * incomplete -- which is exactly the state a reader must treat as "do not
     * restore this", and it is why read_manifest checks for DONE rather than
     * trusting the directory's existence. */
    char done[PATHBUF];
    jpath(done, sizeof done, final, "DONE");
    FILE *d = fopen(done, "wb");
    if (!d) {
        set_err(err, ecap, "cannot mark snapshot complete: %s", strerror(errno));
        return false;
    }
    fputs("complete\n", d);
    bool ok = fflush(d) == 0 && fsync(fileno(d)) == 0;
    fclose(d);
    if (!ok) {
        set_err(err, ecap, "cannot fsync snapshot marker");
        return false;
    }
    fsync_dir(final);
    return true;
}

/* ---------------- list / find ---------------- */

void snap_list_free(snap_list *l) {
    if (!l) return;
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

static int cmp_info_desc(const void *a, const void *b) {
    const snap_info *x = a, *y = b;
    if (x->taken_ms != y->taken_ms) return x->taken_ms < y->taken_ms ? 1 : -1;
    return strcmp(y->id, x->id);
}

bool snap_list_all(const char *store, snap_list *out, char *err, size_t ecap) {
    if (!store || !out) return false;
    memset(out, 0, sizeof *out);
    char snaps[PATHBUF];
    jpath(snaps, sizeof snaps, store, "snapshots");
    DIR *d = opendir(snaps);
    if (!d) return true;               /* no snapshots yet is not an error */

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strstr(de->d_name, ".tmp")) continue;
        char dir[PATHBUF];
        jpath(dir, sizeof dir, snaps, de->d_name);
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        snap_info si;
        memset(&si, 0, sizeof si);
        if (!read_manifest(dir, &si)) continue;
        if (out->n == out->cap) {
            size_t ncap = out->cap ? out->cap * 2 : 8;
            snap_info *ni = realloc(out->items, ncap * sizeof *ni);
            if (!ni) {
                set_err(err, ecap, "out of memory");
                return false;
            }
            out->items = ni;
            out->cap = ncap;
        }
        out->items[out->n++] = si;
    }
    closedir(d);
    qsort(out->items, out->n, sizeof *out->items, cmp_info_desc);
    return true;
}

bool snap_find(const char *store, const char *id_or_prefix, snap_info *out,
               char *err, size_t ecap) {
    if (!store || !id_or_prefix || !out) return false;
    snap_list l;
    if (!snap_list_all(store, &l, err, ecap)) return false;

    /* `latest` / `micro` / `session` are names for "the newest", because that is
     * what a human means at a prompt after something went wrong. */
    if (strcmp(id_or_prefix, "latest") == 0 || strcmp(id_or_prefix, "micro") == 0 ||
        strcmp(id_or_prefix, "session") == 0) {
        for (size_t i = 0; i < l.n; i++) {
            if (!l.items[i].complete) continue;
            *out = l.items[i];
            snap_list_free(&l);
            return true;
        }
        set_err(err, ecap, "no complete snapshot to roll back to");
        snap_list_free(&l);
        return false;
    }

    size_t hits = 0;
    snap_info found;
    memset(&found, 0, sizeof found);
    for (size_t i = 0; i < l.n; i++) {
        if (strncmp(l.items[i].id, id_or_prefix, strlen(id_or_prefix)) == 0) {
            hits++;
            found = l.items[i];
        }
    }
    snap_list_free(&l);
    if (hits == 0) {
        set_err(err, ecap, "no snapshot matching \"%.64s\"", id_or_prefix);
        return false;
    }
    if (hits > 1) {
        /* Refusing is the only safe answer: restoring a snapshot the human did
         * not name is a state change nobody asked for. */
        set_err(err, ecap, "\"%.64s\" matches %zu snapshots; be more specific",
                id_or_prefix, hits);
        return false;
    }
    if (!found.complete) {
        set_err(err, ecap, "snapshot %.64s is incomplete; refusing to restore it",
                found.id);
        return false;
    }
    *out = found;
    return true;
}

/* ---------------- restore ---------------- */

bool snap_restore(const char *jail, const char *store, const char *id_or_prefix,
                  char *replaced_at, size_t replaced_cap,
                  char *err, size_t ecap) {
    if (!jail || !store || !id_or_prefix) return false;
    if (replaced_at && replaced_cap) replaced_at[0] = 0;

    snap_info si;
    if (!snap_find(store, id_or_prefix, &si, err, ecap)) return false;

    char snaps_dir[PATHBUF], snapdir[PATHBUF], snapsjail[PATHBUF], snapsstore[PATHBUF];
    if (!jpath(snaps_dir, sizeof snaps_dir, store, "snapshots")) {
        set_err(err, ecap, "store path too long");
        return false;
    }
    jpath(snapdir, sizeof snapdir, snaps_dir, si.id);
    jpath(snapsjail, sizeof snapsjail, snapdir, "jail");
    jpath(snapsstore, sizeof snapsstore, snapdir, "store");

    struct stat st;
    if (stat(snapsjail, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_err(err, ecap, "snapshot %.64s has no jail copy", si.id);
        return false;
    }

    /* Copy out first, into siblings of the targets so the later renames are on
     * one filesystem and therefore atomic. If anything fails here the live trees
     * have not been touched. */
    /* The staging copies MUST be siblings of their targets. They used to be,
     * and then the image put the workspace on the ext2 side disk while the
     * snapshot ran from the initramfs: rename() of a directory across two
     * filesystems answers EXDEV, and the error -- "cannot move the current jail
     * aside: Invalid cross-device link" -- reads like a permissions problem
     * rather than a layout one.
     *
     * Siblings make each put-into-place an atomic rename on one filesystem.
     * `jail` and `store` are absolute by contract, so the parent directory is
     * well defined. */
    char jrestore[PATHBUF], srestore[PATHBUF];
    if (!sibling_path(jrestore, sizeof jrestore, jail, ".restore") ||
        !sibling_path(srestore, sizeof srestore, store, ".restore")) {
        set_err(err, ecap, "jail or store path too long");
        return false;
    }
    rm_rf(jrestore);
    rm_rf(srestore);

    if (!copy_tree(snapsjail, jrestore, NULL, NULL, NULL, err, ecap)) {
        rm_rf(jrestore);
        return false;
    }
    if (!copy_tree(snapsstore, srestore, NULL, NULL, NULL, err, ecap)) {
        rm_rf(jrestore);
        rm_rf(srestore);
        return false;
    }

    /* The current state is PRESERVED, never deleted: copied to .replaced/ before
     * anything is swapped. It has to be a copy rather than a rename because the
     * snapshots live inside the store, so the store cannot be moved out from
     * under them -- rename() answers EINVAL, which is how this was found.
     *
     * Copying costs O(bytes) on a restore, which is acceptable: restores are
     * rare, and "the thing I just undid is still recoverable" is worth more than
     * the milliseconds. An 8 MiB disk is the real constraint, and prune is the
     * answer to that, not skipping the backup. */
    /* The replaced state goes NEXT TO the store, not inside it. Inside would put
     * it on the store's filesystem, and the jail's backup has to be on the
     * JAIL's filesystem or the rename that preserves it answers EXDEV. Keeping
     * both backups as siblings of their targets means each is on the same
     * filesystem as the thing it protects, which is the only arrangement where
     * "the current state is preserved" is a rename rather than a copy that can
     * fail half-way. */
    char replaced[PATHBUF];
    if (!sibling_path(replaced, sizeof replaced, store, ".replaced")) {
        set_err(err, ecap, "store path too long");
        rm_rf(jrestore);
        rm_rf(srestore);
        return false;
    }
    {
        char stamp[48];
        snprintf(stamp, sizeof stamp, "%lld-%d",
                 (long long)aios_now_ms(), (int)getpid());
        char stamped[PATHBUF];
        if (!sibling_path(stamped, sizeof stamped, replaced, stamp)) {
            set_err(err, ecap, "replaced-state path too long");
            rm_rf(jrestore);
            rm_rf(srestore);
            return false;
        }
        snprintf(replaced, sizeof replaced, "%s", stamped);
    }
    if (aios_mkdirs(replaced) != AIOS_OK) {
        set_err(err, ecap, "cannot create %.400s: %s", replaced, strerror(errno));
        rm_rf(jrestore);
        rm_rf(srestore);
        return false;
    }
    char oldjail[PATHBUF], oldstore[PATHBUF];
    if (!jpath(oldjail, sizeof oldjail, replaced, "jail") ||
        !jpath(oldstore, sizeof oldstore, replaced, "store")) {
        set_err(err, ecap, "replaced-state path too long");
        rm_rf(jrestore);
        rm_rf(srestore);
        return false;
    }

    if (stat(jail, &st) == 0 &&
        !copy_tree(jail, oldjail, NULL, NULL, NULL, err, ecap)) {
        rm_rf(jrestore);
        rm_rf(srestore);
        rm_rf(replaced);
        return false;
    }
    if (stat(store, &st) == 0 &&
        !copy_tree(store, oldstore, STORE_SKIP, NULL, NULL, err, ecap)) {
        rm_rf(jrestore);
        rm_rf(srestore);
        rm_rf(replaced);
        return false;
    }
    if (replaced_at && replaced_cap) snprintf(replaced_at, replaced_cap, "%s", replaced);

    /* Now swap. Both replacements happen after both copies exist, so a failure
     * before this point has not touched the live trees at all. */
    char trashjail[PATHBUF], trashstore[PATHBUF];
    /* Sibling of the JAIL, not under `replaced`: the move of the live jail has
     * to be a rename on the jail's own filesystem. This is the same EXDEV that
     * the image's data disk caused, one level deeper. */
    if (!sibling_path(trashjail, sizeof trashjail, jail, ".old") ||
        !jpath(trashstore, sizeof trashstore, replaced, "old-store")) {
        set_err(err, ecap, "replaced-state path too long");
        rm_rf(jrestore);
        rm_rf(srestore);
        rm_rf(replaced);
        return false;
    }
    bool had_jail = (stat(jail, &st) == 0);
    bool had_store = (stat(store, &st) == 0);
    if (had_jail && rename(jail, trashjail) != 0) {
        set_err(err, ecap, "cannot move the current jail aside: %s", strerror(errno));
        rm_rf(jrestore);
        rm_rf(srestore);
        rm_rf(replaced);
        return false;
    }
    /* The store cannot be renamed (the snapshots are inside it), so its contents
     * are cleared by moving the store directory to a sibling and creating a new
     * one -- which needs the store's PARENT to be writable. Where that is not
     * possible, fail before touching anything rather than half-restore. */
    char storeaside[PATHBUF];
    if (!jpath(storeaside, sizeof storeaside, replaced, "store-aside")) {
        set_err(err, ecap, "replaced-state path too long");
        if (had_jail) rename(trashjail, jail);
        rm_rf(jrestore);
        rm_rf(srestore);
        rm_rf(replaced);
        return false;
    }
    (void)had_store;

    if (rename(jrestore, jail) != 0) {
        set_err(err, ecap,
                "cannot put the restored jail in place (%s); the previous state is "
                "in %.300s", strerror(errno), replaced);
        if (had_jail) rename(trashjail, jail);
        rm_rf(srestore);
        return false;
    }
    /* Merge the restored store back in: replace every entry that the snapshot
     * has, leave snapshots/ and staging/ alone (they are this process's own
     * bookkeeping, not restored state). */
    if (!merge_dir(srestore, store, err, ecap)) {
        set_err(err, ecap,
                "cannot put the restored store in place; the previous state is in "
                "%.300s", replaced);
        return false;
    }
    rm_rf(srestore);
    /* The preserved old jail is moved into the place `replaced_at` promises.
     * Leaving it as <jail>.old would mean the path we report does not contain
     * what we said it contains -- and a backup nobody can find is not a backup.
     * That move IS allowed to be a copy: it is housekeeping after the restore
     * has already succeeded. */
    {
        char kept[PATHBUF];
        if (jpath(kept, sizeof kept, replaced, "old-jail")) {
            /* A failure here is not a restore failure: the restore already
             * happened. It is a lost convenience copy, and the honest thing is to
             * leave the original in place rather than delete it. */
            bool kept_ok = copy_tree(trashjail, kept, NULL, NULL, NULL, NULL, 0);
            if (kept_ok) rm_rf(trashjail);
        } else {
            rm_rf(trashjail);
        }
    }
    (void)trashstore;
    (void)storeaside;
    return true;
}
/* ---------------- prune ---------------- */

int snap_prune(const char *store, size_t keep, char *err, size_t ecap) {
    if (!store) return -1;
    char snaps[PATHBUF];
    jpath(snaps, sizeof snaps, store, "snapshots");
    DIR *d = opendir(snaps);
    if (!d) return 0;

    /* Complete ones, newest first; then everything else is a candidate. */
    snap_list l;
    if (!snap_list_all(store, &l, err, ecap)) { closedir(d); return -1; }

    size_t kept = 0;
    int removed = 0;
    for (size_t i = 0; i < l.n; i++) {
        bool keep_it = l.items[i].complete && kept < keep;
        if (keep_it) { kept++; continue; }
        char dir[PATHBUF];
        jpath(dir, sizeof dir, snaps, l.items[i].id);
        rm_rf(dir);
        removed++;
    }

    /* Interrupted snapshots: a directory without a manifest, or a .tmp left by a
     * crash mid-copy. Neither can be restored, so neither should accumulate. */
    rewinddir(d);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        bool known = false;
        for (size_t i = 0; i < l.n; i++)
            if (strcmp(l.items[i].id, de->d_name) == 0) known = true;
        if (known) continue;
        char dir[PATHBUF];
        jpath(dir, sizeof dir, snaps, de->d_name);
        rm_rf(dir);
        removed++;
    }
    closedir(d);
    snap_list_free(&l);
    return removed;
}

/* ---------------- text ---------------- */

char *snap_list_text(const snap_list *l) {
    size_t cap = 256 + (l ? l->n : 0) * 200;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t off = 0;
    if (!l || l->n == 0) {
        snprintf(out, cap, "no snapshots (nothing to roll back to)");
        return out;
    }
    for (size_t i = 0; i < l->n; i++) {
        const snap_info *s = &l->items[i];
        off += (size_t)snprintf(out + off, cap - off,
                                "%-22s %8zu files %8llu bytes  %s%s\n",
                                s->id, s->files, s->bytes,
                                s->label[0] ? s->label : "(no label)",
                                s->complete ? "" : "  [INCOMPLETE]");
        if (off >= cap) break;
    }
    return out;
}
