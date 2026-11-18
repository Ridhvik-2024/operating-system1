/* hardening.c — M6: seccomp allowlist + Landlock jail MAC. See aios/hardening.h.
 *
 * No external libraries: cBPF assembled inline for seccomp; raw
 * landlock_create_ruleset/add_rule/restrict_self syscalls for Landlock.
 */
#include "aios/hardening.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <linux/landlock.h>
#include <linux/sched.h>

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_landlock_add_rule
#define __NR_landlock_add_rule 445
#endif
#ifndef __NR_landlock_restrict_self
#define __NR_landlock_restrict_self 446
#endif

#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE 0 /* ABI v2+, absent on older headers */
#endif

static void whyf(char *why, size_t cap, const char *fmt, ...) {
    if (!why || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* seccomp: syscall allowlist (cBPF)                                    */

static int seccomp_install(char *why, size_t cap) {
    /* syscalls the tool worker legitimately needs: fd-based IO, memory,
     * time, basic info. Everything else (execve, fork, socket, mount,
     * ptrace, bpf, kexec...) is rejected with SIGSYS. */
    static const int allow[] = {
        __NR_read, __NR_write, __NR_close,
        __NR_open, __NR_openat, __NR_stat, __NR_lstat, __NR_fstat, __NR_newfstatat,
        __NR_statx, __NR_lseek, __NR_readlinkat, __NR_getdents64,
        __NR_mmap, __NR_munmap, __NR_mprotect, __NR_brk,
        __NR_rt_sigaction, __NR_rt_sigprocmask, __NR_rt_sigreturn,
        __NR_ioctl, __NR_futex, __NR_set_tid_address, __NR_set_robust_list,
        __NR_exit, __NR_exit_group,
        __NR_clock_gettime, __NR_clock_nanosleep, __NR_nanosleep,
        __NR_getpid, __NR_getppid, __NR_gettid,
        __NR_getuid, __NR_geteuid, __NR_getgid, __NR_getegid,
        __NR_getrandom, __NR_uname, __NR_sysinfo, __NR_prlimit64,
        __NR_sched_yield, __NR_sched_getaffinity,
        __NR_mkdir, __NR_rmdir, __NR_unlink, __NR_rename, __NR_ftruncate,
        __NR_dup, __NR_dup2, __NR_dup3, __NR_fcntl,
        /* hardening itself may still need these after the filter is on */
        __NR_seccomp,
        __NR_landlock_create_ruleset, __NR_landlock_add_rule,
        __NR_landlock_restrict_self,
    };
    size_t nallow = sizeof allow / sizeof allow[0];

    struct sock_filter filt[64 + nallow];
    size_t n = 0;

    /* [0] load arch */
    filt[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                    offsetof(struct seccomp_data, arch));
    /* [1] check arch == AUDIT_ARCH_X86_64 */
    filt[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                    AUDIT_ARCH_X86_64, 0, 4 /* -> kill */);
    /* [2] load nr */
    filt[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                    offsetof(struct seccomp_data, nr));
    /* allowlist */
    for (size_t i = 0; i < nallow; i++) {
        filt[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                        (uint32_t)allow[i], 0, 0); /* jt patched below */
    }
    /* kill: SECCOMP_RET_KILL_PROCESS */
    filt[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                    SECCOMP_RET_KILL_PROCESS);
    /* allow: SECCOMP_RET_ALLOW */
    filt[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                    SECCOMP_RET_ALLOW);

    /* patch each allowlist JEQ to jump to the ALLOW ret (last insn).
     * BPF jump semantics: target = insn_index + 1 + jt.
     * allow index = n-1; from index (3+i): jt = (n-1) - (3+i) - 1. */
    for (size_t i = 0; i < nallow; i++) {
        struct sock_filter *j = &filt[3 + i];
        j->jt = (uint8_t)((n - 1) - (3 + i) - 1);
        j->jf = 0;                          /* next compare */
    }
    /* arch mismatch: at insn[1], jt=0 (continue), jf=4 -> skip to kill
     * (insns 2,3..; kill sits at n-2) — recompute: from 1, jf target = n-2
     * => jf = (n-2) - 1 - 1 = n - 4 */
    filt[1].jf = (uint8_t)(n - 4);

    struct sock_fprog fprog = {
        .len = (unsigned short)n,
        .filter = filt,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        whyf(why, cap, "PR_SET_NO_NEW_PRIVS: %s", strerror(errno));
        return -1;
    }
    if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog) != 0) {
        whyf(why, cap, "seccomp(SECCOMP_SET_MODE_FILTER): %s",
             strerror(errno));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Landlock: filesystem view limited to the jail                        */

/* One rule: "everything beneath `path` may be read and written". Returns 0 on
 * success, -1 with *why filled. */
static int landlock_allow(const char *path, int ruleset_fd, __u64 handled,
                          long abi, char *why, size_t cap) {
    int jfd = open(path, O_PATH | O_CLOEXEC | O_DIRECTORY);
    if (jfd < 0) {
        whyf(why, cap, "open %s: %s", path, strerror(errno));
        return -1;
    }
    struct landlock_path_beneath_attr pb = {
        .allowed_access = handled,
        .parent_fd = (unsigned)jfd,
    };
    int rc = (int)syscall(__NR_landlock_add_rule, ruleset_fd,
                          LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    close(jfd);
    if (rc != 0) {
        whyf(why, cap, "landlock_add_rule %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int landlock_apply(const char *jail, const char *extra_rw,
                          char *why, size_t cap) {
    long abi = syscall(__NR_landlock_create_ruleset, NULL, 0,
                       LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) {
        whyf(why, cap, "landlock unavailable: %s", strerror(errno));
        return -1;
    }
    /* What the tools actually do: read, write, create (O_CREAT on a new file is
     * MAKE_REG, not WRITE_FILE), and remove. WRITE_FILE alone is enough to
     * overwrite an existing file and NOT enough to create one, which is exactly
     * the difference between a staged write landing and returning EACCES with no
     * output at all -- the failure mode that made this look like the sandbox
     * refusing to run rather than refusing one access bit.
     *
     * MAKE_DIR is here for the same reason: fs.write creates missing parents. */
    __u64 handled = LANDLOCK_ACCESS_FS_READ_FILE |
                    LANDLOCK_ACCESS_FS_WRITE_FILE |
                    LANDLOCK_ACCESS_FS_READ_DIR |
                    LANDLOCK_ACCESS_FS_MAKE_REG |
                    LANDLOCK_ACCESS_FS_MAKE_DIR;
    if (abi >= 2) handled |= LANDLOCK_ACCESS_FS_REMOVE_FILE |
                             LANDLOCK_ACCESS_FS_REMOVE_DIR;
    if (abi >= 3) handled |= LANDLOCK_ACCESS_FS_TRUNCATE;

    struct landlock_ruleset_attr rs = { .handled_access_fs = handled };
    int ruleset_fd = (int)syscall(__NR_landlock_create_ruleset, &rs,
                                  sizeof rs, 0);
    if (ruleset_fd < 0) {
        whyf(why, cap, "landlock_create_ruleset: %s", strerror(errno));
        return -1;
    }

    /* Allow everything within the jail. */
    if (landlock_allow(jail, ruleset_fd, handled, abi, why, cap) != 0) {
        close(ruleset_fd);
        return -1;
    }
    /* And the staging directory, when there is one. Not "the store": the
     * staging subtree the parent named, which is the only outside place a
     * machine's write is supposed to reach. */
    if (extra_rw && *extra_rw) {
        if (landlock_allow(extra_rw, ruleset_fd, handled, abi, why, cap) != 0) {
            /* Not fatal, and deliberately so. The jail rule is the one that must
             * hold; the staging rule is an addition, and failing to add it must
             * not turn "sandbox is up" into "sandbox is off". What it must do is
             * SAY so, because the symptom otherwise is a machine write that
             * silently goes nowhere. */
            fprintf(stderr, "tool-exec: landlock staging rule not added (%s) -- "
                            "a machine write will be refused\n", why);
            why[0] = 0;
        }
    }

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        whyf(why, cap, "PR_SET_NO_NEW_PRIVS: %s", strerror(errno));
        close(ruleset_fd);
        return -1;
    }
    if (syscall(__NR_landlock_restrict_self, ruleset_fd, 0) != 0) {
        whyf(why, cap, "landlock_restrict_self: %s", strerror(errno));
        close(ruleset_fd);
        return -1;
    }
    close(ruleset_fd);
    return 0;
}

/* ------------------------------------------------------------------ */

int aios_hardening_apply(const char *jail, const char *extra_rw,
                         char *why, size_t whycap) {
    int applied = 0;
    char w1[128] = {0}, w2[128] = {0};

    /* Landlock FIRST (it still needs its own syscalls + open), then the
     * seccomp allowlist last — from that point on, nothing outside the
     * list can be attempted at all. */
    if (jail && landlock_apply(jail, extra_rw, w2, sizeof w2) == 0)
        applied |= AIOS_HARD_LANDLOCK;
    if (seccomp_install(w1, sizeof w1) == 0) applied |= AIOS_HARD_SECCOMP;

    if (why && whycap)
        snprintf(why, whycap, "seccomp=%s%s landlock=%s%s",
                 (applied & AIOS_HARD_SECCOMP) ? "on" : "off",
                 w1[0] ? "" : "",
                 (applied & AIOS_HARD_LANDLOCK) ? "on" : "off",
                 w2[0] ? "" : "");
    return applied;
}
