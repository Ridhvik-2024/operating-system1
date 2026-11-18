/* aios/firewall.h — M10: the agent firewall, as a network of reactions.
 *
 * The layer above (stoich.h) is mathematics with no opinion about the world.
 * This layer is the OPINION: it says that a delete is a reaction from `jail` to
 * `trash`, that a wire is a reaction from `usd:checking` to `usd:external`, and
 * that `usd:external` is the one species a chain may never grow.
 *
 * That mapping is the whole security property. The mathematics is only as honest
 * as the model, which is why the model is a readable struct you can review and
 * diff — not weights. A firewall whose species mapping is wrong is a firewall
 * that conserves the wrong thing perfectly.
 *
 * The threat this is shaped around is the MULTI-STEP chain, because single steps
 * are what the existing policy layer already refuses. Each of these is legal
 * alone:
 *
 *   read 100 files           (5 reads, 5 reads, ... legal)
 *   move 10% of an account   (legal, under any per-call limit)
 *   grant a permission       (legal, if you hold the capability)
 *
 * and the sequence is the attack. So the check is on the CUMULATIVE state of the
 * chain, and on the state the chain ENDS in.
 */
#ifndef AIOS_FIREWALL_H
#define AIOS_FIREWALL_H

#include <stdbool.h>
#include <stddef.h>

#include "aios/stoich.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The default model. Four species groups, and the point is that `usd:external`
 * is a sink: money may leave a watched pair of accounts only by a reaction whose
 * conservation law says so, and the envelope forbids the destination from
 * growing. */
typedef enum {
    FW_SPECIES_JAIL = 0,      /* bytes resident in the workspace             */
    FW_SPECIES_TRASH,         /* bytes moved out but recoverable             */
    FW_SPECIES_DESTROYED,     /* bytes actually gone (a delete, unrecoverable)*/
    FW_SPECIES_BACKUP,        /* a second copy, held against a later restore   */
    FW_SPECIES_CHECKING,      /* usd, cents                                 */
    FW_SPECIES_SAVINGS,
    FW_SPECIES_EXTERNAL,      /* usd that has left the user's control        */
    FW_SPECIES_CAPABILITY,    /* permission tokens held                      */
    FW_SPECIES_GRANTED,       /* permissions exercised                       */
    FW_SPECIES__COUNT
} fw_species;

typedef enum {
    FW_RX_READ = 0,           /* touch a file: jail -> jail (no quantity change) */
    FW_RX_DELETE_TO_TRASH,    /* jail -> trash                                    */
    FW_RX_PURGE,              /* trash -> destroyed    (the irreversible one)     */
    FW_RX_BACKUP,             /* jail -> jail + trash  (copy)                     */
    FW_RX_WIRE_OUT,           /* checking -> external  (leaves your control)      */
    FW_RX_TRANSFER,           /* checking <-> savings  (internal, conserves)      */
    FW_RX_SPEND_CAPABILITY,   /* capability -> granted                            */
    FW_RX__COUNT
} fw_reaction;

typedef struct {
    stoich_net  net;
    stoich_laws laws;
    stoich_crn  crn;
    /* The species whose growth is the alarm. A chain that raises any of these
     * above its initial value has done the thing this firewall exists to stop. */
    bool        forbidden[AIOS_STOICH_MAX_SPECIES];
    char        model_err[256];
} fw_model;

/* Build the default model with the given quantities. `jail_bytes` etc. are the
 * quantities the model starts from — the current state of the world, not a
 * limit. Returns false and fills model_err when the model itself is malformed
 * (a species index out of range, a reaction against a missing species). A
 * malformed model fails CLOSED: nothing is permitted. */
bool fw_model_default(fw_model *m, long long jail_bytes, long long usd_cents,
                      long long capabilities, char *err, size_t ecap);

/* True when some conservation law weights jail_bytes, trash_bytes,
 * destroyed_bytes and backup_bytes equally — i.e. the model cannot create or
 * destroy bytes. Exposed so a test can feed it a network that should fail; the
 * constructor uses it and refuses the model when it returns false. */
bool fw_laws_conserve_bytes(const stoich_net *net, const stoich_laws *laws);

/* Raise a species' envelope: this is a human widening what a chain may do. Kept
 * separate from model construction so that every widening is a call somebody can
 * grep for, rather than a number buried in a default. */
void fw_authorize(fw_model *m, int species, long long lo, long long hi);

/* Forbid a species from ever exceeding its initial value. Used for
 * `usd:external` and `destroyed`. */
void fw_forbid_growth(fw_model *m, int species);

/* One tool call, as the firewall sees it: a reaction and how much of it. */
typedef struct {
    int       reaction;
    long long amount;     /* copies of the reaction; 1 for a single event */
} fw_call;

typedef enum {
    FW_ALLOW = 0,
    FW_DENY_LAW,
    FW_DENY_ENVELOPE,
    FW_DENY_FORBIDDEN,
    FW_DENY_MODEL,
    FW_DENY_ARG
} fw_verdict;

const char *fw_verdict_str(fw_verdict v);

/* Evaluate a whole chain against the model, from the model's initial state.
 *
 * Checks, in order and per step:
 *   1. the step itself (laws, envelope)   -> FW_DENY_LAW / FW_DENY_ENVELOPE
 *   2. the forbidden species, on the state AFTER the step
 * Stopping at the first failure is deliberate: a chain that is refused at step 3
 * of 5 must report 3, so the person can see WHICH step was the attack rather
 * than being told the whole chain was bad.
 *
 * The chain is evaluated on a scratch state and never mutates `m`, so a refused
 * chain leaves no trace in the model: the firewall is a pure function of (model,
 * chain), which is what makes its verdicts reproducible. */
fw_verdict fw_chain_eval(const fw_model *m, const fw_call *calls, int n,
                         int *failed_step, int *which,
                         char *detail, size_t dcap);

/* Render the model as the reviewable thing it is: every species with its
 * envelope, every reaction with its terms, then the conservation laws and the
 * deficiency. This is what a human signs off on. */
void fw_model_dump(const fw_model *m, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_FIREWALL_H */
