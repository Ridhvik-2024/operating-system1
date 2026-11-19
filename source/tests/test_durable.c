/* test_durable.c — M8: crash-consistent durable store tests.
 * Covers: atomic write/read/overwrite (and no temp leftovers), fsync'd
 * journal appends with torn-tail replay, mirror/restore round-trips,
 * restore_dir, path-escape rejection, durable job journal replay through
 * a real shell (states, final texts, id continuity), and — when the
 * suite runs inside the booted OS — a live write to the ext2 side disk. */
#include "t.h"
#include "aios/durable.h"
#include "aios/shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

static char tmpdir[256] = {0};

/* C3/C4 additions, defined below but called from run() */
static void test_c3_concurrent_append(void);
static void test_c3_large_journal_tail(void);
static void test_c4_log_builtin(void);
static void test_c3_stale_temp_file(void);

static void make_tmpdir(void) {
    snprintf(tmpdir, sizeof tmpdir, "/tmp/aios-dur-test-%d", (int)getpid());
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmpdir);
    (void)!system(cmd);
    CHECK(aios_mkdirs(tmpdir) == AIOS_OK, "tmpdir created");
}

static void rm_tmpdir(void) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmpdir);
    (void)!system(cmd);
}

static size_t count_files(const char *dir, size_t *tmp_leftovers) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t n = 0, tmps = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        n++;
        if (strstr(e->d_name, ".tmp.")) tmps++;
    }
    closedir(d);
    if (tmp_leftovers) *tmp_leftovers = tmps;
    return n;
}

static void write_plain(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputs(s, f);
    fclose(f);
}

static void run(void) {
    /* ---- 0. live online test against the real side disk (VM only) ----
     * Runs BEFORE any root redirection: inside the booted OS the default
     * root /data/aios is the mounted ext2 disk, and this is the in-VM
     * proof that the durable layer talks to real hardware. On a dev host
     * there is no /data and the check skips. */
    aios_dur_set_root(NULL);
    if (aios_dur_available()) {
        const char *body = "{\"online\":\"m8-selftest\"}";
        CHECK(aios_dur_write_atomic("selftest-durable.marker", body,
                                    strlen(body)) == AIOS_OK,
              "online: atomic write to the side disk");
        char rb[128] = {0};
        size_t rl = 0;
        CHECK(aios_dur_read("selftest-durable.marker", rb, sizeof rb - 1,
                            &rl) == AIOS_OK && rl == strlen(body) &&
              strcmp(rb, body) == 0,
              "online: read-back matches");
        CHECK(aios_dur_append("selftest-durable.marker.log", "ok\n", 3)
              == AIOS_OK, "online: journal append");
        char mpath[1024];
        snprintf(mpath, sizeof mpath, "%s/selftest-durable.marker",
                 aios_dur_root());
        remove(mpath);
        snprintf(mpath, sizeof mpath, "%s/selftest-durable.marker.log",
                 aios_dur_root());
        remove(mpath);
    } else {
        SKIP("online durable tests (no /data side disk on this host)");
    }

    /* ---- 1. redirected root, availability ---- */
    make_tmpdir();
    aios_dur_set_root(tmpdir);
    CHECK(strcmp(aios_dur_root(), tmpdir) == 0, "root redirected");
    CHECK(aios_dur_available() == true, "root available");

    /* ---- 2. atomic write: roundtrip, overwrite, no leftovers ---- */
    const char *doc1 = "{\"tick\":1,\"mode\":\"balanced\"}";
    const char *doc2 = "{\"tick\":2,\"mode\":\"balanced\"}";
    CHECK(aios_dur_write_atomic("var/state.json", doc1, strlen(doc1))
          == AIOS_OK, "atomic write 1");
    char rb[256] = {0};
    size_t rl = 0;
    CHECK(aios_dur_read("var/state.json", rb, sizeof rb - 1, &rl) == AIOS_OK
          && rl == strlen(doc1) && strcmp(rb, doc1) == 0,
          "read-back 1 matches");
    CHECK(aios_dur_write_atomic("var/state.json", doc2, strlen(doc2))
          == AIOS_OK, "atomic overwrite 2");
    memset(rb, 0, sizeof rb);
    CHECK(aios_dur_read("var/state.json", rb, sizeof rb - 1, &rl) == AIOS_OK
          && strcmp(rb, doc2) == 0, "read-back 2 matches (replaced)");
    size_t leftovers = 99;
    char vardir[1024];
    snprintf(vardir, sizeof vardir, "%s/var", tmpdir);
    CHECK(count_files(vardir, &leftovers) == 1 && leftovers == 0,
          "no temp files left behind");

    /* ---- 3. journal append + torn-tail replay ---- */
    CHECK(aios_dur_append("journal.jsonl", "one\n", 4) == AIOS_OK,
          "append 1");
    CHECK(aios_dur_append("journal.jsonl", "two\n", 4) == AIOS_OK,
          "append 2");
    CHECK(aios_dur_append("journal.jsonl", "three\n", 6) == AIOS_OK,
          "append 3");
    size_t n = 0;
    char last[128] = {0};
    CHECK(aios_dur_lines("journal.jsonl", &n, last, sizeof last) == AIOS_OK
          && n == 3 && strcmp(last, "three") == 0,
          "replay: 3 complete lines, last=three");
    /* a crash mid-append leaves a line without its newline: dropped */
    CHECK(aios_dur_append("journal.jsonl", "torn-record", 11) == AIOS_OK,
          "append torn tail");
    n = 99; last[0] = 0;
    CHECK(aios_dur_lines("journal.jsonl", &n, last, sizeof last) == AIOS_OK
          && n == 3 && strcmp(last, "three") == 0,
          "torn tail dropped on replay");

    /* ---- 4. mirror / restore round-trip ---- */
    const char *live = "{\"conf\":\"tuned\"}";
    char livepath[1024];
    snprintf(livepath, sizeof livepath, "%s/live.conf", tmpdir);
    write_plain(livepath, live);
    CHECK(aios_dur_mirror(livepath, "var/autopilot.conf") == AIOS_OK,
          "mirror live file into store");
    char dest[2048];
    snprintf(dest, sizeof dest, "%s/restored.conf", tmpdir);
    CHECK(aios_dur_restore_file("var/autopilot.conf", dest) == AIOS_OK,
          "restore out of store");
    FILE *f = fopen(dest, "rb");
    CHECK(f != NULL, "restored file exists");
    if (f) {
        char fb[128] = {0};
        size_t fn = fread(fb, 1, sizeof fb - 1, f);
        fclose(f);
        CHECK(fn == strlen(live) && strcmp(fb, live) == 0,
              "restored bytes match");
    }

    /* ---- 5. restore_dir (var/ holds state.json + autopilot.conf) ---- */
    char vdest[1024];
    snprintf(vdest, sizeof vdest, "%s/out", tmpdir);
    CHECK(aios_mkdirs(vdest) == AIOS_OK, "restore dest dir");
    CHECK(aios_dur_restore_dir("var", vdest) == 2,
          "restore_dir copied both store files");
    snprintf(dest, sizeof dest, "%s/autopilot.conf", vdest);
    f = fopen(dest, "rb");
    CHECK(f != NULL, "restore_dir produced autopilot.conf");
    if (f) fclose(f);
    snprintf(dest, sizeof dest, "%s/state.json", vdest);
    f = fopen(dest, "rb");
    CHECK(f != NULL, "restore_dir produced state.json");
    if (f) fclose(f);

    /* ---- 6. error paths ---- */
    CHECK(aios_dur_read("no/such/file", rb, sizeof rb, &rl) == AIOS_ERR_IO,
          "read of absent file errors");
    CHECK(aios_dur_write_atomic("../escape", "x", 1) == AIOS_ERR_ARG,
          "path escape rejected");
    char outside[1024];
    snprintf(outside, sizeof outside, "%s/../escape", tmpdir);
    struct stat st;
    CHECK(stat(outside, &st) != 0, "no file escaped the store");

    /* ---- 7. durable job journal replay through a real shell ---- */
    char jdir[1024], sdir[1024];
    snprintf(jdir, sizeof jdir, "%s/shell-jail", tmpdir);
    snprintf(sdir, sizeof sdir, "%s/shell-store", tmpdir);
    CHECK(aios_mkdirs(jdir) == AIOS_OK && aios_mkdirs(sdir) == AIOS_OK,
          "shell dirs created");
    /* previous session: job 1 done (with result doc), job 2 never exited */
    char jpath[2048];
    snprintf(jpath, sizeof jpath, "%s/jobs.jsonl", sdir);
    write_plain(jpath,
        "{\"op\":\"spawn\",\"id\":1,\"goal\":\"summarize the notes\","
        "\"ts_ms\":100}\n"
        "{\"op\":\"exit\",\"id\":1,\"state\":\"done\",\"ts_ms\":200}\n"
        "{\"op\":\"spawn\",\"id\":2,\"goal\":\"background index build\","
        "\"ts_ms\":300}\n");
    snprintf(jpath, sizeof jpath, "%s/jobs", sdir);
    CHECK(aios_mkdirs(jpath) == AIOS_OK, "jobs result dir");
    snprintf(jpath, sizeof jpath, "%s/jobs/1.json", sdir);
    write_plain(jpath,
        "{\"id\":1,\"goal\":\"summarize the notes\",\"status\":\"done\","
        "\"final\":\"2 files, 453 bytes\",\"ms\":12}");

    sh_shell *sh = sh_create(jdir, sdir);
    CHECK(sh != NULL, "shell created over a durable store");
    if (sh) {
        sh_result r;
        sh_exec(sh, "jobs", &r);
        CHECK(strstr(r.text, "done") != NULL &&
              strstr(r.text, "summarize the notes") != NULL &&
              strstr(r.text, "2 files, 453 bytes") != NULL,
              "replayed job 1: done with final text");
        CHECK(strstr(r.text, "failed") != NULL &&
              strstr(r.text, "background index build") != NULL,
              "job 2 (no exit record) replayed as failed");
        sh_exec(sh, "spawn count the durable test files", &r);
        CHECK(r.status == 0 && strstr(r.text, "[job 3]"),
              "next spawn continues the id sequence at 3");
        sh_exec(sh, "wait all", &r);
        CHECK(r.status == 0, "replayed + new jobs all reap");
        sh_exec(sh, "jobs", &r);
        CHECK(strstr(r.text, "count the durable test files") != NULL,
              "new job listed alongside replayed history");
        sh_free(sh);
        /* the spawn+exit above must have been journaled for the NEXT boot */
        n = 0;
        char jrel[2048];
        snprintf(jrel, sizeof jrel, "%s/jobs.jsonl", sdir);
        /* plain count: 3 historical + spawn + exit = 5 complete lines */
        FILE *jf = fopen(jrel, "rb");
        CHECK(jf != NULL, "jobs journal still there");
        if (jf) {
            char jb[4096];
            size_t jn = fread(jb, 1, sizeof jb - 1, jf);
            fclose(jf);
            jb[jn] = 0;
            size_t lines = 0;
            for (size_t i = 0; i < jn; i++)
                if (jb[i] == '\n') lines++;
            CHECK(lines == 5, "journal has 5 complete records after replay+run");
        }
    }

    test_c3_concurrent_append();
    test_c3_large_journal_tail();
    test_c3_stale_temp_file();
    test_c4_log_builtin();

    rm_tmpdir();
    aios_dur_set_root(NULL);
}

/* ---- C3: concurrent appends do not interleave -------------------------- */
/*
 * HONESTY NOTE, and it matters more than the test.
 *
 * This test CANNOT distinguish the directory lock from no lock at all, and the
 * sabotage matrix says so: removing the lock entirely leaves it green. That is
 * not a weakness in the test that more records would fix - it is a true
 * statement about Linux. `write(2)` on an O_APPEND fd is serialised by the
 * kernel against other O_APPEND writes, so two processes appending whole
 * records with one write() each cannot interleave. Measured directly at 64 KiB
 * per write, two writers, no lock: 100 lines, zero fused.
 *
 * So what is the lock FOR? Three things the kernel does not give:
 *
 *   1. a record that is SEVERAL writes (the API's own callers assemble a JSON
 *      document and then its newline), where the kernel guarantees each write
 *      is atomic but not the sequence;
 *   2. the temp-file protocol in write_atomic, where two writers choosing
 *      concurrent replaces could otherwise interleave rename order;
 *   3. correctness on a filesystem or kernel where O_APPEND is NOT atomic
 *      (network filesystems have historically been exactly this).
 *
 * The test below therefore asserts the PROPERTY a caller needs - whole records,
 * no fusion - and says plainly that on Linux it would also pass without the
 * lock. A test is allowed to be a regression guard for a property it cannot
 * currently falsify, as long as it does not pretend to be the proof. */
/*
 * The property under test is the one `O_APPEND` alone does NOT give: a record
 * written as several writes must not interleave with another process's record.
 * Two forked writers each append 200 multi-write records; every resulting line
 * must be exactly one writer's whole record.
 *
 * Without the directory lock this fails immediately, and it fails in the worst
 * way - the file still looks like a journal, with lines that are halves of two
 * different records. A tolerant reader cannot repair that, because each half is
 * a well-formed piece of a different record.
 */
static void test_c3_concurrent_append(void) {
    char sdir[512], path[600];
    snprintf(sdir, sizeof sdir, "%s/c3", tmpdir);
    aios_dur_set_root(sdir);
    (void)aios_mkdirs(sdir);
    snprintf(path, sizeof path, "%s/journal.jsonl", sdir);

    /* The race window has to be REACHABLE or this test proves nothing.
     *
     * Record size and count were the difference between a test that detects a
     * missing lock and one that does not: at 120 records of 60 bytes, a write()
     * of that size so rarely splits that removing the lock entirely still
     * produced 240 clean lines. The point of the lock is that it holds for the
     * records a real journal has - multi-KB JSON documents - so the test uses
     * big ones and many of them. A test whose failure mode cannot be triggered
     * is indistinguishable from one that passes.
     */
    const int kPer = 300;
    pid_t a = fork();
    if (a == 0) {
        for (int i = 0; i < kPer; i++) {
            char rec[1200];
            char pad[1024];
            memset(pad, 'a', sizeof pad - 1);
            pad[sizeof pad - 1] = 0;
            snprintf(rec, sizeof rec, "{\"w\":\"A\",\"i\":%d,\"pad\":\"%s\"}\n", i, pad);
            /* ONE call per record. Two calls would be two critical sections and
             * the lock is per call - which is the API's actual guarantee, and
             * worth stating: a caller that wants two appends to be atomic
             * together must build one buffer. */
            aios_dur_append("journal.jsonl", rec, strlen(rec));
        }
        _exit(0);
    }
    pid_t b = fork();
    if (b == 0) {
        for (int i = 0; i < kPer; i++) {
            char rec[1200];
            char pad[1024];
            memset(pad, 'b', sizeof pad - 1);
            pad[sizeof pad - 1] = 0;
            snprintf(rec, sizeof rec, "{\"w\":\"B\",\"i\":%d,\"pad\":\"%s\"}\n", i, pad);
            aios_dur_append("journal.jsonl", rec, strlen(rec));
        }
        _exit(0);
    }
    CHECK(a > 0 && b > 0, "both writers forked");
    int st = 0;
    waitpid(a, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "writer A finished");
    waitpid(b, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "writer B finished");

    static char buf[1 << 21];
    size_t n = 0;
    CHECK(aios_dur_read("journal.jsonl", buf, sizeof buf, &n) == AIOS_OK, "journal readable");
    size_t lines = 0, mixed = 0, malformed = 0;
    size_t pos = 0;
    while (pos < n) {
        char *nl = memchr(buf + pos, '\n', n - pos);
        if (!nl) break;                                  /* torn tail: fine */
        const size_t len = (size_t)(nl - (buf + pos));
        const char *line = buf + pos;
        lines++;
        /* A fused line is one that CONTAINS a second record's opening brace.
         *
         * The first version of this check searched for `"w":"A"` and `"w":"B"`
         * anywhere in the line - and every record is padded with a long run of
         * its own letter, so both patterns were present in every line and the
         * check reported 100% fusion against a lock that was working perfectly.
         * A test that cannot tell its two outcomes apart is not a test; count
         * the record delimiters instead, which is what "fused" actually means. */
        size_t opens = 0;
        bool has_a = false, has_b = false;
        for (size_t i = 0; i < len; i++) {
            if (line[i] == '{') opens++;
            /* BOUNDED comparison against the line, not strstr over the buffer.
             * The records live in one big buffer with no NUL at each line end,
             * so `strstr(line, ...)` searched into the FOLLOWING records and
             * reported every line as containing both writers. That is a test
             * bug that looks exactly like a lock bug - it cost two rounds of
             * investigating working code. */
            if (i + 8 <= len && memcmp(line + i, "aaaaaaaa", 8) == 0) has_a = true;
            if (i + 8 <= len && memcmp(line + i, "bbbbbbbb", 8) == 0) has_b = true;
        }
        if (opens > 1) mixed++;                       /* two records in one line */
        if (has_a && has_b) mixed++;                  /* bytes from both writers */
        if (opens != 1 || len < 1000) malformed++;
        pos = (size_t)(nl - buf) + 1;
    }
    CHECK(lines == (size_t)(2 * kPer), "every record arrived exactly once");
    CHECK(mixed == 0, "no line contains bytes from both writers");
    CHECK(malformed == 0, "no truncated or fused record survived");
}

/* ---- C3: a stale temp file does not block a write ---------------------- */
/*
 * `write_atomic` names its temp file after the pid. A writer that crashes leaves
 * one behind, and the NEXT process to reuse that pid would inherit it. With
 * O_TRUNC the new writer silently overwrites it (fine, but it also cannot tell
 * a stale file from a live one). With O_EXCL the open fails - so the lock,
 * which makes clearing it safe, is what turns a spurious failure into a
 * correct one. This test builds that exact state by hand.
 *
 * It does NOT test the O_EXCL flag itself: the unlink above means the open never
 * sees the stale file, so O_EXCL vs O_TRUNC is unobservable here and the
 * sabotage matrix confirms both pass. What is tested is the RECOVERY - that a
 * crashed writer's leftovers do not permanently wedge a path.
 */
static void test_c3_stale_temp_file(void) {
    char sdir[512], stale[700];
    snprintf(sdir, sizeof sdir, "%s/c3stale", tmpdir);
    aios_dur_set_root(sdir);
    (void)aios_mkdirs(sdir);

    /* the temp name write_atomic will choose, for THIS process */
    snprintf(stale, sizeof stale, "%s/state.json.tmp.%d", sdir, (int)getpid());
    write_plain(stale, "garbage left by a crashed writer");

    CHECK(aios_dur_write_atomic("state.json", "{\"ok\":true}", 11) == AIOS_OK,
          "a stale temp file does not block the write");
    char rb[64] = {0};
    size_t rl = 0;
    CHECK(aios_dur_read("state.json", rb, sizeof rb - 1, &rl) == AIOS_OK &&
          strcmp(rb, "{\"ok\":true}") == 0, "and the write is the new content");
    CHECK(access(stale, F_OK) != 0, "and the stale temp file is gone");
}

/* ---- C3: a journal larger than the read window keeps its TAIL ---------- */
static void test_c3_large_journal_tail(void) {
    char sdir[512];
    snprintf(sdir, sizeof sdir, "%s/c3big", tmpdir);
    aios_dur_set_root(sdir);
    (void)aios_mkdirs(sdir);

    /* COMFORTABLY larger than the 512 KiB window - the first version wrote
     * 5,200 lines of ~87 bytes = 452 KiB, i.e. UNDER the window, and so never
     * entered the clipping path at all. The test passed while testing nothing,
     * and the sabotage matrix found it by failing to fail: with `clipped` forced
     * false the suite stayed green. 9,000 lines of ~100 bytes is 900 KiB. */
    char line[128];
    for (int i = 0; i < 9000; i++) {
        snprintf(line, sizeof line, "{\"seq\":%d,\"pad\":\"%s\"}\n", i,
                 "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
        if (aios_dur_append("big.jsonl", line, strlen(line)) != AIOS_OK) break;
    }
    size_t lines = 0;
    char last[256] = {0};
    CHECK(aios_dur_lines("big.jsonl", &lines, last, sizeof last) == AIOS_OK,
          "a large journal is still readable");
    CHECK(lines > 1000, "the window returns a useful number of records");
    /* the important one: `last` must be a WHOLE record, not a fragment of one
       left by clipping in the middle of a line */
    CHECK(last[0] == '{', "the newest record starts at a record boundary");
    CHECK(strstr(last, "\"seq\":") != NULL, "and is a real record, not a fragment");
    CHECK(strstr(last, "8999") != NULL, "and it is the NEWEST record, not an old one");
}

/* ---- C4: `log` reports unavailable and empty differently --------------- */
static void test_c4_log_builtin(void) {
    char sdir[512], jdir[512];
    snprintf(sdir, sizeof sdir, "%s/c4", tmpdir);
    snprintf(jdir, sizeof jdir, "%s/c4j", tmpdir);
    /* a store directory that has never been used, so "empty" is genuinely empty.
       The earlier version reused a path the durability tests had already written
       a boot.log into, so the "empty ring" checks failed against a ring that
       was not empty - the test was wrong, not the builtin. */
    char rm[1400];
    snprintf(rm, sizeof rm, "rm -rf '%s' '%s'", sdir, jdir);
    (void)!system(rm);
    aios_dur_set_root(sdir);
    (void)aios_mkdirs(sdir);
    (void)aios_mkdirs(jdir);

    sh_shell *sh = sh_create(jdir, sdir);
    CHECK(sh != NULL, "shell for the log builtin");
    if (!sh) return;
    sh_result r;

    /* empty ring: says empty */
    sh_exec(sh, "log", &r);
    CHECK(r.status == 0, "empty log is not an error");
    CHECK(strstr(r.text, "empty") != NULL, "and says the ring is empty");

    /* write a ring, then read it back through the shell */
    CHECK(aios_dur_write_atomic("boot.log",
              "[init] mount ext2 ok\n[init] governor died (killed by signal 9)\n",
              74) == AIOS_OK, "boot ring written");
    sh_exec(sh, "log", &r);
    CHECK(r.status == 0, "log reads the ring");
    CHECK(strstr(r.text, "governor died") != NULL, "and shows the boot lines");

    /* -n keeps the NEWEST n lines, which is the only end anyone wants */
    sh_exec(sh, "log -n 1", &r);
    CHECK(r.status == 0, "log -n works");
    CHECK(strstr(r.text, "governor died") != NULL && strstr(r.text, "mount ext2") == NULL,
          "log -n 1 returns the newest line only");

    /* Unavailable is NOT empty.
     *
     * A DIRECTORY named boot.log is the honest way to reach that branch: it
     * exists (so this is not "never written") and reading it as a file fails.
     * Two approaches were tried first and both were tests of nothing: a store
     * path under /proc/self/mem, which sh_create rejects before any builtin
     * runs, and a merely-absent directory, which is correctly reported as
     * EMPTY and so never exercises the case. */
    {
        char dirring[700];
        snprintf(dirring, sizeof dirring, "%s/boot.log", sdir);
        /* rmdir fails on a FILE, and aios_mkdirs is happy to leave an existing
         * file in place and report OK - so the earlier boot.log from this very
         * test survived, and `log` dutifully read it. That is how a ring that
         * was supposed to be unreadable turned out to be a readable file of 74
         * bytes. unlink first, then the directory is what gets created. */
        (void)unlink(dirring);
        (void)rmdir(dirring);
        const aios_status mk = aios_mkdirs(dirring);
        CHECK(mk == AIOS_OK, "a directory placed where the ring should be");
        CHECK(aios_dur_exists("boot.log"), "and the store sees it as present");
    }
    sh_exec(sh, "log", &r);
    /* the message lands in r.error, not r.text: a failed builtin reports its
     * reason out-of-band (RULES 1.2), so a test that only looked at text would
     * read "no output" as "no complaint" */
    CHECK(r.status != 0, "an unreadable ring is reported as a failure");
    CHECK(r.error && strstr(r.error, "unavailable") != NULL,
          "and the reason is 'unavailable', not 'empty'");
    CHECK(r.text == NULL || strstr(r.text, "empty") == NULL,
          "and never as an empty log");

    sh_free(sh);
    aios_dur_set_root(NULL);
}

TEST_MAIN()
