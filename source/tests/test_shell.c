/* test_shell.c — M5: aiosh engine tests.
 * Covers: parser (quotes, key=value, {json}, pipes, sequences), variable
 * expansion ($var $out $? $json $$), text builtins (count/grep/head),
 * tool stages + pipe injection, the scripted intent router (unit),
 * foreground agent runs, background jobs (spawn/jobs/wait/kill), memory
 * builtins, sembus events, audit integrity, and script end-to-end flow. */
#include "t.h"
#include "aios/shell.h"
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

    /* ---- exit (last: it makes the shell want to terminate) ---- */
    run("exit", &r);
    CHECK(r.status == 0 && strstr(r.text, "bye"), "exit builtin");
    sh_result_free(&r);

    sh_free(sh);
    sh = NULL;
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
    printf("%s: %d checks, %d failed\n",
           g_failed ? "FAIL" : "PASS", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
