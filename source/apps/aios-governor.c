/* aios-governor — M7: the autopilot daemon of AI-OS.
 *
 * Closed loop, once per tick (default 2s):
 *
 *   gather   /proc/aios per-class wakeup totals   (M6 kernel bridge)
 *            eBPF sched_switch counts per class   (M7 ctx observer)
 *            cgroup v2 cpu.stat usage per class   (kernel accounting)
 *   decide   aios_ap_tick() — the pure, unit-tested policy
 *   act      write new cpu.max budgets for tools/agents (cgroup v2)
 *   report   state JSON (read by the `autopilot` builtin) +
 *            JSONL journal of every adjustment (/var/lib/aios)
 *
 * The `tune` builtin writes the conf file; the governor re-reads it each
 * tick, so mode/budget changes land without a restart. Best-effort
 * everywhere: on a host without the kernel bridge it still runs and
 * reports "hands off" decisions.
 *
 *   aios-governor [--mode balanced|aggressive|static|off]
 *                 [--interval-ms N] [--conf PATH] [--state PATH]
 *                 [--journal PATH]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aios/autopilot.h"
#include "aios/ksched.h"
#include "aios/ebpf.h"
#include "aios/types.h"
#include "aios/json.h"
#include "aios/durable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#define AP_REGISTRY_MAX 512

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static double wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

int main(int argc, char **argv) {
    aios_ap_conf conf;
    aios_ap_conf_defaults(&conf);
    const char *conf_path    = AIOS_AP_PATH_CONF;
    const char *state_path   = AIOS_AP_PATH_STATE;
    const char *journal_path = AIOS_AP_PATH_JOURNAL;
    int interval_ms = 0; /* 0 = from conf */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            int m = aios_ap_mode_parse(argv[++i]);
            if (m < 0) {
                fprintf(stderr, "aios-governor: unknown mode %s\n", argv[i]);
                return 2;
            }
            conf.mode = (aios_ap_mode)m;
        }
        else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc)
            interval_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--conf") && i + 1 < argc)
            conf_path = argv[++i];
        else if (!strcmp(argv[i], "--state") && i + 1 < argc)
            state_path = argv[++i];
        else if (!strcmp(argv[i], "--journal") && i + 1 < argc)
            journal_path = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: aios-governor [--mode M] [--interval-ms N] "
                   "[--conf P] [--state P] [--journal P]\n");
            return 0;
        } else {
            fprintf(stderr, "aios-governor: unknown arg %s\n", argv[i]);
            return 2;
        }
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, SIG_IGN);

    /* the governor is bounded system work: batch class, kernel registry */
    aios_ksched_apply(AIOS_KS_BATCH);
    aios_ksched_register(AIOS_KS_BATCH);

    /* both scheduler-pressure observers, best-effort */
    int eb_w = aios_ebpf_start() == 0;
    int eb_c = aios_ebpf_ctx_start() == 0;
    fprintf(stderr,
            "[governor] autopilot pid %d mode=%s observers: wakeup=%s "
            "ctx=%s\n", getpid(), aios_ap_mode_name(conf.mode),
            eb_w ? "on" : "off", eb_c ? "on" : "off");

    /* M8 durable: if the previous session left an autopilot journal on the
     * ext2 side disk, continue from it (init already restored the conf +
     * state files into /var/lib/aios before we started). */
    if (aios_dur_available()) {
        size_t prev = 0;
        char last[256] = {0};
        aios_dur_lines("var/autopilot.jsonl", &prev, last, sizeof last);
        if (prev > 0)
            fprintf(stderr,
                    "[governor] durable: %zu journal entries replayed from "
                    "the previous session (last: %.120s)\n", prev, last);
    }

    uint64_t prev_wake[3] = {0}, prev_use[3] = {0}, prev_ctx[3] = {0};
    int have_prev = 0;
    aios_ap_state st = {0};
    uint64_t tick = 0;
    double last_ms = mono_ms();
    int rc_last = 0;

    while (!g_stop) {
        /* ---- config (the `tune` builtin may have rewritten it) ---- */
        int have_conf_file = aios_ap_conf_load(conf_path, &conf) == 0;
        int sleep_ms = interval_ms > 0 ? interval_ms : conf.tick_ms;

        /* ---- gather ---- */
        uint64_t wake[3] = {0}, use[3] = {0}, ctx[3] = {0};
        int registered = -1;
        aios_kernel_wakeups(wake, &registered);
        for (int c = 0; c < 3; c++) aios_cgroup_usage_us((aios_ksched_class)c, &use[c]);
        if (eb_c) {
            long pids[AP_REGISTRY_MAX];
            int cls[AP_REGISTRY_MAX];
            int n = aios_kernel_registry(pids, cls, AP_REGISTRY_MAX);
            for (int i = 0; i < n; i++) {
                uint64_t v = 0;
                if (aios_ebpf_ctx_count(pids[i], &v) == 0)
                    ctx[cls[i]] += v;
            }
        }

        double now = mono_ms();
        double dts = have_prev ? (now - last_ms) / 1000.0 : 0.0;

        aios_ap_snapshot snap;
        memset(&snap, 0, sizeof snap);
        snap.mode = conf.mode;
        snap.tools_base = conf.tools_base;
        snap.agents_base = conf.agents_base;
        snap.tools_floor = conf.tools_floor;
        snap.agents_floor = conf.agents_floor;
        snap.dts = dts;
        snap.tick = ++tick;
        int bt = 0, ba = 0;
        if (aios_cgroup_budget_get(AIOS_KS_BATCH, &bt) == 0)
            snap.tools_pct = bt;
        else
            snap.tools_pct = conf.tools_base;
        if (aios_cgroup_budget_get(AIOS_KS_IDLE, &ba) == 0)
            snap.agents_pct = ba;
        else
            snap.agents_pct = conf.agents_base;
        for (int c = 0; c < 3; c++) {
            snap.wakeups[c] = have_prev && wake[c] > prev_wake[c]
                              ? wake[c] - prev_wake[c] : 0;
            snap.usage_us[c] = have_prev && use[c] > prev_use[c]
                               ? use[c] - prev_use[c] : 0;
            snap.ctxsw[c] = have_prev && ctx[c] > prev_ctx[c]
                            ? ctx[c] - prev_ctx[c] : 0;
        }

        /* ---- decide ---- */
        aios_ap_decision dec;
        aios_ap_tick(&snap, &st, &dec);

        /* ---- act ---- */
        if (dec.adjust_tools > 0)
            aios_cgroup_budget_set(AIOS_KS_BATCH, dec.adjust_tools);
        if (dec.adjust_agents > 0)
            aios_cgroup_budget_set(AIOS_KS_IDLE, dec.adjust_agents);

        /* ---- report: state JSON ---- */
        json_val *o = json_new_obj();
        json_obj_set(o, "tick",      json_new_num((double)snap.tick));
        json_obj_set(o, "ts_ms",     json_new_num(wall_ms()));
        json_obj_set(o, "mode",      json_new_str(aios_ap_mode_name(conf.mode)));
        json_obj_set(o, "pid",       json_new_num((double)getpid()));
        json_obj_set(o, "tools_pct", json_new_num(
            dec.adjust_tools > 0 ? dec.adjust_tools : snap.tools_pct));
        json_obj_set(o, "agents_pct", json_new_num(
            dec.adjust_agents > 0 ? dec.adjust_agents : snap.agents_pct));
        json_obj_set(o, "tools_base",  json_new_num((double)conf.tools_base));
        json_obj_set(o, "agents_base", json_new_num((double)conf.agents_base));
        json_obj_set(o, "registry",  json_new_num((double)registered));
        json_obj_set(o, "ebpf_wakeup", json_new_bool(eb_w));
        json_obj_set(o, "ebpf_ctx",    json_new_bool(eb_c));
        char buf[64];
        for (int c = 0; c < 3; c++) {
            static const char *const cn[3] = { "interactive", "batch", "idle" };
            snprintf(buf, sizeof buf, "wakeups.%s", cn[c]);
            json_obj_set(o, buf, json_new_num((double)snap.wakeups[c]));
            snprintf(buf, sizeof buf, "ctxsw.%s", cn[c]);
            json_obj_set(o, buf, json_new_num((double)snap.ctxsw[c]));
            snprintf(buf, sizeof buf, "util.%s", cn[c]);
            json_obj_set(o, buf, json_new_num(
                snap.dts > 0 ? (double)snap.usage_us[c] / 1e6 / snap.dts * 100.0
                             : 0.0));
        }
        json_obj_set(o, "adjust_tools",  json_new_num((double)dec.adjust_tools));
        json_obj_set(o, "adjust_agents", json_new_num((double)dec.adjust_agents));
        json_obj_set(o, "adjustments_total", json_new_num((double)st.adjustments));
        json_obj_set(o, "reason", json_new_str(dec.reason));

        char *dump = json_dump(o);
        json_free(o);
        if (dump) {
            aios_write_file(state_path, dump, strlen(dump));
            aios_dur_mirror(state_path, "var/autopilot-state.json");
            /* journal: session start, every adjustment, + a heartbeat
             * every 15 ticks (M8: the tick-1 line marks the session
             * boundary in the durable journal) */
            if (dec.adjust_tools > 0 || dec.adjust_agents > 0 ||
                snap.tick == 1 || snap.tick % 15 == 0) {
                FILE *jf = fopen(journal_path, "a");
                if (jf) {
                    fprintf(jf, "%s\n", dump);
                    fclose(jf);
                    aios_dur_mirror(journal_path, "var/autopilot.jsonl");
                }
            }
            free(dump);
        }

        /* ---- next ---- */
        for (int c = 0; c < 3; c++) {
            prev_wake[c] = wake[c];
            prev_use[c]  = use[c];
            prev_ctx[c]  = ctx[c];
        }
        have_prev = 1;
        last_ms = now;
        (void)have_conf_file;
        rc_last = 0;

        /* interruptible sleep */
        for (int waited = 0; waited < sleep_ms && !g_stop; waited += 50)
            msleep(50);
    }

    fprintf(stderr, "[governor] stopped after %llu ticks (%llu adjustments)\n",
            (unsigned long long)tick, (unsigned long long)st.adjustments);
    unlink(state_path); /* the `kernel` builtin reports "not running" */
    return rc_last;
}
