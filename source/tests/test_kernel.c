/* test_kernel.c — M6 kernel-bridge tests: intent device, scheduler
 * policies, cgroup v2 budgets, seccomp/Landlock hardening, eBPF observer.
 *
 * Kernel facilities that a given host may lack degrade to SKIP, not FAIL:
 * this suite must pass inside the AI-OS image (full support) and on
 * ordinary dev hosts (partial support).
 */
#include "t.h"
#include "aios/ksched.h"
#include "aios/ebpf.h"
#include "aios/hardening.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/sched/types.h>
#include <sys/syscall.h>

/* mirrors boot/module/aios_intent.c ioctl interface */
#define AIOS_IOC_MAGIC 'A'
#define AIOS_IOC_REGISTER   _IOW(AIOS_IOC_MAGIC, 1, struct aios_intent_req)
#define AIOS_IOC_UNREGISTER _IOW(AIOS_IOC_MAGIC, 2, struct aios_intent_req)
#define AIOS_IOC_QUERY      _IOWR(AIOS_IOC_MAGIC, 3, struct aios_intent_req)

struct aios_intent_req {
    int32_t pid;
    uint8_t aiclass;
    uint8_t pad[3];
    uint64_t wakeups;
};

static int have_dev_aios(void) {
    struct stat st;
    return stat("/dev/aios", &st) == 0;
}

static int have_cgroup2_writable(void) {
    return access("/sys/fs/cgroup/cgroup.procs", W_OK) == 0;
}

static int have_landlock(void) {
    long abi = syscall(__NR_landlock_create_ruleset, NULL, 0, 1 /* VERSION */);
    return abi >= 0;
}

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#include <sys/syscall.h>

static void run(void) {
    /* ---- 1. intent device registry ---- */
    if (!have_dev_aios()) {
        SKIP("/dev/aios absent (module not loaded)");
    } else {
        int fd = open("/dev/aios", O_RDWR);
        CHECK(fd >= 0, "open /dev/aios");
        struct aios_intent_req r;
        memset(&r, 0, sizeof r);
        r.pid = (int32_t)getpid();
        r.aiclass = 2 /* IDLE */;
        CHECK(ioctl(fd, AIOS_IOC_REGISTER, &r) == 0, "register self");
        memset(&r, 0, sizeof r);
        r.pid = (int32_t)getpid();
        CHECK(ioctl(fd, AIOS_IOC_QUERY, &r) == 0, "query self");
        CHECK(r.aiclass == 2, "query returns registered class");
        /* register a bogus pid -> ESRCH */
        memset(&r, 0, sizeof r);
        r.pid = 3000000;
        CHECK(ioctl(fd, AIOS_IOC_REGISTER, &r) != 0, "bogus pid rejected");
        memset(&r, 0, sizeof r);
        r.pid = (int32_t)getpid();
        CHECK(ioctl(fd, AIOS_IOC_UNREGISTER, &r) == 0, "unregister self");
        close(fd);
    }

    /* ---- 2. sched_setattr policies (verified in forked children:
     *         a non-root process cannot raise priority back, so each
     *         policy is applied in its own process) ---- */
    {
        static const struct { aios_ksched_class c; unsigned pol; const char *name; }
            cases[] = {
                { AIOS_KS_IDLE,        5, "SCHED_IDLE" },
                { AIOS_KS_BATCH,       3, "SCHED_BATCH" },
                { AIOS_KS_INTERACTIVE, 0, "SCHED_OTHER" },
            };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            pid_t p = fork();
            if (p == 0) {
                if (aios_ksched_apply(cases[i].c) != 0)
                    _exit(errno == EPERM ? 5 : 2);
                struct sched_attr g;
                memset(&g, 0, sizeof g);
                g.size = sizeof g;
                if (syscall(__NR_sched_getattr, 0, &g, sizeof g, 0) != 0)
                    _exit(3);
                _exit(g.sched_policy == cases[i].pol ? 0 : 4);
            }
            int st = 0;
            waitpid(p, &st, 0);
            if (WIFEXITED(st) && WEXITSTATUS(st) == 5)
                SKIP("%s needs CAP_SYS_NICE (non-root host)", cases[i].name);
            else
                CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                      cases[i].name);
        }
    }

    /* ---- 3. cgroup v2 class groups ---- */
    if (!have_cgroup2_writable()) {
        SKIP("cgroup v2 not writable (host restriction)");
    } else if (aios_cgroup_boot() != 0) {
        SKIP("cgroup boot failed (root has procs / no cpu ctl)");
    } else {
        char path[128];
        snprintf(path, sizeof path, "/sys/fs/cgroup/aios/tools");
        struct stat st;
        CHECK(stat(path, &st) == 0, "aios/tools exists");
        snprintf(path, sizeof path, "/sys/fs/cgroup/aios/agents");
        CHECK(stat(path, &st) == 0, "aios/agents exists");
        /* move a child into tools */
        pid_t p = fork();
        if (p == 0) { for (;;) pause(); }
        CHECK(aios_cgroup_move_pid((long)p, AIOS_KS_BATCH) == 0, "move child to tools");
        { struct timespec ts={0,200*1000*1000}; nanosleep(&ts,NULL); }
        char buf[256] = {0};
        snprintf(path, sizeof path, "/sys/fs/cgroup/aios/tools/cgroup.procs");
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0, "open tools procs");
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof buf - 1);
            close(fd);
            char expect[32];
            snprintf(expect, sizeof expect, "%d", (int)p);
            CHECK(n > 0 && strstr(buf, expect) != NULL,
                  "child listed in tools group");
        }
        kill(p, SIGKILL);
        waitpid(p, NULL, 0);
    }

    /* ---- 4. seccomp hardening: blocked syscall kills, allowed works ---- */
    {
        /* blocked: socket() must die by SIGSYS under the allowlist */
        pid_t p = fork();
        if (p == 0) {
            char why[128];
            int bits = aios_hardening_apply(NULL, why, sizeof why);
            if (!(bits & AIOS_HARD_SECCOMP)) _exit(42); /* unavailable */
            int s = socket(AF_INET, SOCK_STREAM, 0);
            (void)s;
            _exit(0); /* if we get here, filter failed */
        }
        int st = 0;
        waitpid(p, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 42) {
            SKIP("seccomp filter not supported here");
        } else {
            CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS,
                  "socket() killed by seccomp (SIGSYS)");
        }
        /* allowed: normal file IO keeps working under the same filter */
        p = fork();
        if (p == 0) {
            char why[128];
            aios_hardening_apply(NULL, why, sizeof why);
            FILE *f = fopen("build/seccomp-ok.txt", "w");
            if (!f) _exit(1);
            fputs("hello", f);
            fclose(f);
            f = fopen("build/seccomp-ok.txt", "r");
            char b[8] = {0};
            size_t n = f ? fread(b, 1, 5, f) : 0;
            if (f) fclose(f);
            _exit(n == 5 && strcmp(b, "hello") == 0 ? 0 : 2);
        }
        waitpid(p, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 42) {
            SKIP("seccomp filter not supported here (2nd)");
        } else {
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                  "file IO still works under seccomp");
        }
    }

    /* ---- 5. Landlock: jail enforced ---- */
    if (!have_landlock()) {
        SKIP("landlock unavailable (ABI probe failed)");
    } else {
        (void)!system("mkdir -p build/ll-jail");
        FILE *f = fopen("build/ll-jail/inside.txt", "w");
        if (f) { fputs("in", f); fclose(f); }
        /* The outside path must EXIST and be outside the jail, or the kernel
         * answers ENOENT before any LSM gets a say - which is not a denial.
         * /etc/passwd exists on every Linux host and in the image; the old
         * code used /sbin/aiosh, which only exists inside the image, so this
         * check failed on every host for the wrong reason. */
        if (access("/etc/passwd", R_OK) != 0) {
            SKIP("landlock: no outside file to probe (/etc/passwd missing)");
        } else {
        pid_t p = fork();
        if (p == 0) {
            if (chdir("build") != 0) _exit(3);
            char why[128];
            int bits = aios_hardening_apply("ll-jail", why, sizeof why);
            if (!(bits & AIOS_HARD_LANDLOCK)) _exit(42); /* unavailable */
            int fd = open("ll-jail/inside.txt", O_RDONLY);
            int ok_in = (fd >= 0);
            int in_err = errno;
            if (fd >= 0) close(fd);
            int fd2 = open("/etc/passwd", O_RDONLY); /* outside: must fail */
            int out_err = errno;
            int blocked_out = (fd2 < 0 && (errno == EACCES || errno == EPERM));
            if (fd2 >= 0) close(fd2);
            if (!ok_in) { fprintf(stderr, "ll: inside open err=%s\n", strerror(in_err)); _exit(7); }
            if (!blocked_out) { fprintf(stderr, "ll: outside open err=%d (%s)\n", out_err, strerror(out_err)); _exit(6); }
            _exit(0);
        }
        int st = 0;
        waitpid(p, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 42) {
            SKIP("landlock restrict failed (check ABI)");
        } else {
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                  "landlock: jail readable, /etc/passwd blocked");
        }
        }
    }

    /* ---- 6. eBPF wakeup observer ---- */
    if (aios_ebpf_start() != 0) {
        SKIP("ebpf observer: %s", aios_ebpf_error());
    } else {
        CHECK(aios_ebpf_active(), "ebpf attached");
        struct timespec ts = {0, 120 * 1000 * 1000};
        nanosleep(&ts, NULL); /* wake ourselves */
        uint64_t w = 0;
        CHECK(aios_ebpf_wakeups((long)getpid(), &w) == 0, "ebpf lookup");
        CHECK(w >= 1, "wakeup counter grew (>=1)");
        aios_ebpf_stop();
    }
}

#include "t.h"

TEST_MAIN()
