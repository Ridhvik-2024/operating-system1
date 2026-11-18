/* apps/firewall-demo.c — M10: the agent firewall, run against real attack chains.
 *
 * Each scenario below is a chain an agent could actually emit. The interesting
 * ones are not the single calls — a policy layer refuses those — but the
 * sequences in which every individual step is legal and the sequence is the
 * attack. Those are what a per-call check cannot see and a conservation check
 * over cumulative state can.
 *
 * The demo prints the model and the laws FIRST, because a verdict you cannot
 * inspect is a verdict you have to believe. Then it runs the chains and exits
 * non-zero if any verdict differs from what the model says it should be — so
 * this file is a test as well as a demonstration.
 */
#include "aios/firewall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;

static void expect(fw_verdict got, fw_verdict want, const char *what) {
    g_checks++;
    if (got == want) {
        printf("  ok   %s\n       -> %s\n", what, fw_verdict_str(got));
    } else {
        g_failed++;
        printf("  FAIL %s\n       wanted %s, got %s\n",
               what, fw_verdict_str(want), fw_verdict_str(got));
    }
}

/* Run a chain and print the refusal verbatim. The sentence is part of the
 * deliverable: a firewall whose refusals cannot be argued with is a firewall
 * people route around. */
static fw_verdict run(const fw_model *m, const fw_call *calls, int n,
                      const char *label) {
    int step = -1, which = -1;
    char detail[512];
    fw_verdict v = fw_chain_eval(m, calls, n, &step, &which, detail, sizeof detail);
    printf("  %-46s -> %s\n", label, fw_verdict_str(v));
    if (detail[0]) printf("       %s\n", detail);
    (void)step; (void)which;
    return v;
}

int main(int argc, char **argv) {
    fw_model m;
    char err[256] = {0};
    /* The world: an 8 KiB workspace, $250.00 in checking, 3 capabilities. */
    if (!fw_model_default(&m, 8192, 25000, 3, err, sizeof err)) {
        printf("the model could not be built: %s\n", err);
        return 2;
    }

    char dump[8192];
    fw_model_dump(&m, dump, sizeof dump);

    if (argc > 1 && strcmp(argv[1], "--model") == 0) {
        printf("%s", dump);
        return 0;
    }

    printf("================================================================\n");
    printf(" THE MODEL — this is the artifact a human signs off on\n");
    printf("================================================================\n");
    printf("%s", dump);

    printf("\n================================================================\n");
    printf(" SCENARIO 1 — an agent tidies up\n");
    printf("================================================================\n");
    printf("  Three calls. Each one alone is reasonable, and is what the\n");
    printf("  per-call policy layer already permits.\n\n");
    {
        fw_call c[] = {
            { FW_RX_READ, 1 },
            { FW_RX_DELETE_TO_TRASH, 1 },
            { FW_RX_BACKUP, 1 },
        };
        fw_verdict v = run(&m, c, 3, "read, delete to trash, back up");
        expect(v, FW_ALLOW, "a reversible delete sequence is allowed");
    }
    printf("\n  The trash is recoverable, so this is the ordinary case. It must\n");
    printf("  be allowed, or the firewall is just a thing that says no.\n");

    printf("\n================================================================\n");
    printf(" SCENARIO 2 — the same agent, one step further: PURGE\n");
    printf("================================================================\n");
    printf("  Adding a fourth call makes the destruction irreversible. Every\n");
    printf("  call is still individually legal.\n\n");
    {
        fw_call c[] = {
            { FW_RX_READ, 1 },
            { FW_RX_DELETE_TO_TRASH, 1 },
            { FW_RX_BACKUP, 1 },
            { FW_RX_PURGE, 1 },
        };
        fw_verdict v = run(&m, c, 4, "read, delete, back up, purge");
        expect(v, FW_DENY_FORBIDDEN,
               "the chain is refused because `destroyed_bytes` cannot grow");
    }

    printf("\n================================================================\n");
    printf(" SCENARIO 3 — exfiltration that conserves everything\n");
    printf("================================================================\n");
    printf("  This is the case that shows why conservation alone is not\n");
    printf("  authorisation. `wire_out` breaks no conservation law: the money\n");
    printf("  leaves `usd:checking` and arrives in `usd:external`, and both\n");
    printf("  sides are in the model. What stops it is the separately-stated\n");
    printf("  rule that `usd:external` may never grow.\n\n");
    {
        fw_call c[] = { { FW_RX_WIRE_OUT, 1 } };
        fw_verdict v = run(&m, c, 1, "wire $1 out to an external account");
        expect(v, FW_DENY_FORBIDDEN, "money leaving your control is refused");
    }

    printf("\n  An INTERNAL transfer, by contrast, is allowed: it conserves\n");
    printf("  `usd:checking + usd:savings + usd:external` by construction and\n");
    printf("  it does not grow a forbidden quantity. This difference is the\n");
    printf("  whole design: conserve, and stay inside the envelope.\n\n");
    {
        fw_call c[] = { { FW_RX_TRANSFER, 100 } };
        fw_verdict v = run(&m, c, 1, "move $1.00 checking -> savings");
        expect(v, FW_ALLOW, "an internal transfer is allowed");
    }

    printf("\n================================================================\n");
    printf(" SCENARIO 4 — the drip: many legal steps, one illegal state\n");
    printf("================================================================\n");
    printf("  This is the multi-step attack the proposal names. Each call is\n");
    printf("  inside every per-call limit. The chain is not.\n\n");
    {
        fw_call ok8[8];
        for (int i = 0; i < 8; i++) ok8[i] = (fw_call){ FW_RX_DELETE_TO_TRASH, 512 };
        fw_verdict v = run(&m, ok8, 8, "8 x 512 bytes moved to trash (4096 total)");
        expect(v, FW_ALLOW, "partial cleanup of the workspace is allowed");

        fw_call too_much[20];
        for (int i = 0; i < 20; i++) too_much[i] = (fw_call){ FW_RX_DELETE_TO_TRASH, 512 };
        int step = -1, which = -1;
        char detail[512];
        fw_verdict v2 = fw_chain_eval(&m, too_much, 20, &step, &which,
                                      detail, sizeof detail);
        printf("  %-46s -> %s\n", "20 x 512 bytes moved to trash", fw_verdict_str(v2));
        printf("       %s\n", detail);
        expect(v2, FW_DENY_ENVELOPE,
               "the drip is stopped when the CUMULATIVE state leaves the box");
        g_checks++;
        if (step == 16) {
            printf("  ok   and it stops at step %d, the exact step that overflows "
                   "the envelope\n", step + 1);
        } else {
            g_failed++;
            printf("  FAIL stopped at step %d, expected 17\n", step + 1);
        }
    }

    printf("\n  Note that the same calls, fewer of them, are ALLOWED above. The\n");
    printf("  firewall does not know that deleting is bad; it knows where the\n");
    printf("  chain may END, and that is a property of the sequence.\n");

    printf("\n================================================================\n");
    printf(" SCENARIO 5 — capability laundering\n");
    printf("================================================================\n");
    printf("  Spending capabilities conserves by construction, so it is\n");
    printf("  allowed — up to the number held. An agent that tries to spend\n");
    printf("  more than it has is stopped by the same envelope check.\n\n");
    {
        fw_call spend[3];
        for (int i = 0; i < 3; i++) spend[i] = (fw_call){ FW_RX_SPEND_CAPABILITY, 1 };
        fw_verdict v = run(&m, spend, 3, "spend all 3 capabilities");
        expect(v, FW_ALLOW, "spending what you hold is allowed");

        fw_call over[4];
        for (int i = 0; i < 4; i++) over[i] = (fw_call){ FW_RX_SPEND_CAPABILITY, 1 };
        fw_verdict v2 = run(&m, over, 4, "spend 4 capabilities when 3 are held");
        expect(v2, FW_DENY_ENVELOPE, "and spending more than you hold is not");
    }

    printf("\n================================================================\n");
    printf(" SCENARIO 6 — determinism\n");
    printf("================================================================\n");
    printf("  The same chain must produce the same verdict and the same\n");
    printf("  sentence, every time. A heuristic cannot promise this; an exact\n");
    printf("  integer check can, and here it is asked to.\n\n");
    {
        int step = -1, which = -1;
        char d1[512], d2[512];
        fw_call c[] = { { FW_RX_DELETE_TO_TRASH, 1 }, { FW_RX_PURGE, 1 } };
        fw_verdict a = fw_chain_eval(&m, c, 2, &step, &which, d1, sizeof d1);
        fw_verdict b = fw_chain_eval(&m, c, 2, &step, &which, d2, sizeof d2);
        g_checks++;
        if (a == b && strcmp(d1, d2) == 0) {
            printf("  ok   identical verdict and identical reasoning on re-run\n");
        } else {
            g_failed++;
            printf("  FAIL non-deterministic verdict\n");
        }
        printf("       \"%s\"\n", d1);
    }

    printf("\n================================================================\n");
    printf(" What this does NOT do — stated so it is not mistaken for solved\n");
    printf("================================================================\n");
    printf("  * There is no neural critic. A learned component cannot be shown\n");
    printf("    to hold, drifts with its training data, and fails silently off\n");
    printf("    that distribution. It may one day PROPOSE the species mapping;\n");
    printf("    the exact check above must still decide.\n");
    printf("  * The species mapping is AUTHORED. The mathematics is only as\n");
    printf("    honest as the model, which is why the model is printed above\n");
    printf("    and is meant to be read.\n");
    printf("  * Conservation is not authorisation: a chain can conserve\n");
    printf("    perfectly and still be an attack. That is what the envelope and\n");
    printf("    the forbidden-quantity rule are for, and why the M2 policy layer\n");
    printf("    still runs.\n");
    printf("  * It is not wired into sh_exec() yet. This is the engine and its\n");
    printf("    proof, not the enforcement point — hooking it into the shell's\n");
    printf("    action gate is the next step, and it is a change to the gate\n");
    printf("    rather than to this file.\n");

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
