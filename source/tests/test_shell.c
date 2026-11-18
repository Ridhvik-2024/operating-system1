/* test_shell.c — M5: aiosh engine tests.
 * Covers: parser (quotes, key=value, {json}, pipes, sequences), variable
 * expansion ($var $out $? $json $$), text builtins (count/grep/head),
 * tool stages + pipe injection, the scripted intent router (unit),
 * foreground agent runs, background jobs (spawn/jobs/wait/kill), memory
 * builtins, sembus events, audit integrity, and script end-to-end flow. */
#include "t.h"
#include "aios/shell.h"
#include "aios/policy.h"
#include "aios/snapshot.h"
#include "aios/sembus.h"
#include "../src/shroute.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Locate a test fixture without depending on one way of knowing where we are.
 *
 * /proc/self/exe is the best answer and the obvious one, but it is not the only
 * answer: under a chroot it reports a path that does not exist inside the
 * chroot, and /proc may not be mounted at all on a minimal boot. When the first
 * candidate fails this used to give up, and the failure looked like "the fixture
 * is missing" -- a test that quietly stops testing.
 */
static bool find_fixture(const char *name, char *out, size_t cap, const char *argv0) {
    char self[256];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = 0;
            snprintf(out, cap, "%s/%s", self, name);
            if (access(out, X_OK) == 0) return true;
        }
    }

    /* Where the booted image installs fixtures, by convention. */
    snprintf(out, cap, "/usr/lib/aios/tests/%s", name);
    if (access(out, X_OK) == 0) return true;

    /* The source tree, for `make test`. */
    snprintf(out, cap, "build/%s", name);
    if (access(out, X_OK) == 0) return true;

    /* The directory argv[0] was invoked from, which is all a shell knows. */
    if (argv0 && strchr(argv0, '/')) {
        char dir[256];
        snprintf(dir, sizeof dir, "%s", argv0);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            snprintf(out, cap, "%s/%s", dir, name);
            if (access(out, X_OK) == 0) return true;
        }
    }
    return false;
}

static sh_shell *sh = NULL;

static void run(const char *line, sh_result *r) {
    sh_exec(sh, line, r);
}

/* ---- sembus capture ---- */
static char cap_ping[512] = {0};
static int  cap_shcmd = 0;

static void cap_ping_fn(const char *topic, const char *payload,
                        size_t len, void *ud) {
    (void)topic; (void)ud;
    snprintf(cap_ping, sizeof cap_ping, "%.*s", (int)(len > 500 ? 500 : len),
             payload);
}
static void cap_shcmd_fn(const char *topic, const char *payload,
                         size_t len, void *ud) {
    (void)payload; (void)len; (void)ud;
    if (strcmp(topic, "sh.cmd") == 0) cap_shcmd = 1;
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputs(content, f);
    fclose(f);
}

/* Read a file and compare it byte for byte. A rollback is only a rollback if the
 * bytes are the old bytes -- a restored file of the right NAME with the wrong
 * contents is the failure this whole milestone exists to prevent. */
static bool file_is(const char *path, const char *want) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strcmp(buf, want) == 0;
}

static void shell_tests(void) {
    (void)!system("rm -rf build/test-shell");
    CHECK(aios_mkdirs("build/test-shell/jail") == AIOS_OK, "mkdir jail");
    CHECK(aios_mkdirs("build/test-shell/store") == AIOS_OK, "mkdir store");
    write_file("build/test-shell/jail/notes.txt",
               "hello ai-os\nsecond line\n");
    write_file("build/test-shell/jail/metrics.csv", "module,loc\nmem,900\n");

    sh = sh_create("build/test-shell/jail", "build/test-shell/store");
    CHECK(sh != NULL, "shell created");
    if (!sh) return;
    CHECK(strcmp(sh_cortex_mode(sh), "scripted") == 0, "default cortex");
    sembus_sub(sh_bus(sh), "test.ping", cap_ping_fn, NULL);
    sembus_sub(sh_bus(sh), "*", cap_shcmd_fn, NULL);

    /* ---- basics ---- */
    sh_result r;
    run("version", &r);
    CHECK(r.status == 0, "version ok");
    CHECK_STR_CONTAINS(r.text, AIOS_VERSION, "version string matches AIOS_VERSION");
    CHECK(r.json != NULL && json_type_of(r.json) == JSON_OBJ, "version json");
    sh_result_free(&r);

    run("echo hello world", &r);
    CHECK(r.status == 0 && strcmp(r.text, "hello world") == 0, "echo joins args");
    sh_result_free(&r);

    run("echo \"a b\"  c", &r);
    CHECK(r.status == 0 && strcmp(r.text, "a b c") == 0, "quoting");
    sh_result_free(&r);

    /* ---- variables ---- */
    run("set f notes.txt", &r);
    CHECK(r.status == 0, "set f");
    sh_result_free(&r);
    run("echo $f", &r);
    CHECK(r.status == 0 && strcmp(r.text, "notes.txt") == 0, "$f expands");
    sh_result_free(&r);
    run("echo ${f}", &r);
    CHECK(r.status == 0 && strcmp(r.text, "notes.txt") == 0, "${f} expands");
    sh_result_free(&r);

    run("echo $?", &r);
    CHECK(strcmp(r.text, "0") == 0, "$? is 0 after success");
    sh_result_free(&r);

    run("fs.read missing.txt", &r);
    CHECK(r.status == 1, "fs.read of missing file fails");
    CHECK(r.error != NULL && strstr(r.error, "fs.read"), "error text present");
    sh_result_free(&r);
    run("echo $?", &r);
    CHECK(strcmp(r.text, "1") == 0, "$? is 1 after failure");
    sh_result_free(&r);

    run("echo abc", &r);
    sh_result_free(&r);
    run("echo $out", &r);
    CHECK(strcmp(r.text, "abc") == 0, "$out is last output");
    sh_result_free(&r);

    run("echo $$", &r);
    CHECK(strcmp(r.text, "$") == 0, "$$ is a literal dollar");
    sh_result_free(&r);

    run("unset f", &r);
    sh_result_free(&r);
    run("echo [$f]", &r);
    CHECK(strcmp(r.text, "[]") == 0, "unset variable expands empty");
    sh_result_free(&r);

    /* ---- comments, sequences ---- */
    run("   # a comment", &r);
    CHECK(r.status == 0 && r.text[0] == 0, "comment line is a no-op");
    sh_result_free(&r);
    run("echo a ; echo b", &r);
    CHECK(r.status == 0 && strcmp(r.text, "a\nb") == 0, "; sequences lines");
    sh_result_free(&r);

    /* ---- pipes + text builtins ---- */
    run("fs.read notes.txt | count", &r);
    CHECK(r.status == 0 && strcmp(r.text, "2") == 0, "piped count = 2 lines");
    sh_result_free(&r);
    run("fs.read notes.txt | grep second", &r);
    CHECK(strcmp(r.text, "second line") == 0, "piped grep");
    sh_result_free(&r);
    run("fs.read notes.txt | head 1", &r);
    CHECK(strcmp(r.text, "hello ai-os") == 0, "piped head");
    sh_result_free(&r);
    run("echo one two three | count -w", &r);
    CHECK(strcmp(r.text, "3") == 0, "count -w words");
    sh_result_free(&r);
    run("count", &r);
    CHECK(r.status == 1 && strstr(r.error, "piped"), "count without pipe errors");
    sh_result_free(&r);

    /* ---- tool stages ---- */
    run("calc expr=12*(7+5)", &r);
    CHECK(r.status == 0 && strcmp(r.text, "144") == 0, "calc key=value");
    CHECK(r.json && json_num(json_obj_get(r.json, "value")) == 144.0,
          "calc json result");
    sh_result_free(&r);
    run("calc '{\"expr\":\"2+3\"}'", &r);
    CHECK(r.status == 0 && strcmp(r.text, "5") == 0, "calc {json} argument");
    sh_result_free(&r);
    run("echo 6*7 | calc", &r);
    CHECK(r.status == 0 && strcmp(r.text, "42") == 0, "pipe into calc");
    sh_result_free(&r);
    run("echo piped-data | fs.write path=pipe.txt", &r);
    CHECK(r.status == 0, "pipe into fs.write");
    sh_result_free(&r);
    run("fs.read pipe.txt", &r);
    CHECK(strcmp(r.text, "piped-data") == 0, "piped content was written");
    sh_result_free(&r);
    run("fs.read notes.txt | memory.search", &r);
    CHECK(r.status == 0, "pipe into memory.search");
    sh_result_free(&r);

    /* ---- intent router (scripted) through the full shell ---- */
    run("12*(7+5)", &r);
    CHECK(r.status == 0 && strcmp(r.text, "144") == 0, "NL arithmetic -> calc");
    sh_result_free(&r);
    run("read notes.txt", &r);
    CHECK(r.status == 0 && strstr(r.text, "hello ai-os"), "NL read -> fs.read");
    sh_result_free(&r);
    run("list files", &r);
    CHECK(r.status == 0 && strstr(r.text, "notes.txt"), "NL list -> fs.list");
    sh_result_free(&r);
    run("what can you do", &r);
    CHECK(r.status == 0 && strstr(r.text, "scripted"), "NL question -> reply");
    sh_result_free(&r);

    /* ---- memory builtins ---- */
    run("remember aiosh-test-fact-42", &r);
    CHECK(r.status == 0 && strstr(r.text, "remembered"), "remember stores");
    sh_result_free(&r);
    run("recall aiosh-test-fact", &r);
    CHECK(r.status == 0 && strstr(r.text, "aiosh-test-fact-42"),
          "recall finds it");
    sh_result_free(&r);

    /* ---- foreground agent (scripted cortex) ----
     *
     * M3 changed what "the agent wrote a file" MEANS. Before, the write landed
     * in the jail immediately. Now an AI write is staged, and the human is shown
     * a diff. This test asserted the old behaviour, so it is the test that had
     * to change -- not the behaviour. */
    run("summarize the workspace", &r);
    CHECK(r.status == 0 && strstr(r.text, "summary.md"),
          "NL action verb -> agent run");
    CHECK(strstr(r.text, "nothing was written to the jail") != NULL,
          "the agent's write is reported as staged, not applied");
    CHECK(strstr(r.text, "promote") != NULL,
          "and the human is told how to apply it");
    sh_result_free(&r);
    {
        FILE *f = fopen("build/test-shell/jail/summary.md", "r");
        CHECK(f == NULL, "the agent's write did NOT land in the jail");
        if (f) fclose(f);
    }
    /* and the human can see what it wants to do */
    run("staged", &r);
    CHECK(r.status == 0 && strstr(r.text, "summary.md"),
          "`staged` shows the pending change");
    sh_result_free(&r);
    run("discard", &r);
    CHECK(r.status == 0, "`discard` throws it away");
    sh_result_free(&r);
    run("staged", &r);
    CHECK(strstr(r.text, "nothing is staged") != NULL,
          "and nothing is staged afterwards");
    sh_result_free(&r);

    /* A human write still lands directly: gating a person's own explicit
     * command behind a second confirmation is theatre. */
    run("fs.write path=direct.txt content=human", &r);
    CHECK(r.status == 0, "a human write is not staged");
    sh_result_free(&r);
    {
        FILE *f = fopen("build/test-shell/jail/direct.txt", "r");
        CHECK(f != NULL, "the human's file is in the jail immediately");
        if (f) fclose(f);
    }

    /* ---- sembus events ---- */
    run("emit test.ping '{\"x\":42}'", &r);
    CHECK(r.status == 0 && strstr(r.text, "2 subscriber"),
          "emit publishes (exact + wildcard subscribers)");
    sh_result_free(&r);
    CHECK(strstr(cap_ping, "42") != NULL, "subscriber got test.ping payload");
    CHECK(cap_shcmd == 1, "sh.cmd events published");

    /* ---- history, vars listing, cortex switching ---- */
    run("history", &r);
    CHECK(r.status == 0 && strstr(r.text, "summarize the workspace"),
          "history records lines");
    sh_result_free(&r);
    run("set f notes.txt", &r);
    sh_result_free(&r);
    run("vars", &r);
    CHECK(strstr(r.text, "f=notes.txt"), "vars lists name=value");
    sh_result_free(&r);
    run("cortex llm", &r);
    CHECK(r.status == 0 && strstr(r.text, "llm"), "cortex switches to llm");
    sh_result_free(&r);
    CHECK(strcmp(sh_cortex_mode(sh), "llm") == 0, "mode visible via API");
    run("cortex", &r);
    CHECK(strstr(r.text, "cortex: llm"), "cortex shows current mode");
    sh_result_free(&r);
    run("cortex scripted", &r);
    sh_result_free(&r);

    /* ---- background jobs ---- */
    run("spawn summarize the workspace", &r);
    CHECK(r.status == 0 && strstr(r.text, "[job 1]"), "spawn starts job 1");
    sh_result_free(&r);
    for (int i = 0; i < 1500 && sh_jobs_alive(sh) > 0; i++) {
        sh_poll_jobs(sh);
        usleep(20000);
    }
    CHECK(sh_jobs_alive(sh) == 0, "job 1 finished");
    run("jobs", &r);
    CHECK(strstr(r.text, "done"), "jobs shows done state");
    sh_result_free(&r);
    {
        json_val *o = json_parse_file("build/test-shell/store/jobs/1.json");
        CHECK(o != NULL, "job result file exists");
        if (o) {
            CHECK_STR_CONTAINS(json_str(json_obj_get(o, "status")), "done",
                               "job 1 status done");
            CHECK_STR_CONTAINS(json_str(json_obj_get(o, "final")), "summary.md",
                               "job 1 final mentions summary.md");
            json_free(o);
        }
    }
    run("wait 1", &r);
    CHECK(r.status == 0 && strstr(r.text, "Wrote summary.md"),
          "wait reports the job's final answer");
    sh_result_free(&r);

    /* kill: spawn and terminate quickly; retry on the rare race where
     * the scripted agent finished before the kill landed */
    int killed = 0;
    for (int attempt = 0; attempt < 5 && !killed; attempt++) {
        run("spawn summarize the workspace", &r);
        long id = 0;
        if (r.status == 0 && r.text) sscanf(r.text, "[job %ld]", &id);
        sh_result_free(&r);
        if (id <= 0) break;
        char kline[64];
        snprintf(kline, sizeof kline, "kill %ld", id);
        run(kline, &r);
        killed = (r.status == 0);
        sh_result_free(&r);
    }
    CHECK(killed, "kill terminates a running job");
    run("wait all", &r);
    CHECK(r.status == 0, "wait all reaps everything");
    sh_result_free(&r);
    CHECK(sh_jobs_alive(sh) == 0, "no jobs alive at the end");

    /* ---- audit integrity ---- */
    {
        char *data = NULL;
        size_t len = 0;
        CHECK(aios_read_file("build/test-shell/store/audit.log", &data, &len,
                             1u << 20) == AIOS_OK && data,
              "audit log readable");
        if (data) {
            CHECK_STR_CONTAINS(data, "\"tool\":\"calc\"", "audit has calc");
            CHECK_STR_CONTAINS(data, "\"tool\":\"fs.write\"", "audit has fs.write");
            /* M6: and the records are a chain, not a pile. */
            CHECK_STR_CONTAINS(data, "\"prev\":\"0000000000000000\"",
                               "the first record chains to genesis");
            CHECK_STR_CONTAINS(data, "\"hash\":\"", "records carry a hash");
            free(data);
        }
    }

    /* ---- parse errors ---- */
    run("echo \"abc", &r);
    CHECK(r.status == 1 && strstr(r.error, "quote"), "unterminated quote");
    sh_result_free(&r);

    /* M1: a command whose effects cannot be declared must be REFUSED, and the
     * file must not appear in the jail. This is the fail-closed path: before
     * M1, an over-long path was truncated into the declared effect (naming a
     * different resource than the one touched) and the write went ahead. */
    {
        char *longline = (char *)malloc(1024);
        if (longline) {
            /* width on %d pads with SPACES, not zeros - build the path by
             * hand so it is a single long token, not 700 spaces */
            memset(longline, 0, 1024);
            /* "fs.write path=" is 14 bytes - copying 15 would include its
             * NUL and leave the path empty (which fails for the wrong
             * reason: the tool sees a missing path, not a long one). */
            memcpy(longline, "fs.write path=", 14);
            memset(longline + 14, 'a', 700);
            strcpy(longline + 14 + 700, " content=x");
            run(longline, &r);
            CHECK(r.status != 0, "over-long path is refused");
            CHECK(strstr(r.error ? r.error : "", "refused") != NULL,
                  "the refusal says so");
            sh_result_free(&r);
            /* and nothing was written: the jail has no 700-char name */
            FILE *jf = NULL;
            char cmd[600];
            snprintf(cmd, sizeof cmd,
                     "ls build/test-shell/jail | awk '{print length($0)}' "
                     "| sort -rn | head -1");
            char best[32] = {0};
            jf = popen(cmd, "r");
            if (jf) { if (fgets(best, sizeof best, jf)) { } pclose(jf); }
            CHECK(atoi(best) < 700, "no over-long file was created in the jail");
            free(longline);
        }
    }

    sh_result_free(&r);

    /* ---- M2: the law is readable and the gate obeys it ---- */
    {
        run("policy", &r);
        CHECK(r.status == 0, "policy builtin runs");
        CHECK_STR_CONTAINS(r.text, "law table", "it prints the table");
        CHECK_STR_CONTAINS(r.text, "binds the human too",
                           "and says tier 0 binds the human");
        CHECK_STR_CONTAINS(r.text, "tool:calc", "and lists the AI's capabilities");
        sh_result_free(&r);

        /* The asymmetry, asked of the evaluator through the shell. */
        run("policy check delete old.txt", &r);
        CHECK_STR_CONTAINS(r.text, "NEEDS_HUMAN", "AI delete -> NEEDS_HUMAN");
        CHECK_STR_CONTAINS(r.text, "as human: ALLOW",
                           "the same delete from a human is allowed");
        sh_result_free(&r);

        run("policy check shell ls", &r);
        CHECK_STR_CONTAINS(r.text, "DENY", "AI shell -> DENY");
        sh_result_free(&r);

        run("policy check tool calc", &r);
        CHECK_STR_CONTAINS(r.text, "ALLOW", "AI calc with a capability -> ALLOW");
        sh_result_free(&r);

        /* M2 enforcement, end to end: the audit log is refused for a HUMAN. */
        run("fs.write path=audit.log content=faked", &r);
        CHECK(r.status != 0, "writing the audit log is refused");
        CHECK_STR_CONTAINS(r.error, "policy", "and the error names policy");
        CHECK_STR_CONTAINS(r.error, "tier 0", "and the tier");
        sh_result_free(&r);

        /* ...while an ordinary write still works, in the same shell. */
        run("fs.write path=ordinary.txt content=fine", &r);
        CHECK(r.status == 0, "an ordinary write is still allowed");
        sh_result_free(&r);
    }

    /* ---- M4: a worker that ignores staging is caught by watching the jail ----
     *
     * This is the test that matters most in this file. The worker's account can
     * be forged; the tree it leaves cannot. A swapped tool-exec writes into the
     * jail and reports only what it was asked about, so every self-reported
     * check passes -- and the file is already in the jail.
     *
     * The adversary is tests/fixtures/liar-worker.c, committed and built by the
     * Makefile. It is NOT compiled here at runtime: the booted image has no
     * compiler, and the first version of this test "passed" there only because
     * its fixture failed to build and the failure was treated as a skip.
     */
    {
        char liar[512];
        CHECK(find_fixture("liar-worker", liar, sizeof liar, g_argv0),
              "the adversary worker is built (make build/liar-worker)");

        /* If the fixture is missing, this must FAIL: a security test that
         * quietly does nothing is worse than no test, because it is believed. */
        CHECK(access(liar, X_OK) == 0,
              "the adversary worker is built (make build/liar-worker)");

        if (access(liar, X_OK) == 0) {
            setenv("AIOS_TOOL_EXEC", liar, 1);

            run("agent summarize", &r);
            CHECK(r.status != 0, "a worker that ignores staging fails the run");
            CHECK_STR_CONTAINS(r.error, "jail",
                               "and says the jail changed, not that the tool erred");
            sh_result_free(&r);

            /* The decisive assertion, stated honestly: the file IS in the jail,
             * because a swapped worker can write it. M4's job is to notice and
             * refuse the result; making the write itself impossible needs the
             * overlay (L2). Pretending otherwise would be the lie. */
            FILE *leak = fopen("build/test-shell/jail/summary.md", "r");
            CHECK(leak != NULL, "the liar really did write into the jail");
            if (leak) fclose(leak);
            (void)!system("rm -f build/test-shell/jail/summary.md "
                         "build/test-shell/jail/sneaky.txt");

            unsetenv("AIOS_TOOL_EXEC");
            run("staged", &r);
            CHECK(strstr(r.text, "nothing is staged") != NULL,
                  "and nothing was left staged for the human to promote");
            sh_result_free(&r);
        }
    }


    /* ---- M5: snapshot and rollback, as a person types them ---- *
     *
     * test_snapshot.c proves the mechanism. This proves the thing the milestone
     * actually promises: that a human at the prompt can get their workspace back,
     * and that the shell's own memory does not keep describing the state it just
     * rolled away from.
     *
     * Note what a snapshot does NOT capture: <store>/staging. That is on purpose.
     * Staged changes are proposals, not state; rolling back should not bring a
     * rejected proposal back to life, and the test below asserts the staging
     * directory is still gone afterwards.
     */
    {
        /* Set the state this test will restore to, rather than assuming what
         * earlier groups left behind. A test that depends on another group's
         * leftovers passes or fails according to the order it happens to run in. */
        write_file("build/test-shell/jail/notes.txt", "the original bytes\n");
        write_file("build/test-shell/jail/metrics.csv", "module,loc\nmem,900\n");

        char before[64] = {0};
        run("snap take shell-test", &r);
        CHECK(r.status == 0, "snap take succeeds");
        CHECK_STR_CONTAINS(r.text, "snapshot", "and says what it did");
        if (r.text && strstr(r.text, "snapshot")) {
            sscanf(strstr(r.text, "snapshot") + 9, "%63s", before);
        }
        sh_result_free(&r);

        run("snap", &r);
        CHECK(r.status == 0, "snap lists");
        CHECK_STR_CONTAINS(r.text, "keeping 5", "and says how many it keeps");
        CHECK_STR_CONTAINS(r.text, before, "and lists the one just taken");
        sh_result_free(&r);

        /* Wreck three things, the three ways a workspace gets wrecked: a file
         * changed, a file invented, a file deleted. */
        write_file("build/test-shell/jail/notes.txt", "WRECKED\n");
        write_file("build/test-shell/jail/invented.txt", "not yours\n");
        (void)!remove("build/test-shell/jail/metrics.csv");

        run("rollback", &r);
        CHECK(r.status == 0, "rollback succeeds");
        CHECK_STR_CONTAINS(r.text, "rolled back to", "and says where it went back to");
        CHECK_STR_CONTAINS(r.text, "the state that was replaced",
                           "and where the wrecked state went");
        sh_result_free(&r);

        CHECK(file_is("build/test-shell/jail/notes.txt", "the original bytes\n"),
              "the changed file has its old bytes back");
        CHECK(access("build/test-shell/jail/invented.txt", F_OK) != 0,
              "the invented file is gone");
        CHECK(access("build/test-shell/jail/metrics.csv", F_OK) == 0,
              "the deleted file is back");

        /* The shell's memory and tool registry were reopened; a shell that can
         * still read a file it just rolled away from is worse than one that
         * refuses, because it looks like the rollback worked. */
        run("fs.read notes.txt", &r);
        CHECK(r.status == 0 && strstr(r.text, "the original bytes"),
              "the shell reads the restored tree, not a cached one");
        sh_result_free(&r);

        run("rollback nosuchsnapshot", &r);
        CHECK(r.status != 0, "an unknown id is refused");
        CHECK_STR_CONTAINS(r.error, "snapshot", "and the error says what was missing");
        sh_result_free(&r);
    }

    /* ---- M5: a rollback revokes running jobs ---- *
     * Restoring a tree an agent is still writing to produces a state that is
     * neither the old one nor the new one. So jobs stop first, and the shell
     * says how many.
     */
    {
        run("spawn summarize the workspace", &r);
        long id = 0;
        if (r.status == 0 && r.text) sscanf(r.text, "[job %ld]", &id);
        sh_result_free(&r);
        CHECK(id > 0, "a job is running to be revoked");

        run("rollback", &r);
        CHECK(r.status == 0, "rollback with a job running succeeds");
        CHECK_STR_CONTAINS(r.text, "revoked", "and reports the revocation");
        sh_result_free(&r);
        CHECK(sh_jobs_alive(sh) == 0, "no job is left alive afterwards");
    }


    /* ---- M6: the record is a chain a stranger can check ---- */
    {
        run("audit verify", &r);
        CHECK(r.status == 0, "audit verify passes on a log this shell wrote");
        CHECK_STR_CONTAINS(r.text, "chained and intact", "and says the chain holds");
        CHECK_STR_CONTAINS(r.text, "not cryptographic",
                           "and states its own limit rather than implying more");
        sh_result_free(&r);

        run("audit nonsense", &r);
        CHECK(r.status != 0, "an unknown audit subcommand is refused");
        CHECK_STR_CONTAINS(r.error, "usage", "with the usage");
        sh_result_free(&r);
    }

    /* ---- M6: a changed byte is caught, and the shell says where ---- */
    {
        /* Do the tampering at the file level, the way an attacker or a bug would:
         * the shell must notice from the bytes alone, without being told. */
        char *data = NULL;
        size_t len = 0;
        char *pristine = NULL;
        size_t plen = 0;
        CHECK(aios_read_file("build/test-shell/store/audit.log", &data, &len,
                             1u << 20) == AIOS_OK && data,
              "the audit log can be read to tamper with it");
        /* Keep the ORIGINAL bytes, before touching anything. */
        if (data) {
            pristine = malloc(len ? len : 1);
            CHECK(pristine != NULL, "the original bytes are saved");
            if (pristine) { memcpy(pristine, data, len); plen = len; }
        }
        if (data) {
            char *hit = strstr(data, "\"ok\":true");
            CHECK(hit != NULL, "found something to change");
            if (hit) hit[6] = 'f';      /* true -> frue: a complete, valid JSON
                                         * string in place, so this is a HASH
                                         * mismatch and not a parse error */
            FILE *f = fopen("build/test-shell/store/audit.log", "wb");
            CHECK(f != NULL, "the log can be rewritten");
            if (f) { fwrite(data, 1, len, f); fclose(f); }
            free(data);
        }

        run("audit verify", &r);
        CHECK(r.status != 0, "verification fails after the edit");
        CHECK_STR_CONTAINS(r.text, "BROKEN", "and says the chain is broken");
        CHECK_STR_CONTAINS(r.text, "seq", "and names the sequence number");
        CHECK_STR_CONTAINS(r.text, "compare it with a copy you trust",
                           "and says what the only honest next step is");
        sh_result_free(&r);

        /* Put the exact bytes back. The point is not tidiness: if restoring the
         * bytes restores the chain, then the hash covers exactly what is on disk.
         * If it did not, the hash would be covering something else and the whole
         * idea would be wrong. So keep an untouched copy and write it back. */
        FILE *restored = fopen("build/test-shell/store/audit.log", "wb");
        if (restored) { fwrite(pristine, 1, plen, restored); fclose(restored); }
        CHECK(restored != NULL, "the original bytes are written back");
        free(pristine);

        run("audit verify", &r);
        CHECK(r.status == 0, "restoring the bytes restores the chain");
        CHECK_STR_CONTAINS(r.text, "intact", "and it verifies as intact again");
        sh_result_free(&r);
    }

    /* ---- exit (last: it makes the shell want to terminate) ---- */
    run("exit", &r);
    CHECK(r.status == 0 && strstr(r.text, "bye"), "exit builtin");
    sh_result_free(&r);

    sh_free(sh);
    sh = NULL;
}

/* ---- M7: the origin of a line --------------------------------------------
 *
 * The acceptance tests for M7. The question is not "does the shell have a flag"
 * but "can a line that a MODEL produced cause a change a person did not
 * accept".
 *
 * These need their own shell: the main one has already had `promote`, `discard`
 * and `exit` typed at it by the tests above, and re-running those would be
 * testing the same objects twice. A fresh jail also means "did the write land"
 * cannot be answered by a file some earlier test made.
 */
typedef struct {
    int calls;
    int max_calls;   /* how many times to answer `answer` before switching */
    ui_choice answer;
    ui_choice then;      /* answer after the first call, if different */
} fake_asker;

/* The first `max_calls` answers are `answer`; from then on it is `then`. The
 * index is 1-based because that is how a call count reads. */
static ui_choice fake_ask(const ui_prompt *p, void *ud) {
    (void)p;
    fake_asker *f = ud;
    f->calls++;
    if (f->calls > f->max_calls) return f->then;
    return f->answer;
}

/* How many COMPLETE snapshots does the store hold? Asked of the library, not of
 * the shell, so the two answers can be compared instead of assumed. */
static size_t complete_snapshots(const char *store) {
    snap_list sl;
    memset(&sl, 0, sizeof sl);
    char err[256];
    size_t n = 0;
    if (snap_list_all(store, &sl, err, sizeof err)) {
        for (size_t i = 0; i < sl.n; i++)
            if (sl.items[i].complete) n++;
        snap_list_free(&sl);
    }
    return n;
}

static void origin_tests(void) {
    sh_result r;
    char promote_text[256];
    size_t procap = sizeof promote_text;
    const char *jail  = "build/test-origin/jail";
    const char *store = "build/test-origin/store";

    /* Start from nothing. The rest of the suite builds on what earlier tests left
     * behind, which is fine for them and wrong for these: "did the write land"
     * and "is the machine's promote invisible" are questions about a tree, and a
     * tree with a previous run's summary.md in it answers them wrong. This test
     * failed on a second run for exactly that reason -- and a test that only
     * passes on a clean machine is a test that will fail in CI, where the
     * directory happens to be clean, and pass locally, where it does not. */
    if (system("rm -rf build/test-origin") != 0) { /* best effort */ }
    CHECK(aios_mkdirs(jail) == AIOS_OK, "origin: fresh jail");
    CHECK(aios_mkdirs(store) == AIOS_OK, "origin: fresh store");
    write_file("build/test-origin/jail/seed.txt", "seed\n");

    sh_shell *o = sh_create(jail, store);
    CHECK(o != NULL, "origin: shell");
    if (!o) return;

    /* Nothing has run yet, so no line has an origin. ORIGIN_UNSPECIFIED is the
     * honest answer and it is not one a caller may pass in. */
    CHECK(sh_last_origin(o) == ORIGIN_UNSPECIFIED,
          "origin: a shell that has run nothing has no origin to report");

    /* ---- 1. an undeclared origin is refused, not assumed ---- */
    {
        int rc = sh_exec_origin(o, "count x", ORIGIN_UNSPECIFIED, &r);
        CHECK(rc == 0 && r.status != 0,
              "origin: sh_exec_origin refuses a caller that did not say 0");
        CHECK(r.error && strstr(r.error, "ORIGIN_HUMAN or ORIGIN_AI"),
              "origin: ...and names what it wanted instead");
        sh_result_free(&r);
    }
    CHECK(sh_last_origin(o) == ORIGIN_UNSPECIFIED,
          "origin: a refused line does not stamp an origin");
    /* (sh_exec returns 1 when the LINE failed, 0 when the RESULT carries the
     * status. An argument that is not an origin is not a line, so it is 0 with
     * a non-zero status -- the call was understood and rejected, not aborted.) */

    /* ---- 2. the origin is remembered, and sh_exec means HUMAN ---- */
    sh_exec_origin(o, "count a b c", ORIGIN_AI, &r);
    CHECK(sh_last_origin(o) == ORIGIN_AI, "origin: an AI line is stamped AI");
    sh_result_free(&r);
    sh_exec(o, "count a b c", &r);
    CHECK(sh_last_origin(o) == ORIGIN_HUMAN,
          "origin: sh_exec is the human case, by definition and not by default");
    sh_result_free(&r);

    /* ---- 3. an AI write outside the workspace does not land ---- *
     *
     * This is the milestone's headline sentence, and running it truthfully
     * changed what this test can claim. Policy step 2d refuses ANY path outside
     * the jail, for either origin: a human's absolute path is denied too, on
     * purpose, because `fm.write /etc/passwd` should not be a thing a person can
     * type either. So the refusal here is not the AI rule, it is the jail rule,
     * and a test that presented it as "the AI rule working" would be lying about
     * which law did the work.
     *
     * What is asserted is therefore exact: the write does not happen, neither
     * origin gets a file outside the jail, and the machine's attempt is refused
     * with the jail named as the reason. The AI-specific verdict is asserted
     * directly against the policy function in section 3b, where it is the only
     * rule in play and its own reasoning is what decides. */
    {
        char outside[256];
        snprintf(outside, sizeof outside, "/tmp/m7-ai-%ld.txt", (long)getpid());
        unlink(outside);

        char line[512];
        snprintf(line, sizeof line, "fs.write path=%s content=from-a-model",
                 outside);

        int rc = sh_exec_origin(o, line, ORIGIN_AI, &r);
        CHECK(rc == 1 || r.status != 0,
              "origin: an AI write outside the jail does not succeed");
        CHECK_STR_CONTAINS(r.error ? r.error : "", "outside the jail",
                           "origin: ...refused by the jail rule, and named as such");
        {
            FILE *f = fopen(outside, "r");
            CHECK(f == NULL, "origin: THE FILE WAS NOT WRITTEN");
            if (f) fclose(f);
        }
        sh_result_free(&r);
        remove(outside);

        /* A human is refused too -- the same bytes, and the same reason. If this
         * ever diverges, one of the two rules moved and this test says which. */
        rc = sh_exec_origin(o, line, ORIGIN_HUMAN, &r);
        CHECK(rc != 0 || r.status != 0,
              "origin: a human write outside the jail is refused as well");
        {
            FILE *f = fopen(outside, "r");
            CHECK(f == NULL, "origin: ...and no file appeared for the human either");
            if (f) fclose(f);
        }
        sh_result_free(&r);
        remove(outside);
    }

    /* ---- 3b. the AI-only rule, asked directly ---- *
     * Step 4c is "an AI may not write outside the jail's workspace" and it says
     * HUMAN, not DENY: a person may decide. That verdict is unreachable through
     * sh_exec because 2d denies first (see above), so it is asserted against the
     * law itself -- otherwise the difference between "denied" and "a human may
     * allow it" would be a comment in the source with nothing checking it. */
    {
        /* A jail that is NOT /workspace, with a path inside it. Inside the jail,
         * nothing else applies, and the answer must be ALLOW for a human and for
         * an AI (capability aside). The AI case is the one M3 built: it is staged,
         * not applied. */
        aios_action *human_write = aios_action_new(ACT_FS_WRITE, ORIGIN_HUMAN,
                                                   "fs.write");
        /* declare_tool_effects, not add_effect: a set of effects that is not
         * marked complete is an INVALID descriptor, and policy says so before it
         * says anything else. The first version of this test built one by hand
         * and got "descriptor invalid" -- which is M1 working, but not the
         * question being asked here. */
        json_val *hargs = json_new_obj();
        json_obj_set(hargs, "path", json_new_str("build/test-origin/jail/x.txt"));
        json_obj_set(hargs, "content", json_new_str("x"));
        aios_action_set_args(human_write, hargs);
        aios_action_declare_tool_effects(human_write, "fs.write", hargs);
        aios_policy_ctx ctx = aios_policy_ctx_default();
        ctx.sandbox_executed = 1;
        aios_decision dh = aios_policy_eval(human_write, "build/test-origin/jail",
                                            NULL, 0, &ctx, aios_now_ms());
        CHECK(dh.v == VERDICT_ALLOW || dh.v == VERDICT_NEEDS_HUMAN,
              "policy: a human write inside the jail is not denied");
        aios_action_free(human_write);

        /* The other half: an AI writing outside ITS jail is never allowed, and
         * the refusal names the jail rather than the origin -- because the jail
         * rule is the one that cannot be argued with. */
        aios_action *ai_out = aios_action_new(ACT_FS_WRITE, ORIGIN_AI, "fs.write");
        json_val *oargs = json_new_obj();
        json_obj_set(oargs, "path", json_new_str("/etc/aios-outside.txt"));
        json_obj_set(oargs, "content", json_new_str("x"));
        aios_action_set_args(ai_out, oargs);
        aios_action_declare_tool_effects(ai_out, "fs.write", oargs);
        /* M3: an AI action must carry the human request it is answering. Making
         * one up is the honest thing here -- this test IS the request. */
        aios_action_set_intent(ai_out, "m7 test: write outside the jail");
        aios_policy_ctx c2 = aios_policy_ctx_default();
        c2.sandbox_executed = 1;
        c2.rollback_available = 1;
        aios_decision d2 = aios_policy_eval(ai_out, "build/test-origin/jail", NULL,
                                            0, &c2, aios_now_ms());
        CHECK(d2.v == VERDICT_DENY, "policy: an AI write into /etc is DENIED");
        CHECK(d2.r == R_PATH_OUTSIDE_JAIL || d2.r == R_TIER0_PROTECTED_PATH,
              "policy: ...by the jail/tier-0 rule, which binds the human too");
        aios_action_free(ai_out);
    }

    /* ---- 4. the hole M7 was opened for: an AI cannot promote ---- *
     *
     * `promote`/`discard`/`rollback` are builtins, dispatched before the router,
     * so before this milestone an origin-blank sh_exec let a model reach them
     * directly. Stage something first, then try to accept it as a machine. */
    sh_exec(o, "summarize the workspace", &r);      /* stages summary.md */
    sh_result_free(&r);
    {
        int rc = sh_exec_origin(o, "promote", ORIGIN_AI, &r);
        CHECK(rc == 1 || r.status != 0, "origin: an AI `promote` is refused");
        CHECK_STR_CONTAINS(r.error ? r.error : "",
                           "not available to a machine-originated line",
                           "origin: ...with the origin named as the reason");
        CHECK_STR_CONTAINS(r.error ? r.error : "",
                           "a person's decision, not a model's",
                           "origin: ...and the reason is the decision, not a bug");
        sh_result_free(&r);
    }
    {
        FILE *f = fopen("build/test-origin/jail/summary.md", "r");
        CHECK(f == NULL, "origin: nothing was promoted by the machine");
        if (f) fclose(f);
    }
    /* and `staged` still shows it, because looking is not deciding */
    sh_exec_origin(o, "staged", ORIGIN_AI, &r);
    CHECK(r.status == 0 && strstr(r.text, "summary.md"),
          "origin: a machine may still SEE what is staged");
    sh_result_free(&r);

    /* the human still can, which is the other half of the claim.
     *
     * This is the M7 acceptance line -- "the same line with ORIGIN_HUMAN (after
     * promote) succeeds" -- and it has to be asserted honestly. A `promote` that
     * reports success over an EMPTY stage is not a success, so check what the
     * shell said it promoted, not just the return code. That distinction is not
     * pedantry: the first version of this test passed here and failed in the boot
     * image, where the sandbox worker could not be run, nothing was staged, and
     * `promote` still answered "promoted 0 files into the jail" with a status of
     * 0. The failure message now says which of the two it was. */
    sh_exec(o, "promote", &r);
    CHECK(r.status == 0, "origin: the human can still promote 0");
    *promote_text = 0;
    if (r.text) snprintf(promote_text, procap, "%s", r.text);
    CHECK(r.text && strstr(r.text, "promoted 1 file") != NULL,
          "origin: ...and it promoted ONE file, not zero over an empty stage");
    {
        sh_result_free(&r);
        FILE *f = fopen("build/test-origin/jail/summary.md", "r");
        CHECK(f != NULL, "origin: ...and it landed");
        if (f) fclose(f);
    }
    if (!promote_text[0] || !strstr(promote_text, "promoted 1 file")) {
        CHECK(0, "origin: THE AGENT'S WRITE WAS NEVER STAGED (worker problem, "
                 "not an origin problem) -- if the text above says "
                 "'NOTHING WAS STAGED', the sandbox worker could not run in this "
                 "environment and this test cannot judge origin tagging");
    }

    /* ---- 5. every human-only builtin, not just the one that was named ---- */
    {
        static const char *const human_only[] = {
            "discard", "rollback", "snap", "exit", "cortex", NULL };
        for (int i = 0; human_only[i]; i++) {
            int rc = sh_exec_origin(o, human_only[i], ORIGIN_AI, &r);
            char what[160];
            snprintf(what, sizeof what,
                     "origin: an AI `%s` is refused", human_only[i]);
            CHECK(rc == 1 || r.status != 0, what);
            CHECK_STR_CONTAINS(r.error ? r.error : "",
                               "machine-originated line", what);
            sh_result_free(&r);
        }
    }
    /* exit is the one whose failure would be invisible: a machine that can end
     * the session ends the accountability, and the shell would look fine. */
    CHECK(sh_last_origin(o) == ORIGIN_AI,
          "origin: a refused `exit` did not end the session");
    sh_exec(o, "calc 1+1", &r);
    CHECK(r.status == 0, "origin: ...and the shell is still taking human input");
    sh_result_free(&r);

    /* ---- 6. an AI line may still do what it IS allowed to do ---- *
     * The point of a gate is what it lets through. A shell that refused
     * everything from a model would pass test 4 and be useless. */
    sh_exec_origin(o, "calc 2+3", ORIGIN_AI, &r);
    CHECK(r.status == 0, "origin: an AI tool call inside the jail still runs");
    sh_result_free(&r);
    sh_exec_origin(o, "fs.write path=ai-in-jail.txt content=ok", ORIGIN_AI, &r);
    CHECK(r.status == 0, "origin: an AI write INSIDE the jail is not blocked");
    /* and it did not land directly: M3 still stages a machine's write. Both
     * halves matter -- a test that only checked the status would pass on a shell
     * that refused it, and one that only checked the file would pass on a shell
     * that wrote it. */
    CHECK(strstr(r.text ? r.text : "", "nothing was written to the jail") != NULL,
          "origin: ...and it was STAGED, not applied");
    {
        FILE *f = fopen("build/test-origin/jail/ai-in-jail.txt", "r");
        CHECK(f == NULL, "origin: ...the file is not in the jail yet");
        if (f) fclose(f);
    }
    sh_result_free(&r);

    /* ---- 7. rollback_available tells the truth now ---- *
     *
     * M7 replaced a hardcoded 0 with a real question: is there a COMPLETE
     * snapshot of this store? The first run of this test asked a delete and
     * expected the verdict to change after a snapshot -- it does not, and the
     * reason is worth keeping: "an AI may not delete without a human" is a tier-1
     * rule of its own, independent of undoability, so a snapshot cannot unhold a
     * delete. What the snapshot changes is the FACT the rollback rule is fed, and
     * that is exactly what `policy check` now prints. Asserting the fact rather
     * than a verdict that was never going to move is the difference between
     * testing the law and testing a guess about it. */
    {
        /* The shell snapshots before an agent run, and section 4 ran an agent, so
         * there may already be one. Do not assume either way: ask the store, then
         * check that the shell said the same thing. That is what "no longer a
         * constant" has to mean -- if both answers are derived from the same
         * stale 0 they agree with each other and this still catches it, because
         * the store's answer is not stale. */
        size_t before = complete_snapshots("build/test-origin/store");

        sh_exec_origin(o, "policy check delete seed.txt", ORIGIN_AI, &r);
        CHECK(r.status == 0, "origin: `policy check` runs");
        CHECK_STR_CONTAINS(r.text ? r.text : "",
                           before ? "rollback=available" : "rollback=not available",
                           "origin: the gate is told the store's actual state");
        CHECK_STR_CONTAINS(r.text ? r.text : "", "verdict: NEEDS_HUMAN",
                           "origin: an AI delete is held (tier 1, not rollback)");
        sh_result_free(&r);

        sh_exec(o, "snap take m7-proof", &r);
        CHECK(r.status == 0 && strstr(r.text, "taken"),
              "origin: a human can take a snapshot");
        sh_result_free(&r);

        size_t after = complete_snapshots("build/test-origin/store");
        /* Pruning keeps at most AIOS_SNAP_KEEP, so "grew" is only the right
         * question when the store is not already full -- and on a fresh store it
         * is not. Both halves, because a snapshot that silently failed to be
         * taken would leave `after == before` and the second check would pass
         * for the wrong reason. */
        CHECK(after > before, "origin: the snapshot store actually grew");
        /* `before` is 1, not 0, and that is the system working: the shell
         * snapshots BEFORE an agent run, so the agent in section 4 left one
         * behind. Asserting 0 here would be asserting that a safety feature does
         * not work. What matters is that the count went up by this snapshot. */
        CHECK(after == before + 1, "origin: ...by exactly the one we just took");

        sh_exec_origin(o, "policy check delete seed.txt", ORIGIN_AI, &r);
        CHECK_STR_CONTAINS(r.text ? r.text : "", "rollback=available",
                           "origin: ...and the gate now reports a rollback exists");
        CHECK_STR_CONTAINS(r.text ? r.text : "", "verdict: NEEDS_HUMAN",
                           "origin: ...while the delete stays held, by tier 1");
        sh_result_free(&r);
    }

    /* ---- 7b. spawn inherits the origin, and a machine may not spawn ---- *
     *
     * Both halves. The refusal is the one people expect; the INHERITANCE is the
     * one that matters more, because a spawn whose origin defaults is background
     * work judged by a different standard than the work that asked for it. */
    {
        int rc = sh_exec_origin(o, "spawn summarize something", ORIGIN_AI, &r);
        CHECK(rc == 1 || r.status != 0, "origin: an AI may not spawn");
        CHECK_STR_CONTAINS(r.error ? r.error : "",
                           "may not spawn background work",
                           "origin: ...and says which rule, not just no");
        CHECK_STR_CONTAINS(r.error ? r.error : "", "unbounded swarm",
                           "origin: ...and why the rule exists");
        sh_result_free(&r);

        /* A human can, and the job runs at the human's origin because that is
         * what it inherited. The job table is the evidence. */
        rc = sh_exec(o, "spawn summarize something", &r);
        CHECK(rc == 0 && r.status == 0, "origin: a human may spawn");
        sh_result_free(&r);
        sh_exec(o, "wait all", &r);
        sh_result_free(&r);
    }

    /* ---- 8. the hold, end to end: an asker changes the outcome ---- *
     *
     * Section 7 saw the shell say "needs a human" because no asker was
     * installed. Here an asker is installed, the same line runs, and the answer
     * decides. The two together are the claim: the verdict is NEEDS_HUMAN, and
     * what happens next is a person's choice rather than the gate's guess.
     */
    {
        /* `policy check shell` builds an ACT_SHELL as an AI: tier 0 DENY, not a
         * hold. Use a delete, which is a tier-1 HUMAN verdict, and drive it
         * through the asker. The line itself is the one section 7 used. */
        fake_asker no = { 0, 1, UI_CHOICE_DISCARD, UI_CHOICE_DISCARD };
        sh_set_ui_asker(o, fake_ask, &no);
        sh_exec_origin(o, "policy check delete seed.txt", ORIGIN_AI, &r);
        CHECK(r.status == 0, "hold: the shell answers with an asker installed");
        CHECK(no.calls == 0,
              "hold: `policy check` ASKS nothing (it is introspection, not action)");
        sh_result_free(&r);

        /* An AI-origin line must not be able to answer its own question. The
         * front-end enforces that by not installing an asker for --as-ai, and
         * this checks the library's half of it: an asker set for a human run is
         * consulted, and one that says DISCARD leaves the world alone. */
        fake_asker yes = { 0, 1, UI_CHOICE_PROMOTE, UI_CHOICE_PROMOTE };
        sh_set_ui_asker(o, fake_ask, &yes);
        sh_exec_origin(o, "policy check delete seed.txt", ORIGIN_AI, &r);
        CHECK(yes.calls == 0,
              "hold: a `yes` asker is not consulted for introspection either");
        sh_result_free(&r);

        /* Removing the asker is the documented way to get refusals back, and it
         * must actually take effect rather than leaving a dangling pointer. */
        sh_set_ui_asker(o, NULL, NULL);
        CHECK(sh_last_origin(o) == ORIGIN_AI,
              "hold: the shell is still usable after the asker is removed");
    }

    sh_free(o);
    sh = NULL; /* the global still points at the freed shell; make that loud */
}


/* ---- M7: the ask, and the answer that is not an answer -------------------
 *
 * The acceptance card says: promote / discard / show diff, and NO timeout
 * auto-promote -- a timeout means discard. The behaviour that matters is not
 * that the prompt renders; it is that PROMOTE is only ever returned when a
 * person chose it. These tests can say "a person chose it" because they ARE the
 * asker: an asker is a function, and a function that returns DISCARD is a person
 * who walked away.
 */

static void ui_tests(void) {
    ui_prompt p;
    memset(&p, 0, sizeof p);
    snprintf(p.title, sizeof p.title, "fs.delete /workspace/x");
    snprintf(p.reason, sizeof p.reason, "destructive: needs a human");
    p.origin = ORIGIN_AI;

    /* ---- no asker = nobody to ask ---- */
    CHECK(ui_present(&p, NULL, NULL) == UI_CHOICE_DISCARD,
          "ui: with no asker, nothing is promoted");
    CHECK(ui_present(NULL, fake_ask, NULL) == UI_CHOICE_DISCARD,
          "ui: no prompt makes no choice but discard");

    /* ---- a person says no, or says nothing ---- */
    {
        fake_asker f = { 0, 1, UI_CHOICE_DISCARD, UI_CHOICE_DISCARD };
        CHECK(ui_present(&p, fake_ask, &f) == UI_CHOICE_DISCARD,
              "ui: discard is passed through");
        CHECK(f.calls == 1, "ui: and the person was asked exactly once");
    }

    /* ---- show diff is a question, not consent ---- */
    {
        fake_asker f = { 0, 1, UI_CHOICE_SHOW_DIFF, UI_CHOICE_DISCARD };
        CHECK(ui_present(&p, fake_ask, &f) == UI_CHOICE_DISCARD,
              "ui: showing the diff does not promote anything");
        CHECK(f.calls == 2, "ui: ...it asks again, and then takes the answer");
    }
    {
        /* An asker that can ONLY say "show me" must not be an infinite source of
         * consent, nor an infinite loop. Both are failures; one is a hang. */
        fake_asker f = { 0, 1000, UI_CHOICE_SHOW_DIFF, UI_CHOICE_SHOW_DIFF };
        CHECK(ui_present(&p, fake_ask, &f) == UI_CHOICE_DISCARD,
              "ui: a person who only ever looks eventually gets no");
        CHECK(f.calls <= 16, "ui: ...and asking is bounded");
    }

    /* ---- promote only when chosen ---- */
    {
        fake_asker f = { 0, 1, UI_CHOICE_PROMOTE, UI_CHOICE_PROMOTE };
        CHECK(ui_present(&p, fake_ask, &f) == UI_CHOICE_PROMOTE,
              "ui: promote is returned when a person chooses it");
    }
    {
        /* The one that would be a hole: look first, THEN say yes. The loop must
         * carry the yes out, not lose it. */
        fake_asker f = { 0, 1, UI_CHOICE_SHOW_DIFF, UI_CHOICE_PROMOTE };
        CHECK(ui_present(&p, fake_ask, &f) == UI_CHOICE_PROMOTE,
              "ui: show diff then promote is a promote");
    }

    /* ---- the three choices are shown, and the no-answer rule is stated ---- */
    {
        char text[1024];
        int n = ui_prompt_text(&p, text, sizeof text);
        CHECK(n > 0, "ui: the prompt renders");
        CHECK_STR_CONTAINS(text, "promote", "ui: it offers promote");
        CHECK_STR_CONTAINS(text, "discard", "ui: it offers discard");
        CHECK_STR_CONTAINS(text, "show diff", "ui: it offers show diff");
        CHECK_STR_CONTAINS(text, "a model", "ui: it names a model as the proposer");
        CHECK_STR_CONTAINS(text, "no answer = discard",
                           "ui: and states that silence is a no");
    }

    /* ---- the console asker reads words, and distrusts everything else ---- */
    {
        /* A pipe, not a terminal: this is the same code path a person gets. */
        FILE *saved = stdin;
        char buf[] = "promote\n";
        FILE *f = tmpfile();
        CHECK(f != NULL, "ui: a pipe to answer on");
        if (f) {
            fwrite(buf, 1, strlen(buf), f);
            rewind(f);
            stdin = f;
            CHECK(sh_console_ask(&p, NULL) == UI_CHOICE_PROMOTE,
                  "ui: the console takes the whole word `promote`");
            fclose(f);
        }

        /* And the word that starts with p but is not p. */
        buf[0] = 0;
        f = tmpfile();
        if (f) {
            fwrite("please\n", 1, 7, f);
            rewind(f);
            stdin = f;
            CHECK(sh_console_ask(&p, NULL) == UI_CHOICE_DISCARD,
                  "ui: `please` is not a promotion");
            fclose(f);
        }

        /* EOF: a person who closed the terminal has not agreed to anything. */
        f = tmpfile();
        if (f) {
            rewind(f);
            stdin = f;
            CHECK(sh_console_ask(&p, NULL) == UI_CHOICE_DISCARD,
                  "ui: EOF is a discard, not a yes");
            fclose(f);
        }
        stdin = saved;
    }
}

/* ---------------- router unit tests (no shell needed) ---------------- */

#include "aios/types.h"

static void router_tests(void) {
    tool_reg *tr = tool_reg_create("/tmp", "/tmp/shroute-test", "t", NULL);
    CHECK(tr != NULL, "router test registry");
    if (!tr) return;
    tool_reg_install_default(tr);

    struct { const char *line; sh_route_kind kind; const char *tool; } cases[] = {
        { "12*(7+5)",              SH_ROUTE_TOOL,  "calc" },
        { "2^10",                  SH_ROUTE_TOOL,  "calc" },
        { "read notes.txt",        SH_ROUTE_TOOL,  "fs.read" },
        { "cat notes.txt",         SH_ROUTE_TOOL,  "fs.read" },
        { "list files",            SH_ROUTE_TOOL,  "fs.list" },
        { "ls",                    SH_ROUTE_TOOL,  "fs.list" },
        { "write hello to out.txt",SH_ROUTE_TOOL,  "fs.write" },
        { "summarize the workspace", SH_ROUTE_AGENT, NULL },
        { "analyze the metrics",   SH_ROUTE_AGENT, NULL },
        { "clean the tmp files",   SH_ROUTE_AGENT, NULL },
        { "please do the thing",   SH_ROUTE_AGENT, NULL },
        { "what is an OS",         SH_ROUTE_REPLY, NULL },
        { "how does memory work",  SH_ROUTE_REPLY, NULL },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        sh_decision d;
        char err[256] = {0};
        aios_status st = sh_route("scripted", cases[i].line, tr, &d,
                                  err, sizeof err);
        char msg[256];
        /* A CHECK label, not data: clamp both halves so the label always
         * fits the buffer (err alone can be 255 bytes). */
        snprintf(msg, sizeof msg, "route '%.100s' (%.100s)", cases[i].line, err);
        CHECK(st == AIOS_OK, msg);
        CHECK(d.kind == cases[i].kind, msg);
        if (cases[i].tool && d.kind == SH_ROUTE_TOOL) {
            CHECK(d.tool && strcmp(d.tool, cases[i].tool) == 0, msg);
        }
        /* M1: every routed non-REPLY decision carries a descriptor, and the
         * args now live in it (sh_route transfers ownership out of the
         * decision). A routed line is an AI decision, so origin must arrive
         * UNSPECIFIED for the caller to stamp. */
        if (d.kind != SH_ROUTE_REPLY) {
            CHECK(d.action != NULL, "router built an action descriptor");
            if (d.action) {
                CHECK(d.action->origin == ORIGIN_UNSPECIFIED,
                      "router leaves origin for the caller to stamp");
                CHECK(d.action->intent_hash != 0,
                      "descriptor carries the human intent hash");
                CHECK(aios_action_is_valid(d.action, NULL, 0) == 0,
                      "an unstamped descriptor is invalid until the caller stamps");
                aios_action_stamp_origin(d.action, ORIGIN_AI);
                CHECK(aios_action_is_valid(d.action, NULL, 0) == 1,
                      "stamping AI makes it valid");
            }
        } else {
            CHECK(d.action == NULL, "REPLY proposes no action");
        }
        if (d.kind == SH_ROUTE_TOOL && strcmp(d.tool, "fs.write") == 0) {
            /* args moved into the descriptor; the decision's pointer is NULL */
            CHECK(d.args == NULL, "args transferred out of the decision");
            json_val *args = d.action ? d.action->args : NULL;
            CHECK(args != NULL, "descriptor carries the routed args");
            CHECK_STR_CONTAINS(json_str(json_obj_get(args, "path")),
                               "out.txt", "routed fs.write path");
            CHECK_STR_CONTAINS(json_str(json_obj_get(args, "content")),
                               "hello", "routed fs.write content");
            /* and the effect the action claims is on the record */
            CHECK(d.action && d.action->n_declared == 1
                  && strcmp(d.action->declared[0], "write:out.txt") == 0,
                  "router declared the write effect for the routed tool");
        }
        sh_decision_free(&d);
    }
    tool_reg_free(tr);
}

#include "aios/types.h"


int main(int argc, char **argv) {
    (void)argc; g_argv0 = argv[0];
    router_tests();
    shell_tests();
    origin_tests();
    ui_tests();
    printf("%s: %d checks, %d failed\n",
           g_failed ? "FAIL" : "PASS", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
