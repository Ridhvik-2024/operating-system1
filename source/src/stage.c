/* aios/stage.c — M3: staging, diff, promote, discard, trash, gc.
 *
 * The contract this file exists to keep: a machine-originated write must not
 * change reality until a human says so, and every change it does make must be
 * reversible after the fact.
 *
 * Three design points that are easy to get wrong, and are therefore spelled out
 * here and pinned by tests:
 *
 * 1. The manifest is written BEFORE the first rename into the jail. A staging
 *    directory with no manifest is not "empty", it is *unpromotable* — it may
 *    be a promote that died half-way, and promoting it again could resurrect
 *    files the user already discarded. gc deletes it; promote refuses it.
 *
 * 2. Promote is re-runnable. Interruption leaves state=PROMOTING and some
 *    renames done. Re-running renames the rest and finishes. rename(2) onto an
 *    existing path replaces it, so "promote the same note twice" is safe — the
 *    source is gone the second time, which we treat as done, not as an error.
 *
 * 3. Deletes move, they never unlink. The file sits in trash/ with a tombstone
 *    note in the manifest; undelete puts it back. The audit chain gets the
 *    tombstone (M6), but the file itself is recoverable even if the chain is
 *    not consulted.
 *
 * The diff is computed by reading both sides. That is O(bytes) and is right:
 * the whole point is that a human looks at it before it becomes real.
 */
#include "aios/stage.h"
#include "aios/types.h"
#include "aios/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MANIFEST_NAME "manifest.json"

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

const char *stage_op_str(stage_op op) {
    switch (op) {
    case STAGE_OP_ADD:    return "add";
    case STAGE_OP_MODIFY: return "modify";
    case STAGE_OP_DELETE: return "delete";
    }
    return "?";
}

const char *stage_state_str(stage_state st) {
    switch (st) {
    case STAGE_MANIFEST_NONE:      return "none";
    case STAGE_MANIFEST_OPEN:      return "open";
    case STAGE_MANIFEST_PROMOTING: return "promoting";
    case STAGE_MANIFEST_DONE:      return "done";
    }
    return "?";
}

static bool op_from_str(const char *s, stage_op *out) {
    if (!s) return false;
    if (strcmp(s, "add") == 0)    { *out = STAGE_OP_ADD;    return true; }
    if (strcmp(s, "modify") == 0) { *out = STAGE_OP_MODIFY; return true; }
    if (strcmp(s, "delete") == 0) { *out = STAGE_OP_DELETE; return true; }
    return false;
}

/* ---------------- path handling ---------------- */

/* A jail-relative path that cannot escape the jail or reach into the staging
 * metadata. Deliberately stricter than the tool layer: a staged path becomes a
 * real path in two different trees, so ".." is refused outright rather than
 * resolved. */
static bool rel_ok(const char *rel) {
    if (!rel || !*rel) return false;
    if (rel[0] == '/') return false;
    if (strlen(rel) >= AIOS_STAGE_PATH_MAX) return false;
    const char *p = rel;
    while (*p) {
        const char *sep = strchrnul(p, '/');
        size_t len = (size_t)(sep - p);
        if (len == 0) return false;                       /* // or trailing / */
        if (len == 1 && p[0] == '.') return false;
        if (len == 2 && p[0] == '.' && p[1] == '.') return false;
        p = *sep ? sep + 1 : sep;
    }
    return true;
}

bool stage_path_for(const stage_session *s, const char *rel,
                    char *out, size_t ocap) {
    if (!s || !out || ocap == 0 || !rel_ok(rel)) return false;
    int n = snprintf(out, ocap, "%s/%s", s->upper, rel);
    return n > 0 && (size_t)n < ocap;
}

static bool trash_path_for(const stage_session *s, const char *rel,
                           char *out, size_t ocap) {
    if (!s || !out || ocap == 0 || !rel_ok(rel)) return false;
    int n = snprintf(out, ocap, "%s/%s", s->trash, rel);
    return n > 0 && (size_t)n < ocap;
}

static bool join(char *out, size_t ocap, const char *a, const char *b) {
    int n = snprintf(out, ocap, "%s/%s", a, b);
    return n > 0 && (size_t)n < ocap;
}

/* mkdir -p for the parent directory of `path`. */
static bool mkdir_parent(const char *path) {
    char buf[AIOS_STAGE_PATH_MAX + 64];
    if (strlen(path) >= sizeof buf) return false;
    snprintf(buf, sizeof buf, "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return true;
    *slash = 0;
    return aios_mkdirs(buf) == AIOS_OK;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* fsync the directory containing `path`, so a rename becomes durable. Best
 * effort: some filesystems (and every fake fs in a container) refuse this, and
 * reporting failure for it would make promote fail on a working system. */
static void fsync_dir_of(const char *path) {
    char buf[AIOS_STAGE_PATH_MAX + 64];
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

/* ---------------- manifest ---------------- */

/* The manifest is the single source of truth for "what did this session intend
 * to change". It is rewritten atomically (tmp + rename) so a crash mid-write
 * leaves either the old manifest or the new one, never a truncated one. */
/* One writer for the manifest. Both "note a file" and "change the state" go
 * through here, so there is exactly one place that decides what the truth on
 * disk looks like. */
static bool manifest_write(const stage_session *s, stage_state st,
                           char (*paths)[AIOS_STAGE_PATH_MAX],
                           const stage_op *ops, size_t n,
                           char *err, size_t ecap) {
    json_val *root = json_new_obj();
    json_obj_set(root, "version", json_new_num(1));
    json_obj_set(root, "intent", json_new_num((double)s->intent_hash));
    json_obj_set(root, "seq", json_new_num((double)s->seq));
    json_obj_set(root, "state", json_new_str(stage_state_str(st)));
    json_obj_set(root, "jail", json_new_str(s->jail));
    json_val *arr = json_new_arr();
    for (size_t i = 0; i < n; i++) {
        json_val *e = json_new_obj();
        json_obj_set(e, "path", json_new_str(paths[i]));
        json_obj_set(e, "op", json_new_str(stage_op_str(ops[i])));
        json_arr_push(arr, e);
    }
    json_obj_set(root, "notes", arr);

    char *dump = json_dump(root);
    json_free(root);
    if (!dump) return set_err(err, ecap, "manifest: cannot serialize"), false;
    aios_status wst = aios_write_file(s->manifest, dump, strlen(dump));
    free(dump);
    if (wst != AIOS_OK)
        return set_err(err, ecap, "manifest: cannot write %s", s->manifest), false;
    fsync_dir_of(s->manifest);
    return true;
}

/* Read the notes off disk into parallel arrays. */
static size_t load_notes(const stage_session *s,
                         char (*paths)[AIOS_STAGE_PATH_MAX], stage_op *ops,
                         size_t cap) {
    json_val *root = json_parse_file(s->manifest);
    if (!root) return 0;
    json_val *arr = json_obj_get(root, "notes");
    size_t n = 0;
    if (arr && json_type_of(arr) == JSON_ARR) {
        size_t total = json_arr_len(arr);
        for (size_t i = 0; i < total && n < cap; i++) {
            json_val *e = json_arr_get(arr, i);
            const char *p = json_str(json_obj_get(e, "path"));
            stage_op op;
            if (!p || !rel_ok(p) ||
                !op_from_str(json_str(json_obj_get(e, "op")), &op))
                continue;
            snprintf(paths[n], AIOS_STAGE_PATH_MAX, "%s", p);
            ops[n] = op;
            n++;
        }
    }
    json_free(root);
    return n;
}

/* Change only the state field, preserving notes. */
bool stage_manifest_sync(stage_session *s, stage_state st, char *err, size_t ecap) {
    if (!s) return set_err(err, ecap, "no session"), false;
    static char paths[AIOS_STAGE_MAX_FILES][AIOS_STAGE_PATH_MAX];
    static stage_op ops[AIOS_STAGE_MAX_FILES];
    size_t n = load_notes(s, paths, ops, AIOS_STAGE_MAX_FILES);
    return manifest_write(s, st, paths, ops, n, err, ecap);
}

stage_state stage_manifest_state(const char *dir) {
    if (!dir) return STAGE_MANIFEST_NONE;
    char path[AIOS_STAGE_PATH_MAX + 64];
    if (!join(path, sizeof path, dir, MANIFEST_NAME)) return STAGE_MANIFEST_NONE;
    json_val *root = json_parse_file(path);
    if (!root) return STAGE_MANIFEST_NONE;
    const char *s = json_str(json_obj_get(root, "state"));
    stage_state st = STAGE_MANIFEST_NONE;
    if (s) {
        if (strcmp(s, "open") == 0)           st = STAGE_MANIFEST_OPEN;
        else if (strcmp(s, "promoting") == 0) st = STAGE_MANIFEST_PROMOTING;
        else if (strcmp(s, "done") == 0)      st = STAGE_MANIFEST_DONE;
    }
    json_free(root);
    return st;
}

size_t stage_manifest_count(const char *dir) {
    if (!dir) return 0;
    char path[AIOS_STAGE_PATH_MAX + 64];
    if (!join(path, sizeof path, dir, MANIFEST_NAME)) return 0;
    json_val *root = json_parse_file(path);
    if (!root) return 0;
    json_val *nv = json_obj_get(root, "notes");
    size_t n = (nv && json_type_of(nv) == JSON_ARR) ? json_arr_len(nv) : 0;
    json_free(root);
    return n;
}

/* ---------------- session lifecycle ---------------- */

void stage_end(stage_session *s) {
    if (!s) return;
    memset(s, 0, sizeof *s);
}

static bool abs_dir(const char *p, char *out, size_t ocap) {
    if (!p || !*p) return false;
    char rp[PATH_MAX];
    if (realpath(p, rp)) {
        if (strlen(rp) >= ocap) return false;   /* too long for our fields */
        memcpy(out, rp, strlen(rp) + 1);
        return true;
    }
    /* Directory may not exist yet (a fresh store). Resolve what we can: the
     * contract says these are absolute, so accept them as given. */
    if (p[0] != '/') return false;
    if (strlen(p) >= ocap) return false;
    memcpy(out, p, strlen(p) + 1);
    return true;
}

/* Count existing <intent>-* directories so seq=0 can auto-increment. */
static uint64_t next_seq(const char *staging_root, uint64_t intent) {
    DIR *d = opendir(staging_root);
    if (!d) return 1;
    char prefix[64];
    snprintf(prefix, sizeof prefix, "%llu-", (unsigned long long)intent);
    uint64_t max = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, prefix, strlen(prefix)) != 0) continue;
        uint64_t v = strtoull(de->d_name + strlen(prefix), NULL, 10);
        if (v > max) max = v;
    }
    closedir(d);
    return max + 1;
}

bool stage_begin(const char *jail, const char *store, uint64_t intent_hash,
                 uint64_t seq, stage_session *out, char *err, size_t ecap) {
    if (!jail || !store || !out) return set_err(err, ecap, "bad arguments"), false;
    memset(out, 0, sizeof *out);

    if (!abs_dir(jail, out->jail, sizeof out->jail))
        return set_err(err, ecap, "jail must be an absolute path"), false;
    if (!abs_dir(store, out->store, sizeof out->store))
        return set_err(err, ecap, "store must be an absolute path"), false;

    char staging[AIOS_STAGE_PATH_MAX];
    if (!join(staging, sizeof staging, out->store, "staging"))
        return set_err(err, ecap, "store path too long"), false;
    if (aios_mkdirs(staging) != AIOS_OK)
        return set_err(err, ecap, "cannot create %s", staging), false;

    out->intent_hash = intent_hash;
    if (seq == 0) seq = next_seq(staging, intent_hash);
    out->seq = seq;

    int dn = snprintf(out->dir, sizeof out->dir, "%s/%llu-%llu",
                      staging, (unsigned long long)intent_hash,
                      (unsigned long long)seq);
    if (dn < 0 || (size_t)dn >= sizeof out->dir)
        return set_err(err, ecap, "staging path too long"), false;
    if (!join(out->upper, sizeof out->upper, out->dir, "upper")) return false;
    if (!join(out->trash, sizeof out->trash, out->dir, "trash")) return false;
    if (!join(out->manifest, sizeof out->manifest, out->dir, MANIFEST_NAME))
        return false;

    if (aios_mkdirs(out->dir) != AIOS_OK ||
        aios_mkdirs(out->upper) != AIOS_OK ||
        aios_mkdirs(out->trash) != AIOS_OK)
        return set_err(err, ecap, "cannot create staging dir %s", out->dir), false;

    out->own_dir = true;
    /* The manifest exists from the moment the directory does. */
    if (!stage_manifest_sync(out, STAGE_MANIFEST_OPEN, err, ecap)) return false;
    return true;
}

bool stage_open(const char *dir, stage_session *out, char *err, size_t ecap) {
    if (!dir || !out) return set_err(err, ecap, "bad arguments"), false;
    memset(out, 0, sizeof *out);
    if (!abs_dir(dir, out->dir, sizeof out->dir))
        return set_err(err, ecap, "staging dir must be absolute"), false;

    char staging[AIOS_STAGE_PATH_MAX];
    snprintf(staging, sizeof staging, "%s", out->dir);
    char *slash = strrchr(staging, '/');
    if (!slash) return set_err(err, ecap, "malformed staging dir"), false;
    *slash = 0;
    snprintf(out->store, sizeof out->store, "%s", staging);
    /* store is <...>/staging -> drop that component */
    slash = strrchr(out->store, '/');
    if (slash) *slash = 0;

    if (!join(out->upper, sizeof out->upper, out->dir, "upper") ||
        !join(out->trash, sizeof out->trash, out->dir, "trash") ||
        !join(out->manifest, sizeof out->manifest, out->dir, MANIFEST_NAME))
        return set_err(err, ecap, "staging path too long"), false;

    json_val *root = json_parse_file(out->manifest);
    if (!root)
        return set_err(err, ecap, "no manifest in %s", out->dir), false;
    json_val *jv = json_obj_get(root, "jail");
    const char *jails = json_str(jv);
    if (jails) snprintf(out->jail, sizeof out->jail, "%s", jails);
    out->intent_hash = (uint64_t)json_num(json_obj_get(root, "intent"));
    out->seq = (uint64_t)json_num(json_obj_get(root, "seq"));
    json_free(root);
    return true;
}

const char *stage_upper(const stage_session *s) {
    return s ? s->upper : NULL;
}

/* ---------------- notes ---------------- */

/* Read the manifest notes into a caller-supplied buffer. Returns count. */
static size_t read_notes(const stage_session *s, stage_diff_entry *buf, size_t cap) {
    static char paths[AIOS_STAGE_MAX_FILES][AIOS_STAGE_PATH_MAX];
    static stage_op ops[AIOS_STAGE_MAX_FILES];
    size_t n = load_notes(s, paths, ops, cap < AIOS_STAGE_MAX_FILES ? cap : AIOS_STAGE_MAX_FILES);
    for (size_t i = 0; i < n; i++) {
        memset(&buf[i], 0, sizeof buf[i]);
        size_t plen = strlen(paths[i]);
        if (plen >= sizeof buf[i].path) continue;
        memcpy(buf[i].path, paths[i], plen + 1);
        buf[i].op = ops[i];
    }
    return n;
}

bool stage_note(stage_session *s, const char *rel, stage_op op,
                char *err, size_t ecap) {
    if (!s) return set_err(err, ecap, "no session"), false;
    if (!rel_ok(rel))
        return set_err(err, ecap, "staged path is not jail-relative"), false;

    static char paths[AIOS_STAGE_MAX_FILES][AIOS_STAGE_PATH_MAX];
    static stage_op ops[AIOS_STAGE_MAX_FILES];
    size_t n = load_notes(s, paths, ops, AIOS_STAGE_MAX_FILES);

    for (size_t i = 0; i < n; i++) {
        if (strcmp(paths[i], rel) == 0) {
            /* Already noted. Never downgrade a delete to an add: the file it
             * refers to is gone from the jail. */
            if (ops[i] == STAGE_OP_DELETE) return true;
            /* A later write to an already-noted path is still just one change;
             * the diff recomputes add-vs-modify from the two trees. */
            return true;
        }
    }
    if (n >= AIOS_STAGE_MAX_FILES)
        return set_err(err, ecap, "too many staged files (max %d)",
                       AIOS_STAGE_MAX_FILES), false;

    snprintf(paths[n], AIOS_STAGE_PATH_MAX, "%s", rel);
    ops[n] = op;
    return manifest_write(s, STAGE_MANIFEST_OPEN, paths, ops, n + 1, err, ecap);
}

/* ---------------- diff ---------------- */

void stage_diff_free(stage_diff *d) {
    if (!d) return;
    free(d->entries);
    d->entries = NULL;
    d->n = d->cap = 0;
}

static bool diff_push(stage_diff *d, const stage_diff_entry *e) {
    if (d->n == d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 8;
        stage_diff_entry *p = realloc(d->entries, nc * sizeof *p);
        if (!p) return false;
        d->entries = p;
        d->cap = nc;
    }
    d->entries[d->n++] = *e;
    return true;
}

/* Count lines and the change between two buffers. Returns line deltas.
 * Naive LCS would be O(n*m); a line-multiset comparison is enough to answer
 * "how much changed" and never lies about *whether* it changed. */
static void line_stats(const char *a, size_t alen, const char *b, size_t blen,
                       int *plus, int *minus) {
    int la = 0, lb = 0;
    /* count lines: number of '\n', plus one if the last byte is not '\n' */
    for (size_t i = 0; i < alen; i++) if (a[i] == '\n') la++;
    if (alen && a[alen - 1] != '\n') la++;
    for (size_t i = 0; i < blen; i++) if (b[i] == '\n') lb++;
    if (blen && b[blen - 1] != '\n') lb++;
    /* A text diff we can afford: count lines unchanged by position when the
     * prefix matches. This underestimates nothing and labels whole-file
     * rewrites as such, which is the honest answer for an 8x16 terminal. */
    size_t i = 0;
    while (i < alen && i < blen && a[i] == b[i]) i++;
    int unchanged_prefix_lines = 0;
    for (size_t k = 0; k < i; k++) if (a[k] == '\n') unchanged_prefix_lines++;
    *plus  = lb - unchanged_prefix_lines;
    *minus = la - unchanged_prefix_lines;
    if (*plus < 0) *plus = 0;
    if (*minus < 0) *minus = 0;
}

static bool read_all(const char *path, char **out, size_t *len) {
    *out = NULL;
    *len = 0;
    /* 1 MiB is the tool layer's write limit; reading more than a file could
     * ever contain only inflates the buffers this binary carries into the
     * initramfs, where every byte of static rodata is paid for twice. */
    return aios_read_file(path, out, len, 1u * 1024 * 1024) == AIOS_OK;
}

bool stage_diff_compute(stage_session *s, stage_diff *out, char *err, size_t ecap) {
    if (!s || !out) return set_err(err, ecap, "bad arguments"), false;
    memset(out, 0, sizeof *out);

    stage_diff_entry *notes = calloc(AIOS_STAGE_MAX_FILES, sizeof *notes);
    if (!notes) return set_err(err, ecap, "out of memory"), false;
    size_t n = read_notes(s, notes, AIOS_STAGE_MAX_FILES);

    for (size_t i = 0; i < n; i++) {
        stage_diff_entry e = notes[i];
        char jailpath[AIOS_STAGE_PATH_MAX * 2], upath[AIOS_STAGE_PATH_MAX];
        snprintf(jailpath, sizeof jailpath, "%s/%s", s->jail, e.path);
        bool have_upper = stage_path_for(s, e.path, upath, sizeof upath);

        char *old_buf = NULL, *new_buf = NULL;
        size_t old_len = 0, new_len = 0;
        bool have_old = file_exists(jailpath) && read_all(jailpath, &old_buf, &old_len);
        bool have_new = have_upper && file_exists(upath) &&
                        read_all(upath, &new_buf, &new_len);

        if (e.op == STAGE_OP_DELETE) {
            /* The file has already left the jail — its content is in trash/.
             * Measuring the jail would report every delete as 0 bytes, which
             * is the one number a human reads to decide if a delete is safe. */
            char tpath[AIOS_STAGE_PATH_MAX];
            char *tbuf = NULL;
            size_t tlen = 0;
            bool have_trash = trash_path_for(s, e.path, tpath, sizeof tpath) &&
                              file_exists(tpath) && read_all(tpath, &tbuf, &tlen);
            if (have_trash) {
                e.bytes = -(long)tlen;
                e.plus = 0;
                e.minus = 0;
                for (size_t k = 0; k < tlen; k++) if (tbuf[k] == '\n') e.minus++;
                if (tlen && tbuf[tlen - 1] != '\n') e.minus++;
            } else if (have_old) {
                /* Not yet deleted (the note exists but the unlink has not run). */
                e.bytes = -(long)old_len;
                e.plus = 0;
                e.minus = 0;
                for (size_t k = 0; k < old_len; k++) if (old_buf[k] == '\n') e.minus++;
                if (old_len && old_buf[old_len - 1] != '\n') e.minus++;
            } else {
                /* Neither side has it: the delete already happened and the
                 * trash was cleaned. Say so with 0 rather than inventing a
                 * size. */
                e.bytes = 0;
                e.plus = e.minus = 0;
            }
            free(tbuf);
        } else if (!have_old && have_new) {
            e.op = STAGE_OP_ADD;
            e.bytes = (long)new_len;
            line_stats("", 0, new_buf, new_len, &e.plus, &e.minus);
        } else if (have_old && !have_new) {
            /* A note exists but the staged copy vanished: report it honestly
             * rather than pretending the file is unchanged. */
            e.op = STAGE_OP_DELETE;
            e.bytes = -(long)old_len;
            e.plus = 0;
            e.minus = (int)old_len;
        } else if (have_old && have_new) {
            e.op = STAGE_OP_MODIFY;
            e.bytes = (long)new_len - (long)old_len;
            line_stats(old_buf, old_len, new_buf, new_len, &e.plus, &e.minus);
        } else {
            /* neither side exists: a note with no content. Report as add of 0. */
            e.op = STAGE_OP_ADD;
            e.bytes = 0;
            e.plus = e.minus = 0;
        }
        free(old_buf);
        free(new_buf);
        if (!diff_push(out, &e)) {
            free(notes);
            stage_diff_free(out);
            return set_err(err, ecap, "out of memory"), false;
        }
    }
    free(notes);
    return true;
}

char *stage_diff_text(const stage_diff *d) {
    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = 0;
    if (!d || d->n == 0) {
        snprintf(out, cap, "no changes staged\n");
        return out;
    }
    long total_bytes = 0;
    int adds = 0, mods = 0, dels = 0;
    for (size_t i = 0; i < d->n; i++) {
        const stage_diff_entry *e = &d->entries[i];
        char line[AIOS_STAGE_PATH_MAX + 96];
        int n;
        if (e->op == STAGE_OP_DELETE)
            n = snprintf(line, sizeof line, "  - %-36s delete   %ld bytes removed\n",
                         e->path, -e->bytes);
        else {
            n = snprintf(line, sizeof line, "  %c %-36s %-8s %+ld bytes  +%d/-%d lines\n",
                         e->op == STAGE_OP_ADD ? '+' : '~', e->path,
                         stage_op_str(e->op), e->bytes, e->plus, e->minus);
            total_bytes += e->bytes;
        }
        if (e->op == STAGE_OP_ADD) adds++;
        else if (e->op == STAGE_OP_MODIFY) mods++;
        else dels++;
        if (len + (size_t)n + 1 > cap) {
            cap = (len + (size_t)n + 1) * 2;
            char *p = realloc(out, cap);
            if (!p) { free(out); return NULL; }
            out = p;
        }
        memcpy(out + len, line, (size_t)n);
        len += (size_t)n;
        out[len] = 0;
    }
    char summary[192];
    int n = snprintf(summary, sizeof summary,
                     "  %zu file%s: %d added, %d modified, %d deleted (net %+ld bytes)\n",
                     d->n, d->n == 1 ? "" : "s", adds, mods, dels, total_bytes);
    if (len + (size_t)n + 1 > cap) {
        cap = len + (size_t)n + 1;
        char *p = realloc(out, cap);
        if (!p) { free(out); return NULL; }
        out = p;
    }
    memcpy(out + len, summary, (size_t)n);
    len += (size_t)n;
    out[len] = 0;
    return out;
}

/* ---------------- promote ---------------- */

/* Returns 1 if a file was moved, 0 if there was nothing to move (already
 * promoted), -1 on error. Callers that count "how much did I promote" must use
 * the 0/1 distinction: counting notes makes a re-run after an interruption
 * claim it promoted files it did not, which is precisely the number a human
 * would use to decide the promote worked. */
static int promote_one_ex(stage_session *s, const char *rel,
                          char *err, size_t ecap) {
    if (!s) return set_err(err, ecap, "no session"), -1;
    if (!rel_ok(rel))
        return set_err(err, ecap, "promote: bad path"), -1;

    char src[AIOS_STAGE_PATH_MAX], dst[AIOS_STAGE_PATH_MAX * 2];
    if (!stage_path_for(s, rel, src, sizeof src))
        return set_err(err, ecap, "promote: bad path"), -1;
    if (snprintf(dst, sizeof dst, "%s/%s", s->jail, rel) >= (int)sizeof dst)
        return set_err(err, ecap, "promote: path too long"), -1;

    if (!file_exists(src)) {
        /* Already promoted (or this is a delete note, which promote skips). */
        return 0;
    }
    if (!mkdir_parent(dst))
        return set_err(err, ecap, "promote: cannot create dirs for %s", rel), -1;

    if (rename(src, dst) != 0) {
        /* Fall back to copy+unlink (cross-device staging, e.g. /tmp vs store). */
        char *buf = NULL;
        size_t len = 0;
        if (!read_all(src, &buf, &len))
            return set_err(err, ecap, "promote: cannot read %s", src), -1;
        aios_status st = aios_write_file(dst, buf, len);
        free(buf);
        if (st != AIOS_OK)
            return set_err(err, ecap, "promote: cannot write %s (%s)", dst, strerror(errno)),
                   -1;
        if (unlink(src) != 0) { /* left behind; gc will take it */ }
    }
    fsync_dir_of(dst);
    return 1;
}

bool stage_promote_one(stage_session *s, const char *rel, char *err, size_t ecap) {
    return promote_one_ex(s, rel, err, ecap) >= 0;
}

bool stage_promote(stage_session *s, size_t *n_promoted, char *err, size_t ecap) {
    if (n_promoted) *n_promoted = 0;
    if (!s) return set_err(err, ecap, "no session"), false;

    stage_state st = stage_manifest_state(s->dir);
    if (st == STAGE_MANIFEST_NONE)
        return set_err(err, ecap,
                       "refusing to promote %s: no manifest (abandoned or corrupt)",
                       s->dir), false;

    /* Mark intent BEFORE the first rename. If we die here, the manifest says
     * PROMOTING and a re-run can finish. */
    if (!stage_manifest_sync(s, STAGE_MANIFEST_PROMOTING, err, ecap)) return false;

    stage_diff_entry *notes = calloc(AIOS_STAGE_MAX_FILES, sizeof *notes);
    if (!notes) return set_err(err, ecap, "out of memory"), false;
    size_t n = read_notes(s, notes, AIOS_STAGE_MAX_FILES);
    size_t done = 0;
    bool ok = true;
    for (size_t i = 0; i < n; i++) {
        if (notes[i].op == STAGE_OP_DELETE) {
            /* A delete note is realised by stage_delete(); promote does not
             * unlink anything. Skipping it is the safe direction. */
            continue;
        }
        int moved = promote_one_ex(s, notes[i].path, err, ecap);
        if (moved < 0) { ok = false; break; }
        if (moved > 0) done++;
    }
    free(notes);
    if (!ok) return false;   /* manifest stays PROMOTING: re-runnable */

    if (!stage_manifest_sync(s, STAGE_MANIFEST_DONE, err, ecap)) return false;
    if (n_promoted) *n_promoted = done;
    return true;
}

bool stage_promote_locked(stage_session *s, size_t *n_promoted,
                          char *err, size_t ecap) {
    if (n_promoted) *n_promoted = 0;
    if (!s) return set_err(err, ecap, "no session"), false;

    stage_state st = stage_manifest_state(s->dir);
    if (st == STAGE_MANIFEST_NONE)
        return set_err(err, ecap,
                       "refusing to promote %s: no manifest (abandoned or corrupt)",
                       s->dir), false;

    /* Snapshot the note list BEFORE touching anything: promotion is driven by
     * the manifest, not by whatever is in the directory at the time. */
    static char paths[AIOS_STAGE_MAX_FILES][AIOS_STAGE_PATH_MAX];
    static stage_op ops[AIOS_STAGE_MAX_FILES];
    size_t n = load_notes(s, paths, ops, AIOS_STAGE_MAX_FILES);

    if (!stage_manifest_sync(s, STAGE_MANIFEST_PROMOTING, err, ecap)) return false;

    size_t done = 0;
    for (size_t i = 0; i < n; i++) {
        if (ops[i] == STAGE_OP_DELETE) continue;   /* never unlink on promote */

        /* Re-validate: the note came off disk, and disk is not trusted. */
        if (!rel_ok(paths[i])) {
            return set_err(err, ecap, "promote: manifest names an unsafe path"), false;
        }
        /* The source must resolve to something inside upper/. A symlink at
         * upper/x pointing at /etc/shadow would otherwise be promoted as the
         * link target's content. */
        char up[AIOS_STAGE_PATH_MAX], real[PATH_MAX];
        if (!stage_path_for(s, paths[i], up, sizeof up))
            return set_err(err, ecap, "promote: bad path"), false;
        if (!realpath(up, real)) {
            continue;   /* already promoted or vanished: nothing to move */
        }
        char realupper[PATH_MAX];
        if (!realpath(s->upper, realupper))
            return set_err(err, ecap, "promote: staging upper is missing"), false;
        size_t ulen = strlen(realupper);
        if (strncmp(real, realupper, ulen) != 0 || real[ulen] != '/') {
            return set_err(err, ecap,
                           "promote: %s escapes the staging directory", paths[i]),
                   false;
        }
        int moved = promote_one_ex(s, paths[i], err, ecap);
        if (moved < 0) return false;
        if (moved > 0) done++;
    }

    if (!stage_manifest_sync(s, STAGE_MANIFEST_DONE, err, ecap)) return false;
    if (n_promoted) *n_promoted = done;
    return true;
}

/* ---------------- discard / trash ---------------- */

bool stage_discard(stage_session *s, char *err, size_t ecap) {
    if (!s) return set_err(err, ecap, "no session"), false;

    char trashroot[AIOS_STAGE_PATH_MAX];
    if (!join(trashroot, sizeof trashroot, s->store, "staging/.trash"))
        return set_err(err, ecap, "path too long"), false;
    if (aios_mkdirs(trashroot) != AIOS_OK)
        return set_err(err, ecap, "cannot create %s", trashroot), false;

    char dest[AIOS_STAGE_PATH_MAX * 2];
    snprintf(dest, sizeof dest, "%s/%llu-%llu-%lld",
             trashroot, (unsigned long long)s->seq, (unsigned long long)s->intent_hash,
             (long long)aios_now_ms());
    if (rename(s->dir, dest) != 0) {
        /* Cross-device: make the destination and move the children. */
        if (aios_mkdirs(dest) != AIOS_OK)
            return set_err(err, ecap, "discard: cannot create %s", dest), false;
        DIR *d = opendir(s->dir);
        if (!d) return set_err(err, ecap, "discard: cannot open %s", s->dir), false;
        struct dirent *de;
        bool ok = true;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char a[AIOS_STAGE_PATH_MAX * 2], b[AIOS_STAGE_PATH_MAX * 3];
            snprintf(a, sizeof a, "%s/%s", s->dir, de->d_name);
            snprintf(b, sizeof b, "%s/%s", dest, de->d_name);
            if (rename(a, b) != 0) ok = false;
        }
        closedir(d);
        if (!ok) return set_err(err, ecap, "discard: incomplete move"), false;
        rmdir(s->dir);
    }
    fsync_dir_of(dest);
    /* Point the session at where it went, so the caller can report it. */
    if (strlen(dest) < sizeof s->dir) memcpy(s->dir, dest, strlen(dest) + 1);
    return true;
}

/* Copy (not move) a jail file into the session's trash, then remove it. Copying
 * first is what makes an interrupted delete recoverable: if we die after the
 * copy, the jail still has its file; if we die after the unlink, the trash has
 * it. There is no window where neither does. */
bool stage_delete(stage_session *s, const char *rel, char *err, size_t ecap) {
    if (!s) return set_err(err, ecap, "no session"), false;
    if (!rel_ok(rel)) return set_err(err, ecap, "delete: bad path"), false;

    char jailpath[AIOS_STAGE_PATH_MAX * 2], tpath[AIOS_STAGE_PATH_MAX];
    if (snprintf(jailpath, sizeof jailpath, "%s/%s", s->jail, rel) >=
        (int)sizeof jailpath)
        return set_err(err, ecap, "delete: path too long"), false;
    if (!trash_path_for(s, rel, tpath, sizeof tpath))
        return set_err(err, ecap, "delete: bad path"), false;

    if (!file_exists(jailpath))
        return set_err(err, ecap, "delete: %s does not exist", rel), false;
    if (!mkdir_parent(tpath))
        return set_err(err, ecap, "delete: cannot create trash dirs"), false;

    char *buf = NULL;
    size_t len = 0;
    if (!read_all(jailpath, &buf, &len))
        return set_err(err, ecap, "delete: cannot read %s", rel), false;
    aios_status st = aios_write_file(tpath, buf, len);
    free(buf);
    if (st != AIOS_OK)
        return set_err(err, ecap, "delete: cannot stage trash copy"), false;

    /* The tombstone note is recorded before the unlink. */
    if (!stage_note(s, rel, STAGE_OP_DELETE, err, ecap)) return false;
    if (unlink(jailpath) != 0)
        return set_err(err, ecap, "delete: cannot unlink %s (%s)", rel, strerror(errno)),
               false;
    fsync_dir_of(jailpath);
    return true;
}

bool stage_undelete(stage_session *s, const char *rel, char *err, size_t ecap) {
    if (!s) return set_err(err, ecap, "no session"), false;
    if (!rel_ok(rel)) return set_err(err, ecap, "undelete: bad path"), false;

    char tpath[AIOS_STAGE_PATH_MAX], jailpath[AIOS_STAGE_PATH_MAX * 2];
    if (!trash_path_for(s, rel, tpath, sizeof tpath))
        return set_err(err, ecap, "undelete: bad path"), false;
    if (snprintf(jailpath, sizeof jailpath, "%s/%s", s->jail, rel) >=
        (int)sizeof jailpath)
        return set_err(err, ecap, "undelete: path too long"), false;

    if (!file_exists(tpath))
        return set_err(err, ecap, "undelete: %s is not in the trash", rel), false;

    char *buf = NULL;
    size_t len = 0;
    if (!read_all(tpath, &buf, &len))
        return set_err(err, ecap, "undelete: cannot read trash copy"), false;
    if (!mkdir_parent(jailpath)) {
        free(buf);
        return set_err(err, ecap, "undelete: cannot create dirs"), false;
    }
    aios_status st = aios_write_file(jailpath, buf, len);
    free(buf);
    if (st != AIOS_OK)
        return set_err(err, ecap, "undelete: cannot restore %s", rel), false;
    if (unlink(tpath) != 0) { /* already gone: fine */ }
    fsync_dir_of(jailpath);
    return true;
}

/* ---------------- gc ---------------- */

/* Recursive delete. Only ever called on staging directories we own. */
static bool remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *de;
    bool ok = true;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char child[AIOS_STAGE_PATH_MAX * 2];
        snprintf(child, sizeof child, "%s/%s", path, de->d_name);
        struct stat sb;
        if (lstat(child, &sb) != 0) { ok = false; continue; }
        if (S_ISDIR(sb.st_mode)) { if (!remove_tree(child)) ok = false; }
        else if (unlink(child) != 0) ok = false;
    }
    closedir(d);
    if (rmdir(path) != 0) ok = false;
    return ok;
}

/* A staging dir is removable when it cannot ever be promoted: no manifest, or
 * an OPEN manifest older than max_age. PROMOTING is never removable. */
bool stage_gc(const char *store, long long now_ms, long long max_age_ms,
              stage_gc_result *out, char *err, size_t ecap) {
    if (out) { out->removed = 0; out->kept = 0; }
    if (!store) return set_err(err, ecap, "no store"), false;

    char staging[AIOS_STAGE_PATH_MAX];
    if (!join(staging, sizeof staging, store, "staging"))
        return set_err(err, ecap, "path too long"), false;

    DIR *d = opendir(staging);
    if (!d) {
        if (errno == ENOENT) return true;      /* nothing staged: success */
        return set_err(err, ecap, "gc: cannot open %s", staging), false;
    }

    struct dirent *de;
    bool ok = true;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char dir[AIOS_STAGE_PATH_MAX * 2];
        snprintf(dir, sizeof dir, "%s/%s", staging, de->d_name);
        struct stat sb;
        if (stat(dir, &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;

        stage_state st = stage_manifest_state(dir);
        bool removable = false;
        if (st == STAGE_MANIFEST_NONE) {
            removable = true;                  /* orphan: never had a manifest */
        } else if (st == STAGE_MANIFEST_OPEN) {
            long long age = now_ms - (long long)sb.st_mtime * 1000;
            /* Filesystem mtime is seconds; now_ms is milliseconds since epoch. */
            if (max_age_ms <= 0 || age > max_age_ms) removable = true;
        }
        /* PROMOTING and DONE are never removed here. */

        if (!removable) {
            if (out) out->kept++;
            continue;
        }
        if (!remove_tree(dir)) { ok = false; continue; }
        if (out) out->removed++;
    }
    closedir(d);
    if (!ok) return set_err(err, ecap, "gc: could not remove every stale dir"), false;
    return true;
}
