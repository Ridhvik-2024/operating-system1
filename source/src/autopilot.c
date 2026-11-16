/* autopilot.c — M7: the closed-loop AI scheduler policy (pure logic).
 * See include/aios/autopilot.h. Everything here is deterministic and
 * unit-tested with synthetic snapshots; the governor daemon only feeds
 * real signals in and writes the decisions out.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aios/autopilot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "aios/types.h"

/* class indices, matching aios_ksched_class */
#define CL_INTER 0
#define CL_BATCH 1
#define CL_IDLE  2

int aios_ap_mode_parse(const char *s) {
    if (!s || !*s) return -1;
    if (strcmp(s, "off") == 0)        return AIOS_AP_OFF;
    if (strcmp(s, "static") == 0)     return AIOS_AP_STATIC;
    if (strcmp(s, "balanced") == 0)   return AIOS_AP_BALANCED;
    if (strcmp(s, "aggressive") == 0) return AIOS_AP_AGGRESSIVE;
    return -1;
}

const char *aios_ap_mode_name(aios_ap_mode m) {
    switch (m) {
    case AIOS_AP_OFF:        return "off";
    case AIOS_AP_STATIC:     return "static";
    case AIOS_AP_AGGRESSIVE: return "aggressive";
    default:                 return "balanced";
    }
}

void aios_ap_conf_defaults(aios_ap_conf *cf) {
    memset(cf, 0, sizeof *cf);
    cf->mode         = AIOS_AP_BALANCED;
    cf->tools_base   = 50;
    cf->agents_base  = 25;
    cf->tools_floor  = 20;
    cf->agents_floor = 10;
    cf->tick_ms      = 2000;
}

/* ---------------- config file ---------------- */

int aios_ap_conf_load(const char *path, aios_ap_conf *base) {
    char *buf = NULL;
    size_t len = 0;
    if (aios_read_file(path, &buf, &len, 4096) != AIOS_OK) return -1;

    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || *line == 0) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        if (strcmp(key, "mode") == 0) {
            int m = aios_ap_mode_parse(val);
            if (m < 0) { free(buf); return -2; }
            base->mode = (aios_ap_mode)m;
        } else if (strcmp(key, "tools") == 0) {
            int v = atoi(val);
            if (v < 1 || v > 100) { free(buf); return -2; }
            base->tools_base = v;
        } else if (strcmp(key, "agents") == 0) {
            int v = atoi(val);
            if (v < 1 || v > 100) { free(buf); return -2; }
            base->agents_base = v;
        } else if (strcmp(key, "tick_ms") == 0) {
            int v = atoi(val);
            if (v < 100 || v > 60000) { free(buf); return -2; }
            base->tick_ms = v;
        }
    }
    free(buf);
    return 0;
}

int aios_ap_conf_save(const char *path, const aios_ap_conf *cf) {
    char buf[256];
    int n = snprintf(buf, sizeof buf,
                     "# AI-OS autopilot configuration (tune builtin)\n"
                     "mode=%s\ntools=%d\nagents=%d\ntick_ms=%d\n",
                     aios_ap_mode_name(cf->mode), cf->tools_base,
                     cf->agents_base, cf->tick_ms);
    if (n < 0) return -1;
    return aios_write_file(path, buf, (size_t)n) == AIOS_OK ? 0 : -1;
}

/* ---------------- cpu.max serialization ---------------- */

int aios_ap_cpu_max_parse(const char *s, int *pct) {
    if (!s || !pct) return -1;
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "max", 3) == 0) { *pct = 100; return 0; }
    int quota = 0, period = 0;
    if (sscanf(s, "%d %d", &quota, &period) != 2 || period <= 0 ||
        quota < 0)
        return -1;
    *pct = (quota * 100 + period / 2) / period;
    if (*pct < 1) *pct = 1;
    if (*pct > 100) *pct = 100;
    return 0;
}

int aios_ap_cpu_max_format(int pct, char *out, size_t cap) {
    if (!out || !cap) return -1;
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    return snprintf(out, cap, "%d 100000", pct * 1000);
}

/* ---------------- the policy ---------------- */

static int clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* utilization of one class, in percent of one CPU, from the usage delta */
static int util_pct(const aios_ap_snapshot *s, int cls) {
    if (s->dts <= 0.0) return 0;
    double pct = ((double)s->usage_us[cls] / 1e6) / s->dts * 100.0;
    if (pct < 0.0) pct = 0.0;
    if (pct > 400.0) pct = 400.0;   /* multi-cpu groups may exceed 100 */
    return (int)(pct + 0.5);
}

void aios_ap_tick(const aios_ap_snapshot *snap, aios_ap_state *st,
                  aios_ap_decision *out) {
    memset(out, 0, sizeof *out);
    if (!snap || !st || !out) return;

    const aios_ap_mode mode = snap->mode;
    if (mode == AIOS_AP_OFF) {
        snprintf(out->reason, sizeof out->reason,
                 "mode=off: observing, not adjusting");
        return;
    }
    if (mode == AIOS_AP_STATIC) {
        snprintf(out->reason, sizeof out->reason,
                 "mode=static: budgets fixed at tools=%d agents=%d",
                 snap->tools_pct, snap->agents_pct);
        return;
    }

    const int hi       = (mode == AIOS_AP_AGGRESSIVE) ? 70 : 85;
    const int lo       = (mode == AIOS_AP_AGGRESSIVE) ? 20 : 30;
    const int step     = (mode == AIOS_AP_AGGRESSIVE) ? 15 : 10;
    const int quiet_n  = 3;

    int tools  = clamp(snap->tools_pct,  1, 100);
    int agents = clamp(snap->agents_pct, 1, 100);
    int ut = util_pct(snap, CL_BATCH);
    int ua = util_pct(snap, CL_IDLE);
    int ui = util_pct(snap, CL_INTER);

    /* -------- raise a class pinned at its cap while still hungry -----
     * Invariant: total caps stay bounded — a class only gains budget
     * that a QUIET class lends back, so the sum tools+agents never
     * grows beyond its boot-time total. */
    /* agents: kernel says the idle class has runnable work (wakeups) and
     * it burned its whole budget -> it is NOT getting enough cpu. */
    if (snap->wakeups[CL_IDLE] > 0 &&
        ua >= hi * agents / 100 + (agents / 10) &&
        agents < 100) {
        int want = clamp(agents + step, agents, 100);
        int give = want - agents;
        if (ut < lo && tools - give >= snap->tools_floor) {
            tools -= give;
            agents = want;
            snprintf(out->reason, sizeof out->reason,
                     "agents pinned at %d%% with %llu wakeups and %d%% "
                     "utilized — raising to %d%% (tools quiet at %d%%, "
                     "lends budget)",
                     agents - give, (unsigned long long)snap->wakeups[CL_IDLE],
                     ua, want, ut);
            out->adjust_agents = agents;
            out->adjust_tools  = tools != snap->tools_pct ? tools : 0;
            st->tools_quiet = st->agents_quiet = 0;
            st->adjustments++;
        } else {
            snprintf(out->reason, sizeof out->reason,
                     "agents pinned at %d%% (%d%% utilized, %llu wakeups) "
                     "but no quiet class to lend from — holding",
                     agents, ua, (unsigned long long)snap->wakeups[CL_IDLE]);
        }
        return;
    }

    /* tools: batch work saturated its cap (usage speaks for itself). */
    if (ut >= hi * tools / 100 + (tools / 10) && tools < 100) {
        int want = clamp(tools + step, tools, 100);
        int give = want - tools;
        if (ua < lo && agents - give >= snap->agents_floor) {
            int old_tools = tools;
            agents -= give;
            tools = want;
            out->adjust_tools  = tools;
            out->adjust_agents = agents != snap->agents_pct ? agents : 0;
            st->tools_quiet = st->agents_quiet = 0;
            st->adjustments++;
            snprintf(out->reason, sizeof out->reason,
                     "tools pinned at %d%% (%d%% utilized) — raising to "
                     "%d%% (agents quiet at %d%%, lends budget)",
                     old_tools, ut, want, ua);
        } else {
            snprintf(out->reason, sizeof out->reason,
                     "tools pinned at %d%% (%d%% utilized) but no quiet "
                     "class to lend from — holding", tools, ut);
        }
        return;
    }

    /* -------- hand borrowed budgets back when a class stays quiet ----- */
    if (tools > snap->tools_base &&
        ut < lo / 2 && snap->wakeups[CL_BATCH] == 0) {
        if (++st->tools_quiet >= quiet_n) {
            int want = clamp(tools - step, snap->tools_base, tools);
            snprintf(out->reason, sizeof out->reason,
                     "tools quiet for %d ticks at %d%% utilized — "
                     "returning budget to base (%d%%)",
                     st->tools_quiet, ut, want);
            out->adjust_tools = want;
            st->tools_quiet = 0;
            st->adjustments++;
            return;
        }
    } else {
        st->tools_quiet = 0;
    }

    if (agents > snap->agents_base &&
        ua < lo / 2 && snap->wakeups[CL_IDLE] == 0) {
        if (++st->agents_quiet >= quiet_n) {
            int want = clamp(agents - step, snap->agents_base, agents);
            snprintf(out->reason, sizeof out->reason,
                     "agents quiet for %d ticks at %d%% utilized — "
                     "returning budget to base (%d%%)",
                     st->agents_quiet, ua, want);
            out->adjust_agents = want;
            st->agents_quiet = 0;
            st->adjustments++;
            return;
        }
    } else {
        st->agents_quiet = 0;
    }

    snprintf(out->reason, sizeof out->reason,
             "steady: tools %d%%/%d%% util, agents %d%%/%d%% util, "
             "interactive %d%% — no adjustment",
             snap->tools_pct, ut, snap->agents_pct, ua, ui);
}
