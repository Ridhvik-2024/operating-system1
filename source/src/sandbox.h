/* sandbox.h — internal: run a job inside the tool-exec worker process. */
#ifndef AIOS_SANDBOX_H
#define AIOS_SANDBOX_H

#include "aios/types.h"
#include "aios/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* M4: a worker's response, with the account it kept of its own actions.
 *
 * `result` is the tool's return value; `resp` is the whole envelope, which is
 * where `effects` / `complete` / `usage` live. The caller must free the node it
 * wants and sandbox_result_free() the rest — keeping the envelope is what makes
 * "the worker's own account" available at the point where the decision to trust
 * it is made, instead of being flattened into a result the caller cannot audit. */
typedef struct {
    json_val    *resp;      /* whole envelope, owned                          */
    json_val    *effects;   /* borrowed: array inside resp                    */
    json_val    *complete;  /* borrowed: bool inside resp                     */
    json_val    *usage;     /* borrowed: object inside resp                   */
} sandbox_result;

void sandbox_result_free(sandbox_result *r);

/* Fork+exec the tool-exec worker (resolved from AIOS_TOOL_EXEC env,
 * the binary directory, or CWD) with `jail` as its argv[1], feed
 * `request_json` on stdin, read the JSON response from stdout with a
 * hard wall-clock deadline (SIGTERM then SIGKILL). On success
 * *result_out = {"ok":true,"result":...} or {"ok":false,"error":"..."}
 * (worker-level outcome). */
aios_status sandbox_run(const char *jail, const char *request_json,
                        size_t req_len, double timeout_s,
                        json_val **result_out, char *err, size_t ecap);

/* The same run, keeping the envelope. Prefer this one: the effects document is
 * part of the answer, not a debugging extra. */
aios_status sandbox_run_ex(const char *jail, const char *request_json,
                           size_t req_len, double timeout_s,
                           sandbox_result *out, char *err, size_t ecap);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_SANDBOX_H */
