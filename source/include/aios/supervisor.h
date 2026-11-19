/* aios/supervisor.h — M5: PID 1 owns snapshot and rollback.
 *
 * The milestone's goal is not "rollback exists". It is "rollback exists, is
 * available always, and is owned by PID 1 -- not by the component that takes AI
 * input." A shell that can restore itself is a shell an agent can talk into
 * restoring; the power has to live somewhere the agent's input never reaches.
 *
 * So the snapshot verbs are served over a unix socket by the process that
 * supervises everything else. `aiosh` asks; it does not hold the power.
 *
 *   WHO MAY ASK
 *   The socket is created 0600 and its directory 0700 under the system state
 *   directory, and every connection is checked with SO_PEERCRED before a single
 *   byte is read: the peer's uid must be the uid that created the socket.
 *   Today every process on this OS runs as the same uid, so that check does not
 *   distinguish an agent from a human -- SAYING SO IS THE POINT. The check exists
 *   because it is the right shape, and it becomes a real boundary when agents
 *   get their own uid (Phase 3). Until then this is an OWNERSHIP boundary, not
 *   an authorization one: the code that can roll back is not the code that takes
 *   AI input, so an agent that talks the shell into anything still cannot reach
 *   the restore path except by asking the shell, and the shell's request is one
 *   the human typed.
 *
 *   Protocol: one JSON object per line, one response object per line, then the
 *   connection closes.
 *
 *     -> {"op":"ping"}
 *     <- {"ok":true,"pid":1,"uid":0,"snapshots":3,
 *         "store":"/data/aios/store","jail":"/workspace"}
 *
 *   ping reports the trees it serves because a client must be able to refuse a
 *   supervisor that is serving someone else's. In the image PID 1 serves
 *   /workspace; the test suite runs its shell against a scratch jail and store,
 *   and asking PID 1 to snapshot those would put the snapshot in the wrong
 *   place -- which is exactly what happened the first time this shipped.
 *
 *     -> {"op":"snap","label":"before an agent run"}
 *     <- {"ok":true,"id":"1794...-676-0"}
 *
 *     -> {"op":"list"}
 *     <- {"ok":true,"n":3,"snapshots":[{"id":..,"label":..,"taken_ms":..,
 *                                       "files":..,"bytes":..}, ...]}
 *
 *     -> {"op":"restore","id":"latest"}
 *     <- {"ok":true,"id":"1794...","replaced":".replaced1794..."}
 *
 *     -> {"op":"kill","job":7}
 *     <- {"ok":true,"killed":true}
 *
 *   Failure is always {"ok":false,"error":"..."} with a reason a person can act
 *   on. An unknown op is a failure, not a silent success: a typo must not look
 *   like a completed rollback.
 *
 *   The socket path is fixed, because a client that has to be told where the
 *   supervisor lives could be told the wrong place:
 *
 *       /run/aios/supervisor.sock
 */
#ifndef AIOS_SUPERVISOR_H
#define AIOS_SUPERVISOR_H

#include <stdbool.h>
#include <stddef.h>

#define AIOS_SUP_DIR       "/run/aios"
#define AIOS_SUP_SOCK      AIOS_SUP_DIR "/supervisor.sock"
/* A request is a single line of JSON with a label in it; 4 KiB is generous and
 * small enough that a hostile peer cannot make us allocate. */
#define AIOS_SUP_LINE_MAX  4096
#define AIOS_SUP_MAX_SNAPS 32

/* Where the supervisor's store lives when the caller does not say. Follows the
 * same rule as the rest of the OS: the durable store if there is one. */
const char *sup_default_store(void);

/* Serve forever on `sock_path`, taking snapshots of (`jail`, `store`).
 *
 * `max_requests` exists for tests: 0 means serve until killed (what PID 1 does),
 * any positive number means answer that many requests then return. A supervisor
 * test that has to kill a server to finish is a test that leaks a process when it
 * fails, and a leaked server on a fixed socket path makes the NEXT test fail for
 * a reason that has nothing to do with it.
 *
 * Returns 0 on a clean stop, -1 on a setup failure (with `err` filled).
 */
int sup_serve(const char *sock_path, const char *jail, const char *store,
              int max_requests, char *err, size_t ecap);

/* Answer one request line, writing one response line into `out`.
 * Exposed so the protocol can be tested without a socket, which is how the
 * error cases (unknown op, bad JSON, missing field) get tested at all. */
bool sup_handle_line(const char *req, const char *jail, const char *store,
                     char *out, size_t ocap, int *exit_after);

#endif /* AIOS_SUPERVISOR_H */
