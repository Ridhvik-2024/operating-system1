/* aios/audit.h — M6: the record is a chain, so editing history is detectable.
 *
 * Every action the machine takes is appended to <store>/audit.log, one JSON
 * object per line. That was already true (M4). What M6 adds is that each record
 * carries the hash of the record before it, so the file is a CHAIN: change a byte
 * anywhere and every record after it no longer verifies.
 *
 *   {"seq":7,"ts":...,"akind":"tool","origin":"ai","action":{...},
 *    "verdict":"allow","reason":"R_OK","effects":[...],"ok":true,
 *    "prev":"<16 hex>","hash":"<16 hex>"}
 *
 *   hash = fnv1a64( prev || canonical(record_without_hash) )
 *
 * HONEST LIMITS — read these before trusting the word "tamper-evident":
 *
 *   1. fnv1a64 IS NOT CRYPTOGRAPHIC. It is 64 bits of non-linear mixing. Someone
 *      who knows the algorithm can construct a record with any chosen `hash` and
 *      a different body in well under a second (a birthday attack needs ~2^32
 *      work, and a targeted second-preimage on this construction is far cheaper
 *      than that in practice because the tail is unfinalised). It catches
 *      ACCIDENTS — a torn write, a truncated file, a bad edit by a script — and
 *      it catches a careless editor. It does not catch an adversary who reads
 *      this header. sha256 replaces it when a verified implementation exists in
 *      this tree (libcrypto is not in an initramfs, and a hand-rolled sha256 that
 *      nobody has tested against vectors is worse than an honest fnv1a).
 *
 *   2. THE CHAIN CAN BE REWRITTEN WHOLESALE. Verification recomputes from the
 *      first record and never consults anything outside the file. So an attacker
 *      who can write the whole file can produce a consistent chain of their
 *      choosing. The defence against that is a value committed somewhere else:
 *      the next boot's log entry, the supervisor's memory, a signature by the
 *      host. None of those exist yet. Until they do, this detects partial
 *      tampering — the realistic case of one record being edited to hide one
 *      action — and not a wholesale forgery.
 *
 *   3. IT IS WRITTEN BY THE PROCESS BEING AUDITED. `audit_append()` is called
 *      from tools.c, inside the tool registry, which is the AI-facing path. Policy
 *      already denies writes to audit.log (tier 0), so the AI cannot edit the
 *      file through a tool; but the component that logs is still the component
 *      being logged. Moving the write to PID 1's supervisor is M7's work and is
 *      the only thing that makes "the record cannot be suppressed" true.
 *
 *   4. NOTHING ROTATES. The file grows without bound. On an 8 MiB data disk that
 *      is a real ceiling; rotation + an archival hash is a later milestone, and
 *      pretending otherwise would mean a log that silently stops recording.
 *
 * What IS true, and tested:
 *   - every appended record is chained to its predecessor;
 *   - one changed byte is detected, and the first bad seq is reported;
 *   - a truncated tail is reported as a TORN TAIL, not as tampering, because a
 *     crash is not an attack and crying wolf trains people to ignore the alarm;
 *   - a deleted middle or first record is detected (seq must be continuous);
 *   - two writers cannot interleave half-lines (flock, and an explicit EBUSY).
 */
#ifndef AIOS_AUDIT_H
#define AIOS_AUDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aios/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIOS_AUDIT_FILE      "audit.log"
#define AIOS_AUDIT_HEX       16          /* fnv1a64 as %016llx */
#define AIOS_AUDIT_LINE_MAX  8192        /* one record; refuses longer, never truncates */
#define AIOS_AUDIT_RECORDS_MAX 100000    /* verify is bounded; see audit_verify */

/* How a record came about. Kept separate from `origin` (who asked): a human can
 * type something that is recorded as a tool call, and an agent's decision can be
 * recorded without any tool running. */
typedef enum {
    AUDIT_KIND_TOOL = 0,     /* a tool executed (sandboxed or native)       */
    AUDIT_KIND_DECISION,     /* a proposal was judged, with no execution    */
    AUDIT_KIND_EVENT,        /* anything else worth remembering             */
    AUDIT_KIND__COUNT
} audit_kind;

typedef enum {
    AUDIT_OK = 0,
    AUDIT_BAD_SEQ,           /* a record was removed or reordered           */
    AUDIT_BAD_HASH,          /* the record does not match its own hash       */
    AUDIT_BAD_LINK,          /* `prev` does not match the previous `hash`    */
    AUDIT_BAD_JSON,          /* a complete line that is not a JSON object    */
    AUDIT_TORN_TAIL,         /* the last line has no newline: a crash, not an attack */
    AUDIT_TOO_MANY,          /* more records than the caller's budget        */
    AUDIT_UNREADABLE,        /* the file could not be read at all            */
    AUDIT_REASON__COUNT
} audit_verdict;

typedef struct {
    audit_verdict v;
    long long     seq;              /* first offending record, or -1          */
    long long     records;          /* complete records seen before stopping  */
    long long     torn_bytes;       /* bytes in an unterminated final line    */
    char          why[192];
} audit_check;

const char *audit_verdict_str(audit_verdict v);

/* Append one record, chained to the tail of the existing log.
 *
 * `body` is a JSON object WITHOUT the chain fields; the chain fields (`seq`,
 * `prev`, and `hash`) are computed and added here, so a caller cannot forge them.
 * If `body` already contains `hash` or `prev` they are overwritten — a record
 * that supplies its own hash is exactly the thing this exists to prevent.
 *
 * Returns false and fills `err` on any failure, including when the log could not
 * be read (we do NOT guess the predecessor; a guessed `prev` would produce a
 * chain that verifies but is not one).
 *
 * Single writer by contract: the append holds an exclusive flock for its
 * duration. A second writer gets EWOULDBLOCK and an explicit "busy", never a
 * half-written line.
 */
bool audit_append(const char *store, audit_kind kind, const char *body_json,
                  long long *seq_out, char *err, size_t ecap);

/* A convenience for the tool path: the fields M4 wrote, plus the chain. */
bool audit_append_tool(const char *store, const char *tool, const char *args_json,
                       const char *origin, bool ok, double ms,
                       const char *result_json, char *err, size_t ecap);

/* Walk the chain from the first record and report the first problem.
 *
 * `max_records` bounds the work: 0 means AIOS_AUDIT_RECORDS_MAX. A caller that
 * does not bound this is asking to walk an 8 MiB file on a machine with no
 * swap, so the bound is explicit rather than hidden.
 *
 * Never returns a verdict of "fine" for a file it could not read. A missing
 * audit log is TORN_TAIL with why="absent", NOT ok: "there is no evidence" and
 * "the evidence is good" are different answers, and this milestone exists to
 * keep them apart.
 */
audit_check audit_verify(const char *store, long long max_records);

/* Count complete records only, for `dur`. A torn tail is not a record. */
size_t audit_count(const char *store);

/* The last `hash`, or "0" when the log is empty — the anchor a next session
 * could commit to. Exposed so a future milestone can pin it somewhere else
 * instead of having to re-read and re-parse the whole file. */
bool audit_head(const char *store, char out[AIOS_AUDIT_HEX + 1]);

#ifdef __cplusplus
}
#endif

#endif /* AIOS_AUDIT_H */
