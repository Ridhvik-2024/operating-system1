/* aios/audit.c — M6: the append-only record, chained.
 *
 * Read include/aios/audit.h first. It says what this does not protect against,
 * which is the more important half: a chain that verifies only against itself is
 * a chain an attacker who owns the file can rewrite.
 *
 * Implementation notes worth knowing:
 *
 * 1. ONE RECORD IS ONE write() OF ONE COMPLETE LINE. Not a stdio stream of
 *    fprintf calls: a partial write from a process killed between two fprintfs
 *    leaves half a record, and the next append would then chain onto a line that
 *    is not a record. The line is built in memory, hashed, then written with a
 *    single write() under an exclusive lock, then fsync'd.
 *
 * 2. THE CANONICAL FORM IS THE BYTES. `hash` covers the record's bytes exactly as
 *    they are written, minus the `hash` field itself. That means the canonical
 *    form is not "re-serialise the JSON and hash that" -- re-serialising can
 *    legitimately differ (key order, number formatting) and would make a
 *    verifying implementation disagree with a writing one for reasons that have
 *    nothing to do with tampering. The hash is computed over the exact prefix
 *    `<prev>|<line up to the hash field>`.
 *
 * 3. `prev` IS READ FROM THE TAIL, NOT FROM MEMORY. A long-lived process holding
 *    a cached head would chain onto a value that another writer had already
 *    extended past, silently forking the chain. Reading the tail each time costs
 *    a seek to the end of the file and is the only correct answer when the file is
 *    the source of truth.
 *
 * 4. A TORN TAIL IS TRIMMED, NOT CHAINED ONTO. If the file ends in a partial
 *    line (a crash mid-append), the next append removes it before writing. Chaining
 *    onto a partial line would weave the garbage into the chain and make every
 *    later record unverifiable -- one crash would poison the log forever.
 */
#include "aios/audit.h"
#include "aios/json.h"
#include "aios/types.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

const char *audit_verdict_str(audit_verdict v) {
    switch (v) {
    case AUDIT_OK:          return "ok";
    case AUDIT_BAD_SEQ:     return "sequence is not continuous";
    case AUDIT_BAD_HASH:    return "record does not match its own hash";
    case AUDIT_BAD_LINK:    return "record does not link to its predecessor";
    case AUDIT_BAD_JSON:    return "record is not a JSON object";
    case AUDIT_TORN_TAIL:   return "the last record was never finished";
    case AUDIT_TOO_MANY:    return "more records than the check allows";
    case AUDIT_UNREADABLE:  return "the audit log could not be read";
    default:                return "unknown";
    }
}

/* ---- paths ---- */

static bool audit_path(char *out, size_t cap, const char *store) {
    if (!out || cap == 0 || !store || !*store) return false;
    int n = snprintf(out, cap, "%s/%s", store, AIOS_AUDIT_FILE);
    return n > 0 && (size_t)n < cap;
}


/* Remove `"key":<value>` from a JSON object's text, leaving a valid object.
 *
 * A caller that passes {"hash":"deadbeef…"} must not get to choose the chain
 * field: the whole point of the chain is that the writer computes it. The first
 * version of this simply appended its own `hash` after the caller's, producing a
 * record with two `hash` keys -- the parser then read whichever came first, the
 * `prev` the caller supplied was never replaced, and verification failed on a
 * record this module had just written. Silently dropping a duplicate key by
 * relying on parse order is not a defence; removing it is.
 */
static void strip_field(char *obj, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    for (;;) {
        char *k = strstr(obj, pat);
        if (!k) return;
        char *colon = strchr(k + strlen(pat), ':');
        if (!colon) return;
        char *v = colon + 1;
        while (*v == ' ') v++;
        char *endp = v;
        if (*v == '"') {
            endp = v + 1;
            while (*endp && *endp != '"') { if (*endp == '\\') endp++; endp++; }
            if (*endp == '"') endp++;
        } else {
            while (*endp && *endp != ',' && *endp != '}') endp++;
        }
        /* also eat the separator that followed the value */
        char *after = endp;
        while (*after == ' ' || *after == '\t' || *after == '\n') after++;
        /* now remove [k, endp) plus one adjacent comma */
        char *from = k, *to = endp;
        if (*after == ',') to = after + 1;
        else {
            /* value was last: remove the comma BEFORE it instead */
            char *b = k;
            while (b > obj && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\n')) b--;
            if (b > obj && b[-1] == ',') from = b - 1;
            to = endp;
        }
        memmove(from, to, strlen(to) + 1);
    }
}

/* ---- the hash over prev + body-prefix ---- */

static uint64_t chain_hash(const char *prev_hex, const char *body, size_t blen) {
    /* prev first, then the body: a one-byte change to prev changes the hash, and
     * so does one change anywhere in the body. */
    char buf[AIOS_AUDIT_LINE_MAX + AIOS_AUDIT_HEX + 2];
    size_t pl = prev_hex ? strlen(prev_hex) : 0;
    if (pl > AIOS_AUDIT_HEX) pl = AIOS_AUDIT_HEX;
    if (blen + pl + 1 > sizeof buf) blen = sizeof buf - pl - 1;
    if (pl) memcpy(buf, prev_hex, pl);
    buf[pl] = '|';
    memcpy(buf + pl + 1, body, blen);
    return aios_fnv1a(buf, pl + 1 + blen);
}

/* Strip a JSON object's closing brace so chain fields can be appended.
 * Returns the length of the prefix (everything before the final '}'), or 0. */
static size_t brace_prefix(const char *obj, size_t len) {
    while (len > 0 && (obj[len - 1] == '\n' || obj[len - 1] == '\r' ||
                       obj[len - 1] == ' '  || obj[len - 1] == '\t'))
        len--;
    if (len < 2 || obj[0] != '{' || obj[len - 1] != '}') return 0;
    return len - 1;
}

/* Remove a trailing partial line, if there is one. Returns bytes trimmed, or -1.
 *
 * "Torn" means exactly one thing: the last byte is not a newline. So this checks
 * the last byte FIRST, and only scans backwards when it has to.
 *
 * The first version of this scanned backwards in a way that never examined the
 * final byte, so every well-formed log -- one that ends in a newline, which is
 * every log this function ever writes -- looked torn, and it truncated the file to
 * zero before every append. The log then contained exactly one record, always
 * claiming seq 1 and chaining to genesis, and verify() reported "1 records,
 * chained and intact". It was self-consistent, it was tested, and it was an
 * amnesiac log. Two appends in a row is all it takes to see it.
 */
static long long trim_torn_tail(int fd, char *err, size_t ecap) {
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0) {
        snprintf(err, ecap, "seek: %s", strerror(errno));
        return -1;
    }
    if (end == 0) return 0;

    char last = 0;
    if (pread(fd, &last, 1, end - 1) != 1) {
        snprintf(err, ecap, "read the last byte: %s", strerror(errno));
        return -1;
    }
    if (last == '\n') return 0;              /* the common case: nothing to do */

    /* Scan back for the last newline, in blocks, newest first. */
    off_t keep = 0;
    bool found = false;
    off_t pos = end - 1;                       /* exclusive upper bound */
    char buf[4096];
    for (int block = 0; block < 64 && !found && pos > 0; block++) {
        size_t want = (size_t)(pos < (off_t)sizeof buf ? pos : (off_t)sizeof buf);
        off_t from = pos - (off_t)want;
        if (pread(fd, buf, want, from) != (ssize_t)want) break;
        for (size_t i = want; i > 0; i--)
            if (buf[i - 1] == '\n') { keep = from + (off_t)i; found = true; break; }
        pos = from;
    }
    if (!found) keep = 0;                      /* no newline at all: nothing is a record */

    if (ftruncate(fd, keep) != 0) {
        snprintf(err, ecap, "truncate the torn tail: %s", strerror(errno));
        return -1;
    }
    if (lseek(fd, keep, SEEK_SET) < 0) {
        snprintf(err, ecap, "seek after truncate: %s", strerror(errno));
        return -1;
    }
    return (long long)(end - keep);
}

/* Read the last complete record's `hash` and `seq`.
 *
 * Returns 1 when a head was found, 0 when the log is empty (not an error -- the
 * first record chains to genesis), and -1 when the log is corrupt (an error, and
 * `err` says why). Those three cases must be distinct: the first version of this
 * returned a bare bool, so "the first record has no predecessor to find" was
 * indistinguishable from "empty file". The result was that every append chained
 * to genesis with seq 1 while APPENDING to the end -- a log of records all
 * claiming to be the first, which is exactly the thing this milestone exists to
 * make impossible. It looked perfect until the second record.
 */
static int read_head(int fd, char prev[AIOS_AUDIT_HEX + 1], long long *seq,
                     char *err, size_t ecap) {
    snprintf(prev, AIOS_AUDIT_HEX + 1, "%s", "");
    *seq = 0;

    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0) { snprintf(err, ecap, "seek: %s", strerror(errno)); return -1; }
    if (end == 0) return 0;

    /* Read the final window of the file. A record is capped at LINE_MAX, so one
     * window that starts at or before the last newline holds the whole last line;
     * if it does not, widen once and give up rather than loop. */
    size_t win = (size_t)(end < (off_t)(AIOS_AUDIT_LINE_MAX + 4096)
                          ? end : (off_t)(AIOS_AUDIT_LINE_MAX + 4096));
    char *buf = malloc(win + 1);
    if (!buf) { snprintf(err, ecap, "out of memory reading the audit tail"); return -1; }
    off_t from = end - (off_t)win;
    ssize_t got = pread(fd, buf, win, from);
    if (got != (ssize_t)win) {
        snprintf(err, ecap, "read the audit tail: %s", strerror(errno));
        free(buf);
        return -1;
    }
    buf[win] = 0;

    /* Work backwards from the end. The file should end with a newline (a torn tail
     * was trimmed before this is called), so skip that one and take the line
     * before it. */
    ssize_t stop = (ssize_t)win;
    if (stop > 0 && buf[stop - 1] == '\n') stop--;
    if (stop == 0) {
        /* Only a newline in the window and nothing before it. */
        free(buf);
        return from == 0 ? 0 : -1;
    }
    ssize_t startp = -1;
    for (ssize_t i = stop - 1; i >= 0; i--)
        if (buf[i] == '\n') { startp = i + 1; break; }
    if (startp < 0) {
        /* No newline in the window: either this is the one and only line (fine
         * when the window covers the whole file), or the record is longer than a
         * record is allowed to be, which is corruption. */
        if (from != 0) {
            snprintf(err, ecap, "the last record is longer than %d bytes",
                     AIOS_AUDIT_LINE_MAX);
            free(buf);
            return -1;
        }
        startp = 0;
    }

    size_t len = (size_t)(stop - startp);
    char *line = malloc(len + 1);
    if (!line) { snprintf(err, ecap, "out of memory"); free(buf); return -1; }
    memcpy(line, buf + startp, len);
    line[len] = 0;
    free(buf);

    json_val *o = json_parse(line, len);
    free(line);
    if (!o || json_type_of(o) != JSON_OBJ) {
        if (o) json_free(o);
        snprintf(err, ecap, "the last record is not a JSON object");
        return -1;
    }
    const char *h = json_str(json_obj_get(o, "hash"));
    if (!h || strlen(h) != AIOS_AUDIT_HEX) {
        json_free(o);
        snprintf(err, ecap, "the last record has no usable hash");
        return -1;
    }
    snprintf(prev, AIOS_AUDIT_HEX + 1, "%s", h);
    json_val *sv = json_obj_get(o, "seq");
    if (sv) *seq = (long long)json_num(sv);
    json_free(o);
    return 1;
}

/* M8: which record, if any, names this staged change?
 *
 * Read forward, line by line, and look at the "stage" member of each record. A
 * line that does not parse is skipped rather than fatal: audit_verify is the
 * function that judges a broken chain, and a promote must not report "the log is
 * corrupt" for a file it merely could not parse one line of -- it reports that it
 * found no record, and the verify gate says separately whether the chain holds.
 */
long long audit_stage_record(const char *store, const char *stage_id) {
    if (!stage_id || !*stage_id) return -1;
    char path[600];
    if (!audit_path(path, sizeof path, store)) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char *line = malloc(AIOS_AUDIT_LINE_MAX + 2);
    if (!line) { fclose(f); return -1; }

    long long found = -1;
    size_t used = 0;
    int overlong = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF && used == 0) break;
        if (c == '\n' || c == EOF) {
            line[used] = 0;
            if (!overlong && used > 1) {
                json_val *o = json_parse(line, used);
                if (o && json_type_of(o) == JSON_OBJ) {
                    const char *sid = json_str(json_obj_get(o, "stage"));
                    if (sid && strcmp(sid, stage_id) == 0) {
                        json_val *sv = json_obj_get(o, "seq");
                        found = sv ? (long long)json_num(sv) : 0;
                    }
                }
                json_free(o);
                if (found >= 0 && c == '\n') {
                    /* Keep scanning? No: the FIRST record naming this change is
                     * the one that made it promotable, and reporting the newest
                     * would hide a change recorded twice. */
                    break;
                }
            }
            used = 0;
            overlong = 0;
            if (c == EOF) break;
            continue;
        }
        if (used < AIOS_AUDIT_LINE_MAX) line[used++] = (char)c;
        else overlong = 1;
    }
    free(line);
    fclose(f);
    return found;
}

/* ---- append ---- */

bool audit_append(const char *store, audit_kind kind, const char *body_json,
                  long long *seq_out, char *err, size_t ecap) {
    if (err && ecap) err[0] = 0;
    if (seq_out) *seq_out = 0;
    if (!store || !*store) {
        if (err && ecap) snprintf(err, ecap, "no store directory");
        return false;
    }
    if (!body_json || body_json[0] != '{') {
        if (err && ecap) snprintf(err, ecap, "the record body must be a JSON object");
        return false;
    }
    size_t blen = strlen(body_json);
    if (blen > AIOS_AUDIT_LINE_MAX) {
        if (err && ecap)
            snprintf(err, ecap, "record body is %zu bytes; the limit is %d",
                     blen, AIOS_AUDIT_LINE_MAX);
        return false;
    }
    /* Work on a mutable copy so the caller's own chain fields can be removed. */
    char *body = strdup(body_json);
    if (!body) {
        if (err && ecap) snprintf(err, ecap, "out of memory");
        return false;
    }
    strip_field(body, "hash");
    strip_field(body, "prev");
    strip_field(body, "seq");
    blen = strlen(body);
    size_t prefix = brace_prefix(body, blen);
    if (prefix == 0) {
        free(body);
        if (err && ecap) snprintf(err, ecap, "the record body is not a JSON object");
        return false;
    }

    char path[600];
    if (!audit_path(path, sizeof path, store)) {
        free(body);
        if (err && ecap) snprintf(err, ecap, "store path too long");
        return false;
    }

    int fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        if (err && ecap) snprintf(err, ecap, "open %.200s: %s", path, strerror(errno));
        free(body);
        return false;
    }
    /* Single writer. A second writer is told so; it is never allowed to interleave
     * two half-lines into one record. */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        snprintf(err, ecap, "the audit log is busy (another writer holds it)");
        close(fd);
        free(body);
        return false;
    }

    long long trimmed = trim_torn_tail(fd, err, ecap);
    if (trimmed < 0) { close(fd); free(body); return false; }

    char prev[AIOS_AUDIT_HEX + 1];
    long long last_seq = 0;
    int head_rc = read_head(fd, prev, &last_seq, err, ecap);
    if (head_rc < 0) { close(fd); free(body); return false; }
    if (head_rc == 0) { snprintf(prev, sizeof prev, "%s", ""); last_seq = 0; }
    const char *prev_out = (prev[0] == 0) ? "0000000000000000" : prev;
    long long seq = last_seq + 1;

    /* Build the hashed prefix: everything except the hash field. */
    char *line = malloc(AIOS_AUDIT_LINE_MAX + 512);
    if (!line) {
        snprintf(err, ecap, "out of memory building the audit record");
        free(body);
        close(fd);
        return false;
    }
    int head = snprintf(line, AIOS_AUDIT_LINE_MAX + 512,
                        "%.*s,\"seq\":%lld,\"prev\":\"%s\",\"akind\":\"%s\"",
                        (int)prefix, body, seq, prev_out,
                        kind == AUDIT_KIND_TOOL     ? "tool" :
                        kind == AUDIT_KIND_DECISION ? "decision" : "event");
    free(body);
    body = NULL;
    if (head < 0 || (size_t)head >= AIOS_AUDIT_LINE_MAX + 512 - 32) {
        snprintf(err, ecap, "the record is too long to write");
        free(line);
        close(fd);
        return false;
    }

    uint64_t h = chain_hash(prev_out, line, (size_t)head);
    int total = snprintf(line + head, AIOS_AUDIT_LINE_MAX + 512 - (size_t)head,
                         ",\"hash\":\"%016llx\"}\n", (unsigned long long)h);
    if (total <= 0) {
        snprintf(err, ecap, "the record is too long to write");
        free(line);
        close(fd);
        return false;
    }
    size_t len = (size_t)head + (size_t)total;

    /* One write() for one complete line. */
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, line + done, len - done);
        if (w <= 0) {
            if (errno == EINTR) continue;
            snprintf(err, ecap, "write: %s", strerror(errno));
            free(line);
            close(fd);
            return false;
        }
        done += (size_t)w;
    }
    if (fsync(fd) != 0) {
        snprintf(err, ecap, "fsync: %s", strerror(errno));
        free(line);
        close(fd);
        return false;
    }
    /* The directory entry too, or a crash can leave the file's length unrecorded
     * and the record silently gone -- which looks exactly like tampering. */
    {
        char dir[600];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            int dfd = open(dir, O_RDONLY | O_DIRECTORY);
            if (dfd >= 0) { (void)!fsync(dfd); close(dfd); }
        }
    }

    free(line);
    flock(fd, LOCK_UN);
    close(fd);
    if (seq_out) *seq_out = seq;
    (void)trimmed;
    return true;
}

bool audit_append_tool(const char *store, const char *tool, const char *args_json,
                       const char *origin, bool ok, double ms,
                       const char *result_json, char *err, size_t ecap) {
    if (err && ecap) err[0] = 0;
    /* The args are embedded as a JSON value when they parse, else as a string --
     * the same rule M4 used, kept so nothing that reads the old fields breaks. */
    json_val *a = json_parse(args_json ? args_json : "null",
                             args_json ? strlen(args_json) : 4);
    char *args_embedded;
    if (a) {
        args_embedded = strdup(args_json ? args_json : "null");
        json_free(a);
    } else {
        json_val *s = json_new_str(args_json ? args_json : "");
        args_embedded = json_dump(s);
        json_free(s);
    }

    json_val *o = json_new_obj();
    if (!o) {
        free(args_embedded);
        if (err && ecap) snprintf(err, ecap, "out of memory");
        return false;
    }
    json_obj_set(o, "ts", json_new_num(aios_now_ms()));
    json_obj_set(o, "tool", json_new_str(tool ? tool : ""));
    json_obj_set(o, "origin", json_new_str(origin && *origin ? origin : "human"));
    json_obj_set(o, "ok", json_new_bool(ok ? 1 : 0));
    json_obj_set(o, "ms", json_new_num(ms));
    /* embed args by splicing the raw JSON: the document is built as text so the
     * hash covers the same bytes that land in the file */
    char *base = json_dump(o);
    json_free(o);
    if (!base) {
        free(args_embedded);
        if (err && ecap) snprintf(err, ecap, "out of memory");
        return false;
    }
    size_t prefix = brace_prefix(base, strlen(base));
    if (prefix == 0) {
        free(base);
        free(args_embedded);
        if (err && ecap) snprintf(err, ecap, "internal: bad base object");
        return false;
    }

    uint64_t dig = aios_fnv1a(result_json ? result_json : "",
                              result_json ? strlen(result_json) : 0);
    char *body = malloc(strlen(base) + (args_embedded ? strlen(args_embedded) : 4) + 256);
    if (!body) {
        free(base);
        free(args_embedded);
        if (err && ecap) snprintf(err, ecap, "out of memory");
        return false;
    }
    snprintf(body, strlen(base) + (args_embedded ? strlen(args_embedded) : 4) + 256,
             "%.*s,\"args\":%s,\"digest\":\"%016llx\"}",
             (int)prefix, base, args_embedded ? args_embedded : "null",
             (unsigned long long)dig);
    free(base);
    free(args_embedded);

    bool okd = audit_append(store, AUDIT_KIND_TOOL, body, NULL, err, ecap);
    free(body);
    return okd;
}

/* ---- verify ---- */

/* Pull one string field out of a parsed record without copying the document: the
 * caller compares it before freeing. */
static const char *rec_str(json_val *o, const char *k) {
    json_val *v = json_obj_get(o, k);
    return v ? json_str(v) : NULL;
}

audit_check audit_verify(const char *store, long long max_records) {
    audit_check c;
    memset(&c, 0, sizeof c);
    c.v = AUDIT_OK;
    c.seq = -1;

    if (!store || !*store) {
        c.v = AUDIT_UNREADABLE;
        snprintf(c.why, sizeof c.why, "no store directory");
        return c;
    }
    char path[600];
    if (!audit_path(path, sizeof path, store)) {
        c.v = AUDIT_UNREADABLE;
        snprintf(c.why, sizeof c.why, "store path too long");
        return c;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        /* "I cannot see the evidence" must not read as "the evidence is good". */
        c.v = AUDIT_TORN_TAIL;
        snprintf(c.why, sizeof c.why,
                 "no audit log at %.120s (absent, not verified)", path);
        return c;
    }

    if (max_records <= 0) max_records = AIOS_AUDIT_RECORDS_MAX;

    char *line = malloc(AIOS_AUDIT_LINE_MAX + 1);
    if (!line) {
        fclose(f);
        c.v = AUDIT_UNREADABLE;
        snprintf(c.why, sizeof c.why, "out of memory");
        return c;
    }

    char expect_prev[AIOS_AUDIT_HEX + 1];
    snprintf(expect_prev, sizeof expect_prev, "%s", "0000000000000000");
    long long expect_seq = 1;
    long long n = 0;
    bool torn = false;

    for (;;) {
        size_t len = 0;
        int ch;
        bool eof = false;
        while ((ch = fgetc(f)) != EOF) {
            if (ch == '\n') break;
            if (len >= AIOS_AUDIT_LINE_MAX) {
                /* A complete line that is too long is corruption, not a tail. */
                c.v = AUDIT_BAD_JSON;
                c.seq = expect_seq;
                snprintf(c.why, sizeof c.why,
                         "record %lld is longer than %d bytes",
                         expect_seq, AIOS_AUDIT_LINE_MAX);
                free(line);
                fclose(f);
                return c;
            }
            line[len++] = (char)ch;
        }
        if (ch == EOF) {
            eof = true;
            if (len > 0) {                    /* bytes but no newline */
                torn = true;
                c.torn_bytes = (long long)len;
            }
        }
        line[len] = 0;
        if (len == 0 && eof) break;
        if (torn) break;

        if (n >= max_records) {
            c.v = AUDIT_TOO_MANY;
            c.seq = expect_seq;
            snprintf(c.why, sizeof c.why,
                     "stopped after %lld records (the check's bound); "
                     "the chain is unverified past this point", n);
            free(line);
            fclose(f);
            return c;
        }

        /* --- one complete record --- */
        json_val *o = json_parse(line, len);
        if (!o || json_type_of(o) != JSON_OBJ) {
            if (o) json_free(o);
            c.v = AUDIT_BAD_JSON;
            c.seq = expect_seq;
            snprintf(c.why, sizeof c.why, "record %lld is not a JSON object",
                     expect_seq);
            free(line);
            fclose(f);
            return c;
        }

        json_val *sv = json_obj_get(o, "seq");
        long long seq = sv ? (long long)json_num(sv) : -1;
        const char *prev = rec_str(o, "prev");
        const char *hash = rec_str(o, "hash");

        if (seq != expect_seq) {
            c.v = AUDIT_BAD_SEQ;
            c.seq = expect_seq;
            snprintf(c.why, sizeof c.why,
                     "expected seq %lld, found %lld%s", expect_seq, seq,
                     seq > expect_seq ? " -- a record was removed" : "");
            json_free(o);
            free(line);
            fclose(f);
            return c;
        }
        if (!prev || !hash || strlen(hash) != AIOS_AUDIT_HEX) {
            c.v = AUDIT_BAD_HASH;
            c.seq = seq;
            snprintf(c.why, sizeof c.why,
                     "record %lld has no usable prev/hash", seq);
            json_free(o);
            free(line);
            fclose(f);
            return c;
        }
        if (strcmp(prev, expect_prev) != 0) {
            c.v = AUDIT_BAD_LINK;
            c.seq = seq;
            snprintf(c.why, sizeof c.why,
                     "record %lld links to %.16s, but the record before it hashes "
                     "to %.16s", seq, prev, expect_prev);
            json_free(o);
            free(line);
            fclose(f);
            return c;
        }

        /* Recompute over the exact bytes, minus the trailing hash field. The
         * written form is ... ,"hash":"<hex>"}  -- find that field. */
        const char *field = strstr(line, ",\"hash\":\"");
        uint64_t want = 0;
        if (field && field > line) {
            size_t hlen = 0;
            while (hlen < AIOS_AUDIT_HEX && hash[hlen]) hlen++;
            (void)hlen;
            want = chain_hash(prev, line, (size_t)(field - line));
        }
        char want_hex[AIOS_AUDIT_HEX + 1];
        snprintf(want_hex, sizeof want_hex, "%016llx", (unsigned long long)want);
        if (strcmp(want_hex, hash) != 0) {
            c.v = AUDIT_BAD_HASH;
            c.seq = seq;
            snprintf(c.why, sizeof c.why,
                     "record %lld hashes to %s but says %s", seq, want_hex, hash);
            json_free(o);
            free(line);
            fclose(f);
            return c;
        }

        snprintf(expect_prev, sizeof expect_prev, "%s", hash);
        expect_seq++;
        n++;
        json_free(o);
        if (eof) break;
    }

    c.records = n;
    free(line);
    fclose(f);

    if (torn) {
        c.v = AUDIT_TORN_TAIL;
        c.seq = expect_seq;
        snprintf(c.why, sizeof c.why,
                 "the last record (%lld, %lld bytes) was never finished -- a crash, "
                 "not an edit", expect_seq, c.torn_bytes);
        return c;
    }
    c.seq = n;
    snprintf(c.why, sizeof c.why, "%lld records, chained and intact", n);
    return c;
}

size_t audit_count(const char *store) {
    char path[600];
    if (!audit_path(path, sizeof path, store)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = 0;
    int ch, last = 0;
    while ((ch = fgetc(f)) != EOF) { if (ch == '\n') n++; last = ch; }
    fclose(f);
    (void)last;
    return n;
}

bool audit_head(const char *store, char out[AIOS_AUDIT_HEX + 1]) {
    if (!out) return false;
    snprintf(out, AIOS_AUDIT_HEX + 1, "%s", "0000000000000000");
    char path[600];
    if (!audit_path(path, sizeof path, store)) return false;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char prev[AIOS_AUDIT_HEX + 1];
    long long seq = 0;
    char err[128] = {0};
    int had = read_head(fd, prev, &seq, err, sizeof err);
    close(fd);
    if (had <= 0 || prev[0] == 0) return false;
    snprintf(out, AIOS_AUDIT_HEX + 1, "%s", prev);
    return true;
}
