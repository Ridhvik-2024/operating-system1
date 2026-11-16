/* ebpf.c — M6/M7: eBPF observers, hand-assembled. See include/aios/ebpf.h.
 *
 * Two tiny programs, same 22-insn counter template (zero external
 * dependencies — no libbpf, no clang):
 *
 *   M6 wakeup observer  sched:sched_wakeup   ctx+24 = pid being woken
 *   M7 ctx observer     sched:sched_switch   ctx+24 = prev_pid (the task
 *                                            being switched OUT — every
 *                                            hit is one descheduling)
 *
 * Both tracepoints open with the 8-byte common trace entry followed by a
 * 16-byte comm array, so the first pid always sits at ctx+24. The
 * scheduler pressure signals (wakeup rate, switch-out rate per pid) feed
 * the M7 autopilot governor.
 *
 *   ctx+24 -> key            (u32 pid)
 *   lookup map; miss -> insert 1; hit -> xadd 1
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aios/ebpf.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/bpf.h>
#include <linux/perf_event.h>

#ifndef PERF_EVENT_IOC_SET_BPF
#define PERF_EVENT_IOC_SET_BPF _IOW('$', 8, __u32)
#endif
#ifndef PERF_EVENT_IOC_ENABLE
#define PERF_EVENT_IOC_ENABLE  _IO('$', 4)
#endif

typedef struct {
    int map_fd, prog_fd, perf_fd;
    char err[1024];
} observer;

static observer g_wakeup = { -1, -1, -1, {0} };   /* M6 */
static observer g_ctx    = { -1, -1, -1, {0} };   /* M7 */

static void obs_err(observer *o, const char *what) {
    snprintf(o->err, sizeof o->err, "%s: %s", what, strerror(errno));
}

/* BPF_ALU64_IMM/REG etc — local opcode helpers.
 * positional init: dst_reg/src_reg are adjacent 4-bit bitfields and some
 * gcc builds reject designated compound literals on them. */
#define INSN(code, dst, src, off, imm) \
    { (code), (dst), (src), (off), (imm) }

/* Load + attach one counter program on the tracepoint at `id_path`.
 * All failure details land in o->err; fds stay closed on failure. */
static int load_counter(observer *o, const char *id_path) {
    o->err[0] = 0;

    /* ---- map: hash<pid u32> -> <counter u64> ---- */
    union bpf_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.map_type    = BPF_MAP_TYPE_HASH;
    attr.key_size    = 4;
    attr.value_size  = 8;
    attr.max_entries = 4096;
    o->map_fd = (int)syscall(__NR_bpf, BPF_MAP_CREATE, &attr, sizeof attr);
    if (o->map_fd < 0) { obs_err(o, "BPF_MAP_CREATE"); return -1; }

    /* ---- program ---- */
    struct bpf_insn prog[] = {
        /*  0: r2 = *(u32 *)(r1 + 24)  -- pid */
        INSN(BPF_LDX | BPF_MEM | BPF_W, 2, 1, 24, 0),
        /*  1: *(u32 *)(r10 - 4) = r2  -- key on stack */
        INSN(BPF_STX | BPF_MEM | BPF_W, 10, 2, -4, 0),
        /*  2: r2 = r10 - 4 (&key) — lookup args: R1=map, R2=key */
        INSN(BPF_ALU64 | BPF_MOV | BPF_X, 2, 10, 0, 0),
        INSN(BPF_ALU64 | BPF_ADD | BPF_K, 2, 0, 0, -4),
        /*  4-5: r1 = map fd (LD_MAP_FD, 2 slots, patched below) */
        INSN(0, 0, 0, 0, 0), INSN(0, 0, 0, 0, 0),
        /*  6: call bpf_map_lookup_elem -> r0 = value | NULL */
        INSN(BPF_JMP | BPF_CALL, 0, 0, 0, 1),
        /*  7: if r0 == 0 goto 11 (init path) */
        INSN(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 3, 0),
        /*  8: r1 = 1 */
        INSN(BPF_ALU64 | BPF_MOV | BPF_K, 1, 0, 0, 1),
        /*  9: lock *(u64 *)(r0 + 0) += r1 */
        INSN(BPF_STX | BPF_XADD | BPF_DW, 0, 1, 0, 0),
        /* 10: goto 20 (exit path) */
        INSN(BPF_JMP | BPF_JA, 0, 0, 9, 0),
        /* 11: *(u64 *)(r10 - 16) = 1 */
        INSN(BPF_ST | BPF_MEM | BPF_DW, 10, 0, -16, 1),
        /* 12: r3 = r10 - 16 (&value) */
        INSN(BPF_ALU64 | BPF_MOV | BPF_X, 3, 10, 0, 0),
        INSN(BPF_ALU64 | BPF_ADD | BPF_K, 3, 0, 0, -16),
        /* 14-15: r1 = map fd (LD_MAP_FD, patched below) */
        INSN(0, 0, 0, 0, 0), INSN(0, 0, 0, 0, 0),
        /* 16: r2 = r10 - 4 (&key) */
        INSN(BPF_ALU64 | BPF_MOV | BPF_X, 2, 10, 0, 0),
        INSN(BPF_ALU64 | BPF_ADD | BPF_K, 2, 0, 0, -4),
        /* 18: r4 = 0 (BPF_ANY) */
        INSN(BPF_ALU64 | BPF_MOV | BPF_K, 4, 0, 0, 0),
        /* 19: call bpf_map_update_elem */
        INSN(BPF_JMP | BPF_CALL, 0, 0, 0, 2),
        /* 20: r0 = 0 */
        INSN(BPF_ALU64 | BPF_MOV | BPF_K, 0, 0, 0, 0),
        /* 21: exit */
        INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0),
    };
    size_t prog_len = sizeof prog / sizeof prog[0];

    /* patch LD_MAP_FD placeholders (each occupies 2 insn slots) */
    struct { size_t idx; int reg; } patches[] = { {4, 1}, {14, 1} };
    for (size_t i = 0; i < sizeof patches / sizeof patches[0]; i++) {
        size_t idx = patches[i].idx;
        prog[idx].code    = BPF_LD | BPF_DW | BPF_IMM;
        prog[idx].src_reg = BPF_PSEUDO_MAP_FD;
        prog[idx].dst_reg = patches[i].reg;
        prog[idx].imm     = o->map_fd & 0xffffffff;
        prog[idx + 1].imm = 0;
    }

    char license[] = "GPL";
    memset(&attr, 0, sizeof attr);
    attr.prog_type  = BPF_PROG_TYPE_TRACEPOINT;
    attr.insns      = (unsigned long)prog;
    attr.insn_cnt   = prog_len;
    attr.license    = (unsigned long)license;
    o->prog_fd = (int)syscall(__NR_bpf, BPF_PROG_LOAD, &attr, sizeof attr);
    if (o->prog_fd < 0) {
        /* retry with a verifier log and surface the tail of it */
        static char log[16384] = {0};
        int rc2_errno = errno;
        memset(&attr, 0, sizeof attr);
        attr.prog_type  = BPF_PROG_TYPE_TRACEPOINT;
        attr.insns      = (unsigned long)prog;
        attr.insn_cnt   = prog_len;
        attr.license    = (unsigned long)license;
        attr.log_level  = 1;
        attr.log_size   = sizeof log;
        attr.log_buf    = (unsigned long)log;
        syscall(__NR_bpf, BPF_PROG_LOAD, &attr, sizeof attr);
        log[sizeof log - 1] = 0;
        char tail[768] = {0};
        if (log[0]) {
            size_t l = strlen(log);
            const char *t = log + (l > 740 ? l - 740 : 0);
            snprintf(tail, sizeof tail, " [%s]", t);
        }
        snprintf(o->err, sizeof o->err, "BPF_PROG_LOAD: %s%s",
                 strerror(rc2_errno), tail);
        return -1;
    }

    /* ---- attach: perf tracepoint ---- */
    char idbuf[32];
    int idf = open(id_path, O_RDONLY | O_CLOEXEC);
    if (idf < 0) { obs_err(o, "tracepoint id (tracefs)"); return -1; }
    ssize_t n = read(idf, idbuf, sizeof idbuf - 1);
    close(idf);
    if (n <= 0) { obs_err(o, "tracepoint id read"); return -1; }
    idbuf[n] = 0;
    unsigned long tp_id = strtoul(idbuf, NULL, 10);

    struct perf_event_attr pe;
    memset(&pe, 0, sizeof pe);
    pe.size = sizeof pe;
    pe.type = PERF_TYPE_TRACEPOINT;
    pe.config = tp_id;
    pe.sample_period = 1;
    o->perf_fd = (int)syscall(__NR_perf_event_open, &pe,
                              -1 /* any cpu */, 0 /* any pid */, -1, 0);
    if (o->perf_fd < 0) { obs_err(o, "perf_event_open"); return -1; }
    if (ioctl(o->perf_fd, PERF_EVENT_IOC_SET_BPF, o->prog_fd) != 0) {
        obs_err(o, "PERF_EVENT_IOC_SET_BPF"); return -1;
    }
    if (ioctl(o->perf_fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        obs_err(o, "PERF_EVENT_IOC_ENABLE"); return -1;
    }
    return 0;
}

static void unload(observer *o) {
    if (o->perf_fd >= 0) { close(o->perf_fd); o->perf_fd = -1; }
    if (o->prog_fd >= 0) { close(o->prog_fd); o->prog_fd = -1; }
    if (o->map_fd  >= 0) { close(o->map_fd);  o->map_fd  = -1; }
}

static int read_counter(observer *o, long pid, uint64_t *out) {
    if (o->map_fd < 0) return -1;
    uint32_t key = (uint32_t)pid;
    uint64_t val = 0;
    union bpf_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.map_fd = (unsigned)o->map_fd;
    attr.key    = (unsigned long)&key;
    attr.value  = (unsigned long)&val;
    if (syscall(__NR_bpf, BPF_MAP_LOOKUP_ELEM, &attr, sizeof attr) != 0) {
        *out = 0; /* pid never hit the tracepoint since attach */
        return 0;
    }
    *out = val;
    return 0;
}

/* ================= M6 wakeup observer (API unchanged) ================= */

int aios_ebpf_start(void) {
    if (aios_ebpf_active()) return 0;
    int rc = load_counter(&g_wakeup,
        "/sys/kernel/tracing/events/sched/sched_wakeup/id");
    if (rc != 0) unload(&g_wakeup);
    return rc;
}

int aios_ebpf_wakeups(long pid, uint64_t *out) {
    return read_counter(&g_wakeup, pid, out);
}

int aios_ebpf_active(void)   { return g_wakeup.perf_fd >= 0; }
const char *aios_ebpf_error(void) { return g_wakeup.err; }

void aios_ebpf_stop(void) { unload(&g_wakeup); }

/* ================= M7 ctx-switch observer ================= */

int aios_ebpf_ctx_start(void) {
    if (aios_ebpf_ctx_active()) return 0;
    int rc = load_counter(&g_ctx,
        "/sys/kernel/tracing/events/sched/sched_switch/id");
    if (rc != 0) unload(&g_ctx);
    return rc;
}

int aios_ebpf_ctx_count(long pid, uint64_t *out) {
    return read_counter(&g_ctx, pid, out);
}

int  aios_ebpf_ctx_active(void)  { return g_ctx.perf_fd >= 0; }
const char *aios_ebpf_ctx_error(void) { return g_ctx.err; }

void aios_ebpf_ctx_stop(void) { unload(&g_ctx); }
