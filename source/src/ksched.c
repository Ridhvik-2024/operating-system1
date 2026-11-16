/* ksched.c — M6 kernel bridge, userspace side. See include/aios/ksched.h.
 *
 * Everything here is best-effort by design: the OS runs (with degraded
 * scheduling fairness) on kernels that lack any of these facilities.
 * Failure reasons are reported, never fatal.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aios/ksched.h"
#include "aios/ebpf.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/sched/types.h>
#include <sched.h>

#ifndef SCHED_OTHER
#define SCHED_OTHER 0
#endif

#ifndef SCHED_BATCH
#define SCHED_BATCH 3
#endif
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_landlock_add_rule
#define __NR_landlock_add_rule 445
#endif
#ifndef __NR_landlock_restrict_self
#define __NR_landlock_restrict_self 446
#endif

/* mirrors boot/module/aios_intent.c */
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

#define CGROUP_ROOT "/sys/fs/cgroup"

const char *aios_ksched_cgroup_name(aios_ksched_class c) {
    switch (c) {
    case AIOS_KS_BATCH:       return "tools";
    case AIOS_KS_IDLE:        return "agents";
    case AIOS_KS_INTERACTIVE:
    default:                  return "system";
    }
}

/* ------------------------------------------------------------------ */
/* sched_setattr                                                        */

int aios_ksched_apply(aios_ksched_class c) {
    struct sched_attr sa;
    memset(&sa, 0, sizeof sa);
    sa.size = sizeof sa;
    switch (c) {
    case AIOS_KS_BATCH:
        sa.sched_policy = SCHED_BATCH;
        sa.sched_nice   = 5;
        break;
    case AIOS_KS_IDLE:
        sa.sched_policy = SCHED_IDLE;
        break;
    case AIOS_KS_INTERACTIVE:
    default:
        sa.sched_policy = SCHED_OTHER;
        sa.sched_nice   = -5;
        break;
    }
    return (int)syscall(__NR_sched_setattr, 0, &sa, 0);
}

/* ------------------------------------------------------------------ */
/* intent device (/dev/aios)                                            */

static int aios_dev_fd = -2; /* -2 = not probed yet, -1 = absent */

static int aios_dev(void) {
    if (aios_dev_fd == -2) {
        aios_dev_fd = open("/dev/aios", O_RDWR | O_CLOEXEC);
        /* fd kept for process lifetime; harmless if module absent */
        if (aios_dev_fd < 0) aios_dev_fd = -1;
    }
    return aios_dev_fd;
}

int aios_ksched_register(aios_ksched_class c) {
    int fd = aios_dev();
    if (fd < 0) return -1;
    struct aios_intent_req r;
    memset(&r, 0, sizeof r);
    r.pid = (int32_t)getpid();
    r.aiclass = (uint8_t)c;
    return ioctl(fd, AIOS_IOC_REGISTER, &r) == 0 ? 0 : -1;
}

int aios_ksched_unregister(void) {
    int fd = aios_dev();
    if (fd < 0) return -1;
    struct aios_intent_req r;
    memset(&r, 0, sizeof r);
    r.pid = (int32_t)getpid();
    return ioctl(fd, AIOS_IOC_UNREGISTER, &r) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* cgroup v2                                                            */

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = strlen(text);
    ssize_t w = write(fd, text, n);
    close(fd);
    return (w == (ssize_t)n) ? 0 : -1;
}

static int read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return 0;
}

int aios_cgroup_boot(void) {
    char path[256];
    /* cgroup v2 mounted? */
    struct stat st;
    if (stat(CGROUP_ROOT "/cgroup.controllers", &st) != 0) return -1;

    /* 1. create the class groups first (files appear once controllers
     * are enabled). M7 fix: pid 1 lives in aios/system (a LEAF) so
     * aios itself stays process-free and can enable the cpu controller
     * for its children — without this, cpu.max/cpu.stat never appear in
     * the class groups and the budgets are fiction. */
    static const struct { const char *name; const char *max; int weight; } g[] = {
        { "tools",  "50000 100000", 50 },
        { "agents", "25000 100000", 25 },
        { "system", "max",          100 },
    };
    snprintf(path, sizeof path, "%s/aios", CGROUP_ROOT);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
    for (size_t i = 0; i < sizeof g / sizeof g[0]; i++) {
        snprintf(path, sizeof path, "%s/aios/%s", CGROUP_ROOT, g[i].name);
        if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
    }

    /* 2. move pid 1 (and every inherited child) into the system class —
     * this empties the ROOT cgroup, which this kernel requires before
     * it accepts subtree_control writes (verified in the VM). */
    snprintf(path, sizeof path, "%s/aios/system/cgroup.procs", CGROUP_ROOT);
    char procs[32];
    snprintf(procs, sizeof procs, "%ld", (long)getpid());
    if (write_file(path, procs) != 0) return -1;

    /* 3. enable the cpu controller: root first, then the aios subtree */
    snprintf(path, sizeof path, "%s/cgroup.subtree_control", CGROUP_ROOT);
    if (write_file(path, "+cpu") != 0) return -1;
    snprintf(path, sizeof path, "%s/aios/cgroup.subtree_control", CGROUP_ROOT);
    if (write_file(path, "+cpu") != 0) return -1;

    /* 4. budgets + weights for the class groups */
    for (size_t i = 0; i < sizeof g / sizeof g[0]; i++) {
        char val[64];
        snprintf(path, sizeof path, "%s/aios/%s/cpu.max", CGROUP_ROOT, g[i].name);
        snprintf(val, sizeof val, "%s", g[i].max);
        write_file(path, val); /* best-effort */
        snprintf(path, sizeof path, "%s/aios/%s/cpu.weight", CGROUP_ROOT, g[i].name);
        snprintf(val, sizeof val, "%d", g[i].weight);
        write_file(path, val);
    }
    return 0;
}

int aios_cgroup_move_pid(long pid, aios_ksched_class c) {
    char path[256], val[32];
    snprintf(path, sizeof path, "%s/aios/%s/cgroup.procs",
             CGROUP_ROOT, aios_ksched_cgroup_name(c));
    snprintf(val, sizeof val, "%ld", pid);
    return write_file(path, val);
}

/* ------------------------------------------------------------------ */
/* M7: budgets, usage, /proc/aios parsing (governor plumbing)           */

int aios_cgroup_budget_set(aios_ksched_class c, int pct) {
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    char path[256], val[64];
    snprintf(path, sizeof path, "%s/aios/%s/cpu.max",
             CGROUP_ROOT, aios_ksched_cgroup_name(c));
    snprintf(val, sizeof val, "%d 100000", pct * 1000);
    return write_file(path, val);
}

int aios_cgroup_budget_get(aios_ksched_class c, int *pct) {
    char path[256], val[64];
    snprintf(path, sizeof path, "%s/aios/%s/cpu.max",
             CGROUP_ROOT, aios_ksched_cgroup_name(c));
    if (read_file(path, val, sizeof val) != 0) return -1;
    if (strncmp(val, "max", 3) == 0) { *pct = 100; return 0; }
    int quota = 0, period = 0;
    if (sscanf(val, "%d %d", &quota, &period) != 2 || period <= 0)
        return -1;
    *pct = (quota * 100 + period / 2) / period; /* rounded */
    if (*pct < 1) *pct = 1;
    if (*pct > 100) *pct = 100;
    return 0;
}

int aios_cgroup_usage_us(aios_ksched_class c, uint64_t *out) {
    char path[256], buf[512];
    snprintf(path, sizeof path, "%s/aios/%s/cpu.stat",
             CGROUP_ROOT, aios_ksched_cgroup_name(c));
    if (read_file(path, buf, sizeof buf) != 0) return -1;
    const char *p = strstr(buf, "usage_usec");
    if (!p) return -1;
    *out = (uint64_t)strtoull(p + strlen("usage_usec"), NULL, 10);
    return 0;
}

static const char *const ks_class_names[3] = { "interactive", "batch", "idle" };

int aios_kernel_wakeups(uint64_t out[3], int *registered) {
    char buf[8192];
    if (read_file("/proc/aios", buf, sizeof buf) != 0) return -1;
    out[0] = out[1] = out[2] = 0;
    if (registered) *registered = -1;
    int found = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '#') continue;
        for (int c = 0; c < 3; c++) {
            size_t nlen = strlen(ks_class_names[c]);
            if (strncmp(line, ks_class_names[c], nlen) == 0 &&
                line[nlen] == '\t') {
                out[c] = (uint64_t)strtoull(line + nlen + 1, NULL, 10);
                found++;
            }
        }
        if (registered && strncmp(line, "registered\t", 11) == 0)
            *registered = (int)strtol(line + 11, NULL, 10);
    }
    return found == 3 ? 0 : -1;
}

int aios_kernel_registry(long *pids, int *cls, int max) {
    char buf[32768];
    if (read_file("/proc/aios", buf, sizeof buf) != 0) return -1;
    int n = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save);
         line && n < max; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '#' || !isdigit((unsigned char)line[0])) continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        int c = -1;
        for (int k = 0; k < 3; k++)
            if (strcmp(tab + 1, ks_class_names[k]) == 0) c = k;
        if (c < 0) continue;
        pids[n] = strtol(line, NULL, 10);
        cls[n] = c;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* status for the `kernel` builtin                                      */

static void append(char **p, size_t *left, const char *fmt, ...) {
    va_list ap;
    if (!*left) return;
    va_start(ap, fmt);
    int n = vsnprintf(*p, *left, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= *left) { *left = 0; return; }
    *p += n; *left -= (size_t)n;
}

int aios_kernel_status(char *out, size_t cap) {
    if (!out || !cap) return -1;
    char *p = out;
    size_t left = cap;

    /* kernel bridge module */
    struct stat st;
    if (stat("/dev/aios", &st) == 0) {
        append(&p, &left, "kernel bridge: /dev/aios present (aios_intent.ko)\n");
        char aios[4096];
        if (read_file("/proc/aios", aios, sizeof aios) == 0) {
            append(&p, &left, "--- /proc/aios ---\n%s", aios);
        }
    } else {
        append(&p, &left, "kernel bridge: /dev/aios absent "
               "(module not loaded — registry/sched accounting off)\n");
    }

    /* cgroup v2 */
    char tmp[128];
    if (read_file(CGROUP_ROOT "/aios/tools/cpu.max", tmp, sizeof tmp) == 0)
        append(&p, &left, "cgroup v2: aios/tools  cpu.max = %s", tmp);
    if (read_file(CGROUP_ROOT "/aios/agents/cpu.max", tmp, sizeof tmp) == 0)
        append(&p, &left, "cgroup v2: aios/agents cpu.max = %s", tmp);

    /* current policy of this process */
    struct sched_attr g;
    memset(&g, 0, sizeof g);
    g.size = sizeof g;
    if (syscall(__NR_sched_getattr, 0, &g, sizeof g, 0) == 0)
        append(&p, &left, "sched policy: this pid policy=%u nice=%d\n",
               g.sched_policy, g.sched_nice);

    /* eBPF wakeup observer */
    if (aios_ebpf_active()) {
        uint64_t w = 0;
        if (aios_ebpf_wakeups((long)getpid(), &w) == 0)
            append(&p, &left, "eBPF: sched_wakeup observer active; "
                   "this pid wakeups=%llu\n", (unsigned long long)w);
    } else {
        const char *why = aios_ebpf_error();
        append(&p, &left, "eBPF: observer inactive%s%s\n",
               why && *why ? ": " : "", why ? why : "");
    }
    /* eBPF ctx-switch observer (M7) */
    if (aios_ebpf_ctx_active()) {
        uint64_t c = 0;
        if (aios_ebpf_ctx_count((long)getpid(), &c) == 0)
            append(&p, &left, "eBPF: sched_switch observer active; "
                   "this pid ctx-switches=%llu\n", (unsigned long long)c);
    } else {
        const char *why = aios_ebpf_ctx_error();
        append(&p, &left, "eBPF: sched_switch observer inactive%s%s\n",
               why && *why ? ": " : "", why ? why : "");
    }

    /* M7 autopilot governor (state file written by aios-governor) */
    char ap[2048];
    if (read_file("/var/lib/aios/autopilot-state.json", ap, sizeof ap) == 0) {
        const char *mode = strstr(ap, "\"mode\"");
        if (mode) {
            char mname[32] = "?";
            const char *q1 = strchr(mode + 6, '"');
            if (q1) {
                const char *q2 = strchr(q1 + 1, '"');
                if (q2 && (size_t)(q2 - q1 - 1) < sizeof mname) {
                    memcpy(mname, q1 + 1, (size_t)(q2 - q1 - 1));
                    mname[q2 - q1 - 1] = 0;
                }
            }
            append(&p, &left, "autopilot governor: active (mode %s)\n", mname);
        } else {
            append(&p, &left, "autopilot governor: state file present\n");
        }
    } else {
        append(&p, &left, "autopilot governor: not running "
               "(state file /var/lib/aios/autopilot-state.json absent)\n");
    }
    return 0;
}
