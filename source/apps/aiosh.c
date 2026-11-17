/* aiosh — M5/M7: the AI-native shell front-end.
 *
 *   aiosh                          interactive REPL (cortex from
 *                                  $AIOS_SHELL_CORTEX, default scripted)
 *   aiosh --scripted|--micro|--llm cortex override ('micro' = the OS's
 *                                  own micro language model, M7)
 *   aiosh -c "fs.read notes.txt | count"
 *   aiosh script.aiosh             run a script of shell lines
 *   aiosh --jail DIR --store DIR   rebind the workspace jail / OS store
 *
 * The engine lives in libaios (src/shell.c); this binary only wires
 * stdio to sh_exec() and prints results. */
#include "aios/shell.h"
#include "aios/ksched.h"
#include "aios/ebpf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_result(const sh_result *r) {
    if (r->text && *r->text) printf("%s\n", r->text);
    if (r->status != 0 && r->error) printf("error: %s\n", r->error);
}

/* Where the workspace lives when nobody says otherwise.
 *
 * This used to be the relative "./workspace", which is fine at a shell prompt
 * and wrong when PID 1 starts the shell: init never chdir()s, so the jail
 * resolved to /workspace while the image installs it at /workspace -- and the
 * relative path only worked for a human who happened to be in the right
 * directory. A path that depends on someone else's cwd is not a default, it is a
 * coincidence. Absolute first, relative as the fallback for a source-tree run. */
static const char *default_jail(void) {
    if (access("/workspace", F_OK) == 0) return "/workspace";
    if (access("./workspace", F_OK) == 0) return "./workspace";
    return "./workspace";
}

int main(int argc, char **argv) {
    const char *jail = default_jail();
    /* M8 durable: init points AIOS_STORE at the ext2 side disk when it
     * mounted one — memory/audit/jobs then persist across reboots. */
    const char *store = getenv("AIOS_STORE");
    if (!store || !*store) store = ".aios-store";
    const char *mode = NULL; /* default below: $AIOS_SHELL_CORTEX */
    const char *cmd = NULL;
    const char *script = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scripted") == 0) mode = "scripted";
        else if (strcmp(argv[i], "--micro") == 0) mode = "micro";
        else if (strcmp(argv[i], "--llm") == 0) mode = "llm";
        else if (strcmp(argv[i], "--jail") == 0 && i + 1 < argc) jail = argv[++i];
        else if (strcmp(argv[i], "--store") == 0 && i + 1 < argc) store = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) cmd = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: aiosh [--scripted|--micro|--llm] [--jail DIR] "
                   "[--store DIR]\n"
                   "             [-c LINE] | [script.aiosh]\n");
            return 0;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "aiosh: unknown option %s\n", argv[i]);
            return 2;
        }
        else script = argv[i];
    }

    sh_shell *sh = sh_create(jail, store);
    if (!sh) {
        fprintf(stderr, "aiosh: cannot open shell (store=%s jail=%s)\n", store, jail);
        return 1;
    }
    /* M7 LLM-first boot: init exports AIOS_SHELL_CORTEX=micro so the
     * console and the bring-up script route through the OS's own micro
     * language model unless the user overrides it. */
    if (!mode) mode = getenv("AIOS_SHELL_CORTEX");
    if (!mode || !*mode) mode = "scripted";
    sh_set_cortex(sh, mode);

    /* M6/M7 kernel bridge: the shell is the INTERACTIVE class of the OS. */
    aios_ksched_apply(AIOS_KS_INTERACTIVE);
    aios_ksched_register(AIOS_KS_INTERACTIVE);
    aios_ebpf_start();     /* best-effort wakeup observer (M6)     */
    aios_ebpf_ctx_start(); /* best-effort ctx-switch observer (M7) */

    printf("AI-OS %s — aiosh, the AI-native shell\n", AIOS_VERSION);
    printf("cortex=%s jail=%s store=%s durable=%s  ('help' lists commands)\n",
           sh_cortex_mode(sh), jail, store,
           getenv("AIOS_DUR") ? "yes" : "no");

    int last_status = 0;

    if (cmd) {
        sh_result r;
        sh_exec(sh, cmd, &r);
        print_result(&r);
        last_status = r.status;
        sh_result_free(&r);
    } else if (script) {
        FILE *f = strcmp(script, "-") == 0 ? stdin : fopen(script, "r");
        if (!f) {
            fprintf(stderr, "aiosh: cannot open script %s\n", script);
            sh_free(sh);
            return 1;
        }
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            sh_result r;
            if (sh_exec(sh, line, &r) != 0) { sh_result_free(&r); break; }
            print_result(&r);
            last_status = r.status;
            sh_result_free(&r);
        }
        if (f != stdin) fclose(f);
    } else {
        bool tty = isatty(0) != 0;
        char line[4096];
        for (;;) {
            if (tty) printf("aiosh> ");
            fflush(stdout);
            if (!fgets(line, sizeof line, stdin)) break;
            line[strcspn(line, "\r\n")] = 0;
            sh_result r;
            if (sh_exec(sh, line, &r) != 0) {
                print_result(&r);
                sh_result_free(&r);
                break;
            }
            print_result(&r);
            last_status = r.status;
            sh_result_free(&r);
        }
    }

    sh_free(sh);
    return last_status == 0 ? 0 : 1;
}
