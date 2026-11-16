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

static char tmpdir[256] = {0};

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

    rm_tmpdir();
    aios_dur_set_root(NULL);
}

TEST_MAIN()
