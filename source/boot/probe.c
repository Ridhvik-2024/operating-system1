/* probe.c — M6 recon: probe the booted kernel for the features the
 * kernel-bridge milestone needs. Static, zero deps. Staged as /sbin/probe.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* syscall(2) declaration */
#endif
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sched.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/landlock.h>
#include <linux/sched.h>
#include <linux/bpf.h>

/* Same glibc >= 2.36 clash as src/ksched.c: <linux/sched/types.h> and
 * glibc's <sched.h> both define struct sched_param. Declare only the one
 * kernel struct this dev probe actually needs. */
struct sched_attr {
    unsigned int size;
    unsigned int sched_policy;
    long long    sched_flags;
    int          sched_nice;
    unsigned int sched_priority;
    unsigned long long sched_runtime;
    unsigned long long sched_deadline;
    unsigned long long sched_period;
};

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif

#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
#ifndef SCHED_OTHER
#define SCHED_OTHER 0
#endif

int main(void) {
    printf("== AI-OS kernel feature probe ==\n");

    /* 1. Landlock ABI */
    long abi = syscall(__NR_landlock_create_ruleset, NULL, 0,
                       LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0)
        printf("landlock: unavailable: %s\n", strerror(errno));
    else
        printf("landlock: available, ABI v%ld\n", abi);

    /* 2. eBPF syscall: load a trivial socket filter prog */
    struct bpf_insn prog[] = {
        { .code = BPF_ALU64 | BPF_MOV | BPF_K, .dst_reg = 0, .src_reg = 0,
          .off = 0, .imm = 0 },          /* r0 = 0 */
        { .code = BPF_JMP | BPF_EXIT, .dst_reg = 0, .src_reg = 0,
          .off = 0, .imm = 0 },          /* exit */
    };
    char license[] = "AI-OS";
    union bpf_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.prog_type = BPF_PROG_TYPE_SOCKET_FILTER;
    attr.insns = (unsigned long)prog;
    attr.insn_cnt = 2;
    attr.license = (unsigned long)license;
    int bfd = syscall(__NR_bpf, BPF_PROG_LOAD, &attr, sizeof attr);
    if (bfd >= 0)
        printf("ebpf syscall: available (map/prog syscall ok, fd=%d)\n", bfd);
    else
        printf("ebpf syscall: FAILED: %s\n", strerror(errno));
    /* create the hash map first (the wakeup program dereferences it) */
    int mfd = -1;
    {
        union bpf_attr ma;
        memset(&ma, 0, sizeof ma);
        ma.map_type    = BPF_MAP_TYPE_HASH;
        ma.key_size    = 4;
        ma.value_size  = 8;
        ma.max_entries = 4096;
        mfd = (int)syscall(__NR_bpf, BPF_MAP_CREATE, &ma, sizeof ma);
        printf("ebpf hash map create: %s (fd=%d)\n",
               mfd >= 0 ? "OK" : strerror(errno), mfd);
    }

    /* 1b. replicate the ebpf.c wakeup observer program and dump the full
     * verifier log for diagnosis */
    {
        struct bpf_insn wp[] = {
            { BPF_LDX | BPF_MEM | BPF_W, 2, 1, 24, 0 },
            { BPF_STX | BPF_MEM | BPF_W, 10, 2, -4, 0 },
            { BPF_ALU64 | BPF_MOV | BPF_X, 2, 10, 0, 0 },
            { BPF_ALU64 | BPF_ADD | BPF_K, 2, 0, 0, -4 },
            { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 },
            { BPF_JMP | BPF_CALL, 0, 0, 0, 1 },
            { BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 3, 0 },
            { BPF_ALU64 | BPF_MOV | BPF_K, 1, 0, 0, 1 },
            { BPF_STX | BPF_XADD | BPF_DW, 0, 1, 0, 0 },
            { BPF_JMP | BPF_JA, 0, 0, 9, 0 },
            { BPF_ST | BPF_MEM | BPF_DW, 10, 0, -16, 1 },
            { BPF_ALU64 | BPF_MOV | BPF_X, 3, 10, 0, 0 },
            { BPF_ALU64 | BPF_ADD | BPF_K, 3, 0, 0, -16 },
            { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 },
            { BPF_ALU64 | BPF_MOV | BPF_X, 2, 10, 0, 0 },
            { BPF_ALU64 | BPF_ADD | BPF_K, 2, 0, 0, -4 },
            { BPF_ALU64 | BPF_MOV | BPF_K, 4, 0, 0, 0 },
            { BPF_JMP | BPF_CALL, 0, 0, 0, 2 },
            { BPF_ALU64 | BPF_MOV | BPF_K, 0, 0, 0, 0 },
            { BPF_JMP | BPF_EXIT, 0, 0, 0, 0 },
        };
        wp[4].code = BPF_LD | BPF_DW | BPF_IMM; wp[4].src_reg = BPF_PSEUDO_MAP_FD;
        wp[4].dst_reg = 1; wp[4].imm = mfd; /* map fd */
        wp[14].code = BPF_LD | BPF_DW | BPF_IMM; wp[14].src_reg = BPF_PSEUDO_MAP_FD;
        wp[14].dst_reg = 1; wp[14].imm = mfd;

        char lic[] = "GPL";
        static char vlog[32768];
        memset(&attr, 0, sizeof attr);
        attr.prog_type = BPF_PROG_TYPE_TRACEPOINT;
        attr.insns = (unsigned long)wp;
        attr.insn_cnt = sizeof wp / sizeof wp[0];
        attr.license = (unsigned long)lic;
        attr.log_level = 2;
        attr.log_size = sizeof vlog;
        attr.log_buf = (unsigned long)vlog;
        int pfd = (int)syscall(__NR_bpf, BPF_PROG_LOAD, &attr, sizeof attr);
        vlog[sizeof vlog - 1] = 0;
        printf("ebpf wakeup-prog load: %s\n", pfd >= 0 ? "OK" : strerror(errno));
        printf("verifier log:\n%s\n--- end (truncated_to_%d) ---\n",
               vlog, (int)sizeof vlog);
        if (pfd >= 0) close(pfd);
    }


    /* 3. seccomp filter mode */
    int sc = syscall(__NR_seccomp, SECCOMP_GET_ACTION_AVAIL, 0,
                     (unsigned long[]) { SECCOMP_RET_KILL_PROCESS });
    printf("seccomp: %s\n", sc == 0 ? "available" : strerror(errno));

    /* 4. sched_setattr SCHED_IDLE roundtrip */
    struct sched_attr sa = { .size = sizeof sa, .sched_policy = SCHED_IDLE };
    if (syscall(__NR_sched_setattr, 0, &sa, 0) == 0) {
        struct sched_attr g = { .size = sizeof g };
        syscall(__NR_sched_getattr, 0, &g, sizeof g, 0);
        printf("sched_setattr: available (set/get SCHED_IDLE ok, now policy=%u)\n",
               g.sched_policy);
        struct sched_attr back = { .size = sizeof back,
                                   .sched_policy = SCHED_OTHER };
        syscall(__NR_sched_setattr, 0, &back, 0);
    } else {
        printf("sched_setattr: FAILED: %s\n", strerror(errno));
    }

    return 0;
}
