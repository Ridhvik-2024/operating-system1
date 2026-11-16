/* aios/autopilot.h — M7: the closed-loop AI scheduler policy.
 *
 * aios-governor (the daemon) gathers scheduler-pressure signals every
 * tick and calls aios_ap_tick(), a PURE function, to decide whether the
 * cgroup CPU budgets of the intent classes should move:
 *
 *   inputs (per class: interactive/batch/idle):
 *     - kernel wakeup deltas   (M6 /proc/aios per-class totals)
 *     - ctx-switch deltas      (M7 eBPF sched_switch observer)
 *     - CPU usage deltas       (cgroup v2 cpu.stat per class group)
 *   outputs:
 *     - new budget percentages for the tools / agents groups
 *     - a human-readable reason (journalled, shown by `autopilot`)
 *
 * The policy is deliberately small and explainable: raise a class that
 * is pinned at its cap while still hungry (kernel says it has runnable
 * work), take the budget from a class that is quiet, and hand borrowed
 * budgets back after a class stays quiet. Hysteresis lives in
 * aios_ap_state so the tick itself stays stateless.
 */
#ifndef AIOS_AUTOPILOT_H
#define AIOS_AUTOPILOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* well-known autopilot paths inside the running OS (RAM-backed in the
 * image; the `tune`/`autopilot` builtins and the governor agree on them) */
#define AIOS_AP_PATH_CONF    "/var/lib/aios/autopilot.conf"
#define AIOS_AP_PATH_STATE   "/var/lib/aios/autopilot-state.json"
#define AIOS_AP_PATH_JOURNAL "/var/lib/aios/autopilot.jsonl"

typedef enum {
    AIOS_AP_OFF = 0,      /* governor idle: observe, never adjust       */
    AIOS_AP_STATIC = 1,   /* fixed budgets, no adjustments              */
    AIOS_AP_BALANCED = 2, /* default: bounded adjustments with hysteresis */
    AIOS_AP_AGGRESSIVE = 3, /* lower thresholds, bigger steps           */
} aios_ap_mode;

typedef struct {
    aios_ap_mode mode;
    int tools_pct;        /* current budget of the tools group (%)      */
    int agents_pct;       /* current budget of the agents group (%)     */
    int tools_base;       /* boot-time budget (the "home" position)     */
    int agents_base;
    int tools_floor;      /* never shrink below these                   */
    int agents_floor;
    double dts;           /* seconds since the previous snapshot        */
    uint64_t wakeups[3];  /* per-class wakeup deltas since last tick    */
    uint64_t ctxsw[3];    /* per-class ctx-switch deltas (0 = no eBPF)  */
    uint64_t usage_us[3]; /* per-class cpu.stat usage deltas            */
    uint64_t tick;        /* snapshot sequence number                   */
} aios_ap_snapshot;

typedef struct {
    int adjust_tools;     /* new tools budget %, 0 = leave unchanged    */
    int adjust_agents;    /* new agents budget %, 0 = leave unchanged   */
    char reason[256];     /* empty when nothing changed                 */
} aios_ap_decision;

/* hysteresis counters (owned by the caller; zero-initialize) */
typedef struct {
    int tools_quiet;      /* consecutive quiet ticks                    */
    int agents_quiet;
    uint64_t adjustments; /* total adjustments made                     */
} aios_ap_state;

/* The policy. Never fails: with no signal it decides "hands off". */
void aios_ap_tick(const aios_ap_snapshot *snap, aios_ap_state *st,
                  aios_ap_decision *out);

/* config file (written by the `tune` builtin, read by the governor):
 *   mode=balanced|aggressive|static|off
 *   tools=<pct>  agents=<pct>   (base budgets)
 *   tick_ms=<ms>
 * Returns 0 ok; -1 unreadable; -2 invalid value. Unspecified fields keep
 * their defaults from `base`. */
typedef struct {
    aios_ap_mode mode;
    int tools_base, agents_base;
    int tools_floor, agents_floor;
    int tick_ms;
} aios_ap_conf;

void aios_ap_conf_defaults(aios_ap_conf *cf);
int  aios_ap_conf_load(const char *path, aios_ap_conf *base);
int  aios_ap_conf_save(const char *path, const aios_ap_conf *cf);

int         aios_ap_mode_parse(const char *s);       /* -1 unknown */
const char *aios_ap_mode_name(aios_ap_mode m);

/* cpu.max serialization ("50000 100000" <-> 50) */
int aios_ap_cpu_max_parse(const char *s, int *pct);
int aios_ap_cpu_max_format(int pct, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_AUTOPILOT_H */
