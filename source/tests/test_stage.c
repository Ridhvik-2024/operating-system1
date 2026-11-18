/* tests/test_stage.c — M3 acceptance.
 *
 * Beyond the card's list, these tests exist because each one encodes a way the
 * staging layer could silently lie:
 *
 *   - a write that appears to succeed but did not land in staging;
 *   - a promote that half-completes and is then treated as finished;
 *   - a discard that quietly changed the jail anyway;
 *   - a delete that is really an unlink (no way back);
 *   - gc eating the one directory that records a half-finished promote.
 */
#include "aios/stage.h"
#include "aios/types.h"
#include "aios/json.h"
#include "aios/snapshot.h"   /* snap_tree_digest, to prove the jail is untouched */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks, g_failed;

#define CHECK(cond, msg) do {                                          \
    g_checks++;                                                        \
    if (cond) { printf("  ok   %s\n", msg); }                          \
    else { g_failed++; printf("  FAIL %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

#define CHECK_STR(hay, needle, msg) do {                               \
    g_checks++;                                                        \
    const char *h_ = (hay), *n_ = (needle);                            \
    if (h_ && strstr(h_, n_)) { printf("  ok   %s\n", msg); }          \
    else { g_failed++; printf("  FAIL %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

/* ---- fixtures ---- */

static char g_root[512];

static void mkdirs(const char *p) { aios_mkdirs(p); }

static void write_text(const char *path, const char *text) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = 0; mkdirs(dir); }
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  (fixture: cannot write %s)\n", path); return; }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static char *slurp(const char *path) {
    char *buf = NULL;
    size_t len = 0;
    if (aios_read_file(path, &buf, &len, 1u << 20) != AIOS_OK) return NULL;
    return buf;
}

/* Fresh jail+store per test, so one test cannot see another's staging dir. */
static void fixture(const char *name, char *jail, size_t jc, char *store, size_t sc) {
    int n = snprintf(jail, jc, "%s/%s/jail", g_root, name);
    if (n < 0 || (size_t)n >= jc) { printf("  (fixture: path too long)\n"); return; }
    n = snprintf(store, sc, "%s/%s/store", g_root, name);
    if (n < 0 || (size_t)n >= sc) { printf("  (fixture: path too long)\n"); return; }
    mkdirs(jail);
    mkdirs(store);
    char staging[600];
    snprintf(staging, sizeof staging, "%s/staging", store);
    mkdirs(staging);
}

/* Stage a write the way the shell does: redirect into upper/, then note it. */
static bool stage_write(stage_session *s, const char *rel, const char *content,
                        char *err, size_t ecap) {
    char up[600];
    if (!stage_path_for(s, rel, up, sizeof up)) return false;
    write_text(up, content);
    return stage_note(s, rel, STAGE_OP_ADD, err, ecap);
}

/* ---- tests ---- */

static void test_write_does_not_land(void) {
    printf("write -> staging only\n");
    char jail[512], store[512];
    fixture("w1", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};
    stage_session s;
    CHECK(stage_begin(jail, store, 0xabc, 1, &s, err, sizeof err), "stage_begin succeeds");

    CHECK(stage_write(&s, "notes.txt", "hello\nworld\n", err, sizeof err),
          "stage a write");

    char jp[600], up[600];
    snprintf(jp, sizeof jp, "%s/notes.txt", jail);
    CHECK(!exists(jp), "the file does NOT exist in the jail");
    CHECK(stage_path_for(&s, "notes.txt", up, sizeof up), "resolve staged path");
    CHECK(exists(up), "the file DOES exist in staging");
    char *c = slurp(up);
    CHECK(c && strcmp(c, "hello\nworld\n") == 0, "staged content is correct");
    free(c);

    CHECK(stage_manifest_state(s.dir) == STAGE_MANIFEST_OPEN, "manifest is open");
    CHECK(stage_manifest_count(s.dir) == 1, "manifest has exactly one note");
    stage_end(&s);
}

static void test_diff(void) {
    printf("diff reports paths and deltas\n");
    char jail[512], store[512];
    fixture("d1", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};
    stage_session s;
    stage_begin(jail, store, 0xd1, 1, &s, err, sizeof err);

    /* an existing file we modify */
    char jp[600];
    snprintf(jp, sizeof jp, "%s/existing.txt", jail);
    write_text(jp, "one\ntwo\nthree\n");
    CHECK(stage_write(&s, "existing.txt", "one\ntwo\nthree\nfour\n", err, sizeof err),
          "stage a modification");
    /* a new file */
    CHECK(stage_write(&s, "fresh.txt", "brand new\n", err, sizeof err), "stage an add");
    /* a delete */
    char dp[600];
    snprintf(dp, sizeof dp, "%s/gone.txt", jail);
    write_text(dp, "bye\n");
    CHECK(stage_delete(&s, "gone.txt", err, sizeof err), "stage a delete");

    stage_diff d;
    CHECK(stage_diff_compute(&s, &d, err, sizeof err), "diff computes");
    CHECK(d.n == 3, "diff has three rows");

    int adds = 0, mods = 0, dels = 0;
    long modify_delta = 0;
    for (size_t i = 0; i < d.n; i++) {
        if (strcmp(d.entries[i].path, "fresh.txt") == 0) {
            CHECK(d.entries[i].op == STAGE_OP_ADD, "fresh.txt is an add");
            CHECK(d.entries[i].bytes == 10, "add reports +10 bytes");
            adds++;
        } else if (strcmp(d.entries[i].path, "existing.txt") == 0) {
            CHECK(d.entries[i].op == STAGE_OP_MODIFY, "existing.txt is a modify");
            modify_delta = d.entries[i].bytes;
            CHECK(d.entries[i].plus == 1, "modify reports one added line");
            CHECK(d.entries[i].minus == 0, "and no removed lines");
            mods++;
        } else if (strcmp(d.entries[i].path, "gone.txt") == 0) {
            CHECK(d.entries[i].op == STAGE_OP_DELETE, "gone.txt is a delete");
            CHECK(d.entries[i].bytes == -4, "delete reports -4 bytes");
            dels++;
        }
    }
    CHECK(adds == 1 && mods == 1 && dels == 1, "one of each operation");
    CHECK(modify_delta == 5, "the modify delta is +5 bytes");

    char *txt = stage_diff_text(&d);
    CHECK(txt != NULL, "diff renders");
    CHECK_STR(txt, "fresh.txt", "rendered diff names the added file");
    CHECK_STR(txt, "3 files", "rendered diff has a summary");
    CHECK_STR(txt, "1 added, 1 modified, 1 deleted", "summary counts are right");
    free(txt);
    stage_diff_free(&d);
    stage_end(&s);
}

static void test_promote(void) {
    printf("promote -> jail, staging empty\n");
    char jail[512], store[512];
    fixture("p1", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};
    stage_session s;
    stage_begin(jail, store, 0xb1, 1, &s, err, sizeof err);
    stage_write(&s, "a.txt", "aaa\n", err, sizeof err);
    stage_write(&s, "sub/b.txt", "bbb\n", err, sizeof err);

    char jp[600];
    snprintf(jp, sizeof jp, "%s/a.txt", jail);
    CHECK(!exists(jp), "before promote: not in the jail");

    size_t n = 0;
    CHECK(stage_promote(&s, ORIGIN_HUMAN, &n, err, sizeof err), "promote succeeds");
    CHECK(n == 2, "two files promoted");
    CHECK(exists(jp), "a.txt is now in the jail");
    char *c = slurp(jp);
    CHECK(c && strcmp(c, "aaa\n") == 0, "with the right content");
    free(c);

    char sub[600];
    snprintf(sub, sizeof sub, "%s/sub/b.txt", jail);
    CHECK(exists(sub), "a nested file is promoted, with its directory created");

    char up[600];
    stage_path_for(&s, "a.txt", up, sizeof up);
    CHECK(!exists(up), "a.txt is gone from staging");
    CHECK(stage_manifest_state(s.dir) == STAGE_MANIFEST_DONE, "manifest says done");

    /* promote again: must be a no-op, not an error */
    CHECK(stage_promote(&s, ORIGIN_HUMAN, &n, err, sizeof err), "a second promote is not an error");
    CHECK(n == 0, "and promotes nothing new");
    c = slurp(jp);
    CHECK(c && strcmp(c, "aaa\n") == 0, "content is unchanged by the second promote");
    free(c);
    stage_end(&s);
}

static void test_discard(void) {
    printf("discard -> jail untouched, file in trash\n");
    char jail[512], store[512];
    fixture("x1", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};
    stage_session s;
    stage_begin(jail, store, 0xd15c, 1, &s, err, sizeof err);
    stage_write(&s, "draft.txt", "never mind\n", err, sizeof err);

    char jp[600];
    snprintf(jp, sizeof jp, "%s/draft.txt", jail);
    char dir_before[600];
    snprintf(dir_before, sizeof dir_before, "%s", s.dir);
    char up[600];
    stage_path_for(&s, "draft.txt", up, sizeof up);
    CHECK(exists(up), "staged copy exists before discard");

    CHECK(stage_discard(&s, err, sizeof err), "discard succeeds");
    CHECK(!exists(jp), "the jail is untouched (no file appeared)");
    CHECK(!exists(dir_before), "the staging dir is gone");
    CHECK(strstr(s.dir, ".trash") != NULL, "the session now points into the trash");
    CHECK(exists(s.dir), "the trash directory exists");

    char trashed[700];
    snprintf(trashed, sizeof trashed, "%s/upper/draft.txt", s.dir);
    CHECK(exists(trashed), "the staged file is preserved in the trash");
    char *c = slurp(trashed);
    CHECK(c && strcmp(c, "never mind\n") == 0, "with its content intact");
    free(c);
    stage_end(&s);
}

static void test_interrupted_promote(void) {
    printf("interrupted promote is detected and re-runnable\n");
    char jail[512], store[512];
    fixture("i1", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};
    stage_session s;
    stage_begin(jail, store, 0x1211, 1, &s, err, sizeof err);
    stage_write(&s, "f1.txt", "one\n", err, sizeof err);
    stage_write(&s, "f2.txt", "two\n", err, sizeof err);

    /* Simulate a promote that died after the first rename: do exactly one, and
     * leave the manifest in PROMOTING. */
    CHECK(stage_manifest_sync(&s, STAGE_MANIFEST_PROMOTING, err, sizeof err),
          "mark promoting");
    CHECK(stage_promote_one(&s, "f1.txt", err, sizeof err), "promote only the first file");

    char jp1[600], jp2[600];
    snprintf(jp1, sizeof jp1, "%s/f1.txt", jail);
    snprintf(jp2, sizeof jp2, "%s/f2.txt", jail);
    CHECK(exists(jp1), "f1 landed");
    CHECK(!exists(jp2), "f2 did not");
    CHECK(stage_manifest_state(s.dir) == STAGE_MANIFEST_PROMOTING,
          "the manifest still says promoting: the interruption is visible");

    /* A re-run must finish the job. */
    size_t n = 0;
    CHECK(stage_promote(&s, ORIGIN_HUMAN, &n, err, sizeof err), "promote re-runs after interruption");
    CHECK(n == 1, "it promotes exactly the file that was missing");
    CHECK(exists(jp1) && exists(jp2), "both files are in the jail");
    char *c = slurp(jp1);
    CHECK(c && strcmp(c, "one\n") == 0, "the already-promoted file was not corrupted");
    free(c);
    CHECK(stage_manifest_state(s.dir) == STAGE_MANIFEST_DONE, "manifest now says done");
    stage_end(&s);
}

static void test_promote_refuses_no_manifest(void) {
    printf("a directory with no manifest can never be promoted\n");
    char jail[512], store[512];
    fixture("nm", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};

    char orphan[600];
    snprintf(orphan, sizeof orphan, "%s/staging/123-1", store);
    mkdirs(orphan);
    char orph_up[700];
    snprintf(orph_up, sizeof orph_up, "%s/upper", orphan);
    mkdirs(orph_up);
    strncat(orph_up, "/sneaky.txt", sizeof orph_up - strlen(orph_up) - 1);
    write_text(orph_up, "should never land\n");

    stage_session s;
    /* stage_open must fail: there is no manifest to describe the intent. */
    CHECK(!stage_open(orphan, &s, err, sizeof err),
          "stage_open refuses a directory with no manifest");
    CHECK_STR(err, "manifest", "and says why");
}

static void test_delete_and_undelete(void) {
    printf("delete moves to trash; undelete restores\n");
    char jail[512], store[512];
    fixture("del", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};
    stage_session s;
    stage_begin(jail, store, 0xde1, 1, &s, err, sizeof err);

    char jp[600];
    snprintf(jp, sizeof jp, "%s/precious.txt", jail);
    write_text(jp, "do not lose me\n");
    CHECK(exists(jp), "fixture file exists");

    CHECK(stage_delete(&s, "precious.txt", err, sizeof err), "delete stages a copy, then unlinks");
    CHECK(!exists(jp), "the file is gone from the jail");
    char tpath[600];
    snprintf(tpath, sizeof tpath, "%s/trash/precious.txt", s.dir);
    CHECK(exists(tpath), "but it is in the trash");
    CHECK(stage_manifest_count(s.dir) == 1, "a tombstone note was recorded");

    CHECK(stage_undelete(&s, "precious.txt", err, sizeof err), "undelete succeeds");
    CHECK(exists(jp), "the file is back in the jail");
    char *c = slurp(jp);
    CHECK(c && strcmp(c, "do not lose me\n") == 0, "with its exact content");
    free(c);
    CHECK(!exists(tpath), "and the trash copy is consumed");

    /* deleting something that is not there must fail, not silently pass */
    CHECK(!stage_delete(&s, "absent.txt", err, sizeof err),
          "deleting an absent file fails");
    CHECK_STR(err, "does not exist", "with a clear reason");

    /* promote must never unlink: a delete note is skipped */
    size_t n = 0;
    CHECK(stage_promote(&s, ORIGIN_HUMAN, &n, err, sizeof err), "promote runs with a delete note present");
    CHECK(exists(jp), "promote did not remove the file the note described");
    stage_end(&s);
}

static void test_path_escapes(void) {
    printf("staged paths cannot escape\n");
    char jail[512], store[512];
    fixture("esc", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};
    stage_session s;
    stage_begin(jail, store, 0x35c, 1, &s, err, sizeof err);

    char out[600];
    CHECK(!stage_path_for(&s, "../outside.txt", out, sizeof out), ".. is refused");
    CHECK(!stage_path_for(&s, "/etc/passwd", out, sizeof out), "an absolute path is refused");
    CHECK(!stage_path_for(&s, "a//b.txt", out, sizeof out), "an empty component is refused");
    CHECK(!stage_path_for(&s, "", out, sizeof out), "an empty path is refused");
    CHECK(!stage_path_for(&s, "./x.txt", out, sizeof out), ". is refused");
    CHECK(stage_path_for(&s, "a/b/c.txt", out, sizeof out), "a normal nested path is fine");

    CHECK(!stage_note(&s, "../x", STAGE_OP_ADD, err, sizeof err),
          "a note with an escaping path is refused");
    CHECK(stage_manifest_count(s.dir) == 0, "and nothing was recorded");
    stage_end(&s);
}

static void test_gc(void) {
    printf("gc removes orphans, keeps live and promoting dirs\n");
    char jail[512], store[512];
    fixture("gc", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};

    /* 1. an orphan: a directory with no manifest at all */
    char orphan[600];
    snprintf(orphan, sizeof orphan, "%s/staging/1-1", store);
    mkdirs(orphan);

    /* 2. a live session, recently touched */
    stage_session live;
    stage_begin(jail, store, 0x11ce, 5, &live, err, sizeof err);
    stage_write(&live, "live.txt", "keep me\n", err, sizeof err);

    /* 3. a session whose promote was interrupted: must never be gc'd */
    stage_session prom;
    stage_begin(jail, store, 0x9909, 7, &prom, err, sizeof err);
    stage_write(&prom, "half.txt", "half\n", err, sizeof err);
    stage_manifest_sync(&prom, STAGE_MANIFEST_PROMOTING, err, sizeof err);

    stage_gc_result r;
    /* max_age 1 hour: the live session is younger, so it stays */
    CHECK(stage_gc(store, (long long)aios_now_ms(), 3600LL * 1000, &r, err, sizeof err),
          "gc runs");
    CHECK(r.removed == 1, "it removed exactly the orphan");
    CHECK(!exists(orphan), "the orphan is gone");
    CHECK(exists(live.dir), "the live session is kept");
    CHECK(exists(prom.dir), "the promoting session is kept: it is the only record "
                            "of a half-finished promote");

    /* Now age everything out: the live one goes, promoting still does not. */
    CHECK(stage_gc(store, (long long)aios_now_ms() + 1000, 0, &r, err, sizeof err),
          "gc with max_age 0 removes any open session");
    CHECK(r.kept == 1, "exactly one directory was kept");
    CHECK(!exists(live.dir), "the aged-out open session is gone");
    CHECK(exists(prom.dir), "the promoting session survives an aggressive gc");
    stage_end(&live);
    stage_end(&prom);
}

static void test_gc_is_idempotent(void) {
    printf("gc on an empty or absent store is not an error\n");
    char store[600];
    snprintf(store, sizeof store, "%s/nothing/here", g_root);
    stage_gc_result r;
    char err[256] = {0};
    CHECK(stage_gc(store, 0, 1000, &r, err, sizeof err),
          "gc on a store with no staging dir succeeds");
    CHECK(r.removed == 0 && r.kept == 0, "and reports nothing");
}

static void test_two_sessions_do_not_collide(void) {
    printf("two runs of the same command get separate dirs\n");
    char jail[512], store[512];
    fixture("seq", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};

    stage_session a, b;
    CHECK(stage_begin(jail, store, 0x7777, 0, &a, err, sizeof err), "first session");
    CHECK(stage_begin(jail, store, 0x7777, 0, &b, err, sizeof err), "second session");
    CHECK(strcmp(a.dir, b.dir) != 0, "they have different directories");

    stage_write(&a, "x.txt", "from a\n", err, sizeof err);
    stage_write(&b, "x.txt", "from b\n", err, sizeof err);
    char upa[600], upb[600];
    stage_path_for(&a, "x.txt", upa, sizeof upa);
    stage_path_for(&b, "x.txt", upb, sizeof upb);
    char *ca = slurp(upa), *cb = slurp(upb);
    CHECK(ca && strcmp(ca, "from a\n") == 0, "session a kept its own content");
    CHECK(cb && strcmp(cb, "from b\n") == 0, "session b kept its own content");
    free(ca);
    free(cb);

    /* discarding one must not affect the other */
    CHECK(stage_discard(&b, err, sizeof err), "discard session b");
    CHECK(exists(upa), "session a is untouched by b's discard");
    stage_end(&a);
    stage_end(&b);
}

static void test_manifest_survives_roundtrip(void) {
    printf("the manifest is the truth, across processes\n");
    char jail[512], store[512];
    fixture("mf", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};
    stage_session s;
    stage_begin(jail, store, 0x4d46, 3, &s, err, sizeof err);
    stage_write(&s, "one.txt", "1\n", err, sizeof err);
    stage_write(&s, "two.txt", "2\n", err, sizeof err);
    char dir[600];
    snprintf(dir, sizeof dir, "%s", s.dir);
    stage_end(&s);

    /* A different "process" opens it by path only. */
    stage_session re;
    CHECK(stage_open(dir, &re, err, sizeof err), "reopen by path");
    CHECK(re.intent_hash == 0x4d46, "intent hash survives");
    CHECK(re.seq == 3, "seq survives");
    CHECK(strstr(re.upper, "upper") != NULL, "upper dir resolves");
    CHECK(stage_manifest_count(re.dir) == 2, "both notes survive");
    CHECK(stage_manifest_state(re.dir) == STAGE_MANIFEST_OPEN, "state survives");

    size_t n = 0;
    CHECK(stage_promote(&re, ORIGIN_HUMAN, &n, err, sizeof err), "and it can promote");
    CHECK(n == 2, "both files promoted from the reopened session");
    stage_end(&re);
}

static void test_duplicate_note_is_idempotent(void) {
    printf("noting the same file twice does not duplicate the row\n");
    char jail[512], store[512];
    fixture("dup", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};
    stage_session s;
    stage_begin(jail, store, 0x0d0d, 1, &s, err, sizeof err);
    stage_write(&s, "same.txt", "v1\n", err, sizeof err);
    CHECK(stage_note(&s, "same.txt", STAGE_OP_ADD, err, sizeof err),
          "noting it again succeeds");
    CHECK(stage_manifest_count(s.dir) == 1, "but the manifest still has one row");

    /* overwrite the staged content: still one row, and the diff shows v2 */
    stage_write(&s, "same.txt", "v2\n", err, sizeof err);
    CHECK(stage_manifest_count(s.dir) == 1, "a second write is the same row");
    stage_diff d;
    stage_diff_compute(&s, &d, err, sizeof err);
    CHECK(d.n == 1, "one diff row");
    CHECK(d.entries[0].bytes == 3, "the diff reflects the latest content");
    stage_diff_free(&d);
    stage_end(&s);
}

static void test_too_many_files(void) {
    printf("staging refuses to grow without bound\n");
    char jail[512], store[512];
    fixture("many", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};
    stage_session s;
    stage_begin(jail, store, 0x9999, 1, &s, err, sizeof err);
    bool all_ok = true;
    for (int i = 0; i < AIOS_STAGE_MAX_FILES; i++) {
        char rel[64];
        snprintf(rel, sizeof rel, "f%03d.txt", i);
        if (!stage_write(&s, rel, "x\n", err, sizeof err)) all_ok = false;
    }
    CHECK(all_ok, "the stated maximum number of files stages successfully");
    CHECK(stage_manifest_count(s.dir) == AIOS_STAGE_MAX_FILES, "at the limit");

    CHECK(!stage_write(&s, "one-too-many.txt", "x\n", err, sizeof err),
          "one more is refused");
    CHECK_STR(err, "too many", "with a clear reason");
    stage_end(&s);
}


/* The human-triggered promote is the one path a person can drive, so it gets
 * its own adversarial tests. Each of these is a way someone who can write into
 * the store could otherwise get a file into the jail without the policy engine
 * ever seeing it. */
static void test_locked_promote_refuses_orphan(void) {
    printf("locked promote refuses a directory with no manifest\n");
    char jail[512], store[512];
    fixture("lp", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};

    char orphan[600];
    snprintf(orphan, sizeof orphan, "%s/staging/55-1", store);
    char orph_up[700];
    snprintf(orph_up, sizeof orph_up, "%s/upper", orphan);
    mkdirs(orph_up);

    stage_session s;
    memset(&s, 0, sizeof s);
    memcpy(s.dir, orphan, strlen(orphan) + 1);
    memcpy(s.upper, orph_up, strlen(orph_up) + 1);
    memcpy(s.jail, jail, strlen(jail) + 1);
    memcpy(s.store, store, strlen(store) + 1);
    int mn = snprintf(s.manifest, sizeof s.manifest, "%s/manifest.json", orphan);
    CHECK(mn > 0 && (size_t)mn < sizeof s.manifest, "fixture paths fit");

    size_t n = 0;
    CHECK(!stage_promote_locked(&s, ORIGIN_HUMAN, &n, err, sizeof err),
          "promote_locked refuses without a manifest");
    CHECK_STR(err, "manifest", "and says why");
    CHECK(n == 0, "nothing was promoted");
}

/* M7.5 changed this test's conclusion, on purpose.
 *
 * It used to assert: an unlisted file in upper/ is ignored, the listed one is
 * promoted, "the manifest drives the move". The last clause was the problem.
 * Ignoring the unlisted file is indistinguishable, from the human's side, from
 * never having staged it -- and the diff they approved iterates over the
 * manifest, so it did not mention it either. The promote SUCCEEDED and left
 * bytes behind that nobody was ever shown: the same defect as a review that
 * shows one of two pending changes, one layer down.
 *
 * The rule now is the fail-closed one: if the staging directory holds a file the
 * manifest does not name, the manifest has stopped describing the change, and
 * neither promote variant will act on it. The human is told to discard and
 * re-stage, because re-staging re-derives the manifest from reality.
 */
static void test_promote_refuses_unlisted_files(void) {
    printf("promote refuses a staging dir the manifest does not describe\n");
    char jail[512], store[512];
    fixture("lu", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};
    stage_session s;
    stage_begin(jail, store, 0xabcd, 1, &s, err, sizeof err);
    stage_write(&s, "named.txt", "listed\n", err, sizeof err);

    /* Something dropped into upper/ by hand, not named in the manifest. */
    char sneaky[700];
    snprintf(sneaky, sizeof sneaky, "%s/sneaky.txt", s.upper);
    write_text(sneaky, "not listed\n");

    size_t n = 0;
    CHECK(!stage_promote_locked(&s, ORIGIN_HUMAN, &n, err, sizeof err),
          "promote_locked REFUSES rather than promoting part of the change");
    CHECK_STR(err, "not named by the manifest", "and says the manifest does not describe it");
    CHECK(n == 0, "nothing was promoted");

    char jp[600], js[600];
    snprintf(jp, sizeof jp, "%s/named.txt", jail);
    snprintf(js, sizeof js, "%s/sneaky.txt", jail);
    CHECK(!exists(jp), "the listed file did NOT land either: a partial promote is not a promote");
    CHECK(!exists(js), "and the unlisted file certainly did not");

    /* The plain variant has the same rule: it must not be the way around it. */
    err[0] = 0;
    n = 0;
    CHECK(!stage_promote(&s, ORIGIN_HUMAN, &n, err, sizeof err),
          "stage_promote refuses for the same reason");
    CHECK_STR(err, "not named by the manifest", "with the same explanation");
    CHECK(!exists(jp), "and still nothing landed");

    /* Discarding is the documented way out, and it never touches the jail. */
    CHECK(stage_discard(&s, err, sizeof err), "discard works on a change that cannot be promoted");
    CHECK(!exists(jp) && !exists(js), "and the jail is still untouched");
    stage_end(&s);
}

static void test_locked_promote_refuses_symlink_escape(void) {
    printf("locked promote refuses a staged symlink pointing outside\n");
    char jail[512], store[512];
    fixture("ls", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};
    stage_session s;
    stage_begin(jail, store, 0x5111, 1, &s, err, sizeof err);

    /* An outside file we must never be able to pull in. */
    char secret[600];
    snprintf(secret, sizeof secret, "%s/secret.txt", g_root);
    write_text(secret, "TOP SECRET\n");

    /* Stage a symlink where a file would be: without the realpath check, its
     * target's content would be promoted into the jail. */
    char link[700];
    snprintf(link, sizeof link, "%s/leak.txt", s.upper);
    if (symlink(secret, link) == 0) {
        stage_note(&s, "leak.txt", STAGE_OP_ADD, err, sizeof err);
        size_t n = 0;
        (void)stage_promote_locked(&s, ORIGIN_HUMAN, &n, err, sizeof err);
        char jp[600];
        snprintf(jp, sizeof jp, "%s/leak.txt", jail);
        if (exists(jp)) {
            char *c = slurp(jp);
            CHECK(c == NULL || strstr(c, "TOP SECRET") == NULL,
                  "the secret's CONTENT did not reach the jail");
            free(c);
        } else {
            CHECK(true, "the symlink was refused outright");
        }
    } else {
        CHECK(true, "symlink unavailable on this filesystem (skipped)");
    }
    stage_end(&s);
}

/* M7: the last door.
 *
 * Every layer above this one already refuses an AI-origin promotion -- the
 * dispatcher refuses the builtin, the shell refuses the line. This test exists
 * because "every layer above refuses" is a claim about callers, and callers are
 * the part of a program that gets rewritten. stage_promote() is the function
 * that moves a file into the jail; if a future caller forgets to ask, the answer
 * still has to be no.
 *
 * The check is not that promote returns false. It is that the jail is UNCHANGED
 * -- a refusal that renamed half the files first would pass a return-value test.
 */
static void test_promote_refuses_machine_origin(void) {
    printf("promote refuses a non-human origin, and changes nothing\n");
    char jail[512], store[512];
    fixture("mo", jail, sizeof jail, store, sizeof store);
    char err[256] = {0};

    stage_session s;
    CHECK(stage_begin(jail, store, 0xb007, 1, &s, err, sizeof err),
          "a machine's write can be staged");
    CHECK(stage_write(&s, "machine.txt", "written by a model\n", err, sizeof err),
          "the file is in the staging area, not the jail");

    unsigned long long before = 0, after = 0;
    size_t nf = 0;
    unsigned long long nb = 0;
    CHECK(snap_tree_digest(jail, &before, &nf, &nb, err, sizeof err),
          "the empty jail has a digest");

    size_t n = 1;
    err[0] = 0;
    CHECK(!stage_promote(&s, ORIGIN_AI, &n, err, sizeof err),
          "stage_promote REFUSES on its own, with no dispatcher above it");
    CHECK_STR(err, "not from a human", "and says the origin is the reason");
    CHECK(n == 0, "and reports that it promoted nothing");

    /* ORIGIN_UNSPECIFIED is not "probably a person". */
    err[0] = 0;
    CHECK(!stage_promote(&s, ORIGIN_UNSPECIFIED, &n, err, sizeof err),
          "an unspecified origin is refused too, not defaulted to human");

    CHECK(snap_tree_digest(jail, &after, &nf, &nb, err, sizeof err),
          "and it can be read again");
    CHECK(before == after, "the jail is byte-for-byte unchanged after the refusals");
    {
        char jp[600];
        snprintf(jp, sizeof jp, "%s/machine.txt", jail);
        FILE *f = fopen(jp, "r");
        CHECK(f == NULL, "the file did NOT land in the jail");
        if (f) fclose(f);
    }

    /* and the human still can, so the refusal is about WHO not about a broken
     * promote: the same session, the same staged file, one different argument. */
    err[0] = 0;
    CHECK(stage_promote(&s, ORIGIN_HUMAN, &n, err, sizeof err),
          "the same staged file promotes when a human asks");
    CHECK(n == 1, "...and it promoted it");
    {
        char jp[600];
        snprintf(jp, sizeof jp, "%s/machine.txt", jail);
        FILE *f = fopen(jp, "r");
        CHECK(f != NULL, "...into the jail this time");
        if (f) fclose(f);
    }
}

int main(void) {
    snprintf(g_root, sizeof g_root, "/tmp/aios-stage-test-%d", (int)getpid());
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_root);
    if (system(cmd) != 0) { /* best effort */ }
    mkdirs(g_root);

    printf("== test_stage ==\n");
    test_write_does_not_land();
    test_diff();
    test_promote();
    test_discard();
    test_interrupted_promote();
    test_promote_refuses_no_manifest();
    test_delete_and_undelete();
    test_path_escapes();
    test_gc();
    test_gc_is_idempotent();
    test_two_sessions_do_not_collide();
    test_manifest_survives_roundtrip();
    test_duplicate_note_is_idempotent();
    test_too_many_files();
    test_locked_promote_refuses_orphan();
    test_promote_refuses_unlisted_files();
    test_locked_promote_refuses_symlink_escape();
    test_promote_refuses_machine_origin();

    snprintf(cmd, sizeof cmd, "rm -rf %s", g_root);
    if (system(cmd) != 0) { /* best effort */ }

    printf("%s: %d checks, %d failed\n", g_failed ? "FAIL" : "PASS", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
