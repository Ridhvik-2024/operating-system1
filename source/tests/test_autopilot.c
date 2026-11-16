/* test_autopilot.c — M7 suite: the micro language model, the autopilot
 * policy, budget plumbing and the routing classes.
 *
 * Pure parts (microlm, policy, cpu.max math, conf io) are asserted on
 * any host. Kernel-backed parts (/proc/aios totals, cgroup budgets,
 * eBPF observers) run when the facility is present and SKIP honestly
 * otherwise — the same discipline as test_kernel (M6).
 */
#include "t.h"
#include "aios/microlm.h"
#include "aios/autopilot.h"
#include "aios/ksched.h"
#include "aios/ebpf.h"
#include "aios/json.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>

/* ---------------- microlm ---------------- */

static void test_microlm(void) {
    CHECK(aios_microlm_ready() == 1, "micro-lm weights linked in");

    /* the baked acceptance set must classify correctly */
    CHECK(aios_microlm_selftest() == 0, "microlm selftest: all phrases OK");

    /* direct classification with confidence bounds */
    struct { const char *text; aios_lm_class want; } cases[] = {
        { "run the nightly report in the background at idle priority",
          AIOS_LM_BG },
        { "the console is frozen fix it asap", AIOS_LM_URGENT },
        { "summarize the workspace files", AIOS_LM_NORMAL },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        double conf = -1.0;
        aios_lm_class c = aios_microlm_classify(cases[i].text, &conf);
        CHECK(c == cases[i].want, "classifier picks the trained class");
        CHECK(conf >= 0.0 && conf <= 1.0, "confidence within [0,1]");
    }

    /* determinism: identical input -> identical verdict and text */
    double c1 = 0, c2 = 0;
    aios_lm_class k1 = aios_microlm_classify("queue it as background work",
                                             &c1);
    aios_lm_class k2 = aios_microlm_classify("queue it as background work",
                                             &c2);
    CHECK(k1 == k2 && c1 == c2, "classify is deterministic");

    char a[256], b[256];
    CHECK(aios_microlm_generate("about the scheduler", a, sizeof a) > 0,
          "generator produces a sentence");
    CHECK(aios_microlm_generate("about the scheduler", b, sizeof b) > 0,
          "generator runs twice");
    CHECK(strcmp(a, b) == 0, "generation is deterministic");
    CHECK(a[0] != 0 && strlen(a) < 200, "generated sentence is bounded");
    CHECK(a[strlen(a) - 1] == '.', "generated sentence ends with a period");
    CHECK(aios_microlm_generate("x", a, sizeof a) > 0,
          "generator works for an unknown prompt too");

    /* stats + class names */
    char stats[512];
    CHECK(aios_microlm_stats(stats, sizeof stats) > 0, "stats renders");
    CHECK_STR_CONTAINS(stats, "vocab=", "stats mentions vocab size");
    CHECK_STR_CONTAINS(stats, "micro-lm", "stats is labelled micro-lm");
    CHECK(strcmp(aios_microlm_class_name(AIOS_LM_BG), "bg") == 0,
          "class name bg");
    CHECK(strcmp(aios_microlm_class_name(AIOS_LM_URGENT), "urgent") == 0,
          "class name urgent");
    CHECK(strcmp(aios_microlm_class_name(AIOS_LM_NORMAL), "normal") == 0,
          "class name normal");
}

/* ---------------- autopilot policy (pure) ---------------- */

static void fill_snap(aios_ap_snapshot *s) {
    memset(s, 0, sizeof *s);
    s->mode = AIOS_AP_BALANCED;
    s->tools_pct = 50; s->agents_pct = 25;
    s->tools_base = 50; s->agents_base = 25;
    s->tools_floor = 20; s->agents_floor = 10;
    s->dts = 2.0; /* seconds */
    /* defaults: everything quiet */
    s->usage_us[0] = 100000;  /* interactive: 0.1s of 2s = 5%  */
    s->usage_us[1] = 100000;  /* batch: 5% of its 50% budget   */
    s->usage_us[2] = 100000;  /* idle: 5% of its 25% budget    */
}

static void test_policy_static_and_off(void) {
    aios_ap_snapshot s;
    fill_snap(&s);
    aios_ap_state st = {0};
    aios_ap_decision d;

    s.mode = AIOS_AP_OFF;
    aios_ap_tick(&s, &st, &d);
    CHECK(d.adjust_tools == 0 && d.adjust_agents == 0,
          "off: never adjusts");
    CHECK(strstr(d.reason, "off") != NULL, "off: reason names the mode");

    s.mode = AIOS_AP_STATIC;
    aios_ap_tick(&s, &st, &d);
    CHECK(d.adjust_tools == 0 && d.adjust_agents == 0,
          "static: never adjusts");
}

static void test_policy_raise_agents(void) {
    /* idle class pinned at its 25% cap (fully utilized over the tick),
     * kernel reports wakeups (runnable work), tools quiet -> the
     * governor must raise agents and take the budget from tools. */
    aios_ap_snapshot s;
    fill_snap(&s);
    s.usage_us[2] = 1900000;  /* idle: ~95% of one cpu over 2s  */
    s.usage_us[1] = 100000;   /* tools: quiet (5%)              */
    s.usage_us[0] = 100000;   /* interactive: quiet             */
    s.wakeups[2] = 500;       /* kernel: idle class was waking  */

    aios_ap_state st = {0};
    aios_ap_decision d;
    aios_ap_tick(&s, &st, &d);
    CHECK(d.adjust_agents == 35, "agents raised 25 -> 35 (step 10)");
    CHECK(d.adjust_tools == 40, "tools lent the budget 50 -> 40");
    CHECK(strstr(d.reason, "agents") != NULL, "reason names the class");
    CHECK(st.adjustments == 1, "adjustment counted");
}

static void test_policy_hold_when_no_lender(void) {
    /* agents hungry but tools are ALSO busy: nothing to lend from */
    aios_ap_snapshot s;
    fill_snap(&s);
    s.usage_us[2] = 1900000;  /* agents: ~95%   */
    s.usage_us[1] = 1900000;  /* tools: ~95%    */
    s.usage_us[0] = 100000;
    s.wakeups[2] = 500;

    aios_ap_state st = {0};
    aios_ap_decision d;
    aios_ap_tick(&s, &st, &d);
    CHECK(d.adjust_agents == 0 && d.adjust_tools == 0,
          "no budget move when no class is quiet");
    CHECK_STR_CONTAINS(d.reason, "holding",
                       "reason says it is holding");
}

static void test_policy_raise_tools(void) {
    aios_ap_snapshot s;
    fill_snap(&s);
    s.usage_us[1] = 1900000;  /* tools: ~95% of one cpu */
    s.usage_us[2] = 100000;   /* agents quiet           */
    s.usage_us[0] = 100000;
    aios_ap_state st = {0};
    aios_ap_decision d;
    aios_ap_tick(&s, &st, &d);
    CHECK(d.adjust_tools == 60, "tools raised 50 -> 60 (step 10)");
    CHECK(d.adjust_agents == 15, "agents lent the budget 25 -> 15");
    CHECK(d.adjust_agents >= 10, "agents never pushed below floor 10");
}

static void test_policy_return_borrowed(void) {
    /* tools were previously raised to 70 (base 50): three quiet ticks
     * must hand the budget back. */
    aios_ap_snapshot s;
    fill_snap(&s);
    s.tools_pct = 70;
    s.usage_us[1] = 10000;    /* ~0.5% utilized */
    aios_ap_state st = {0};
    aios_ap_decision d;

    aios_ap_tick(&s, &st, &d);
    CHECK(d.adjust_tools == 0, "quiet tick 1: no change yet");
    aios_ap_tick(&s, &st, &d);
    CHECK(d.adjust_tools == 0, "quiet tick 2: no change yet");
    aios_ap_tick(&s, &st, &d);
    CHECK(d.adjust_tools == 60, "quiet tick 3: budget returned one step");
    CHECK(strstr(d.reason, "returning") != NULL,
          "reason says the budget is returned");
    /* a busy tick resets the hysteresis */
    fill_snap(&s);
    s.tools_pct = 70;
    s.usage_us[1] = 1900000;
    st.tools_quiet = 2;
    aios_ap_tick(&s, &st, &d);
    CHECK(st.tools_quiet == 0, "busy tick resets the quiet counter");
}

static void test_policy_aggressive(void) {
    aios_ap_snapshot s;
    fill_snap(&s);
    s.mode = AIOS_AP_AGGRESSIVE;
    s.usage_us[2] = 1900000;
    s.usage_us[1] = 100000;
    s.wakeups[2] = 500;
    aios_ap_state st = {0};
    aios_ap_decision d;
    aios_ap_tick(&s, &st, &d);
    CHECK(d.adjust_agents == 40, "aggressive: bigger step (25 -> 40)");
}

/* ---------------- cpu.max math + conf io ---------------- */

static void test_cpu_max_roundtrip(void) {
    int pct = 0;
    CHECK(aios_ap_cpu_max_parse("50000 100000", &pct) == 0 && pct == 50,
          "cpu.max 50000/100000 = 50%");
    CHECK(aios_ap_cpu_max_parse("25000 100000", &pct) == 0 && pct == 25,
          "cpu.max 25000/100000 = 25%");
    CHECK(aios_ap_cpu_max_parse("max", &pct) == 0 && pct == 100,
          "cpu.max 'max' = 100%");
    CHECK(aios_ap_cpu_max_parse("1000 100000", &pct) == 0 && pct == 1,
          "cpu.max clamps to >= 1%");
    CHECK(aios_ap_cpu_max_parse("garbage", &pct) == -1,
          "cpu.max garbage rejected");

    char buf[64];
    CHECK(aios_ap_cpu_max_format(50, buf, sizeof buf) > 0 &&
          strcmp(buf, "50000 100000") == 0, "format 50% round-trips");
    CHECK(aios_ap_cpu_max_format(25, buf, sizeof buf) > 0 &&
          strcmp(buf, "25000 100000") == 0, "format 25% round-trips");
    CHECK(aios_ap_cpu_max_format(100, buf, sizeof buf) > 0 &&
          strcmp(buf, "100000 100000") == 0, "format 100% round-trips");
}

static void test_conf_io(void) {
    const char *path = "/tmp/aios-ap-conf-test";
    aios_ap_conf cf;
    aios_ap_conf_defaults(&cf);
    CHECK(cf.mode == AIOS_AP_BALANCED && cf.tools_base == 50 &&
          cf.agents_base == 25 && cf.tick_ms == 2000,
          "conf defaults are balanced 50/25/2000");

    aios_ap_conf saved = cf;
    saved.mode = AIOS_AP_AGGRESSIVE;
    saved.tools_base = 60;
    saved.agents_base = 30;
    saved.tick_ms = 1500;
    CHECK(aios_ap_conf_save(path, &saved) == 0, "conf save writes the file");

    aios_ap_conf loaded;
    aios_ap_conf_defaults(&loaded);
    CHECK(aios_ap_conf_load(path, &loaded) == 0, "conf load reads it back");
    CHECK(loaded.mode == AIOS_AP_AGGRESSIVE, "loaded mode");
    CHECK(loaded.tools_base == 60 && loaded.agents_base == 30,
          "loaded base budgets");
    CHECK(loaded.tick_ms == 1500, "loaded tick");

    /* invalid values are rejected, not silently accepted */
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "rewrite conf for the invalid-value test");
    if (f) {
        fputs("mode=banana\n", f);
        fclose(f);
    }
    aios_ap_conf bad;
    aios_ap_conf_defaults(&bad);
    CHECK(aios_ap_conf_load(path, &bad) == -2, "invalid mode rejected");
    CHECK(aios_ap_conf_load("/tmp/aios-ap-conf-does-not-exist", &bad) == -1,
          "missing conf file reported");
    unlink(path);
    CHECK(aios_ap_mode_parse("balanced") == AIOS_AP_BALANCED, "mode parse");
    CHECK(aios_ap_mode_parse("nope") == -1, "mode parse rejects junk");
}

/* ---------------- kernel-backed (best-effort) ---------------- */

static void test_kernel_bridge_live(void) {
    uint64_t wake[3];
    int registered = -1;
    int rc = aios_kernel_wakeups(wake, &registered);
    if (rc != 0) {
        SKIP("kernel bridge /proc/aios absent on this host");
        return;
    }
    CHECK(registered >= 0, "registry count readable");
    /* totals must agree with the registry scan */
    long pids[512];
    int cls[512];
    int n = aios_kernel_registry(pids, cls, 512);
    CHECK(n >= 0, "registry scan works");
    if (registered >= 0 && n >= 0)
        CHECK(n == registered, "registry scan matches registered count");
}

static void test_cgroup_budgets_live(void) {
    int pct = 0;
    if (aios_cgroup_budget_get(AIOS_KS_BATCH, &pct) != 0) {
        SKIP("cgroup v2 aios groups absent on this host");
        return;
    }
    int old_tools = pct, old_agents = 0;
    aios_cgroup_budget_get(AIOS_KS_IDLE, &old_agents);

    CHECK(aios_cgroup_budget_set(AIOS_KS_BATCH, 55) == 0, "set tools 55");
    pct = 0;
    CHECK(aios_cgroup_budget_get(AIOS_KS_BATCH, &pct) == 0 && pct == 55,
          "budget set/get round-trip");
    CHECK(aios_cgroup_budget_set(AIOS_KS_BATCH, old_tools) == 0,
          "restore tools budget");
    CHECK(aios_cgroup_budget_get(AIOS_KS_BATCH, &pct) == 0 &&
          pct == old_tools, "restored");

    uint64_t use = 0;
    CHECK(aios_cgroup_usage_us(AIOS_KS_BATCH, &use) == 0,
          "cpu.stat usage readable");
    (void)old_agents;
}

static void test_ebpf_ctx_live(void) {
    if (aios_ebpf_ctx_start() != 0) {
        SKIP("eBPF ctx observer unavailable "
             "(%s)", aios_ebpf_ctx_error());
        return;
    }
    /* burn some cpu, then yield — blocking on usleep always switches us
     * out, so the observer gets deterministic evidence to count (a short
     * busy loop alone may fit inside one slice under heavy TCG load) */
    volatile double x = 0;
    for (int i = 0; i < 2000000; i++) x += i * 0.5;
    for (int i = 0; i < 5; i++) usleep(2000);
    uint64_t c = 0;
    CHECK(aios_ebpf_ctx_count((long)getpid(), &c) == 0, "ctx count read");
    CHECK(c > 0, "ctx observer counted switch-outs of a busy loop");
    aios_ebpf_ctx_stop();
}

/* ---------------- routing classes via the shell decision path ------- */

static void test_sched_class_helpers(void) {
    /* the pure side of the router: the micro-lm must move a class-free
     * background request into bg (used by shroute in 'micro' mode) */
    double conf = 0.0;
    aios_lm_class c = aios_microlm_classify(
        "tidy the archive when the machine gets a chance", &conf);
    CHECK(c == AIOS_LM_BG, "micro-lm: cue-free background line -> bg");
    c = aios_microlm_classify("the console is unresponsive, act", &conf);
    CHECK(c == AIOS_LM_URGENT, "micro-lm: cue-free urgent line -> urgent");
    c = aios_microlm_classify("draft a memo about the milestones", &conf);
    CHECK(c == AIOS_LM_NORMAL, "micro-lm: normal line stays normal");
}

static void run(void) {
    test_microlm();
    test_policy_static_and_off();
    test_policy_raise_agents();
    test_policy_hold_when_no_lender();
    test_policy_raise_tools();
    test_policy_return_borrowed();
    test_policy_aggressive();
    test_cpu_max_roundtrip();
    test_conf_io();
    test_sched_class_helpers();
    test_kernel_bridge_live();
    test_cgroup_budgets_live();
    test_ebpf_ctx_live();
}

TEST_MAIN()
