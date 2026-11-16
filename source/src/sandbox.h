/* sandbox.h — internal: run a job inside the tool-exec worker process. */
#ifndef AIOS_SANDBOX_H
#define AIOS_SANDBOX_H

#include "aios/types.h"
#include "aios/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fork+exec the tool-exec worker (resolved from AIOS_TOOL_EXEC env,
 * the binary directory, or CWD) with `jail` as its argv[1], feed
 * `request_json` on stdin, read the JSON response from stdout with a
 * hard wall-clock deadline (SIGTERM then SIGKILL). On success
 * *result_out = {"ok":true,"result":...} or {"ok":false,"error":"..."}
 * (worker-level outcome). */
aios_status sandbox_run(const char *jail, const char *request_json,
                        size_t req_len, double timeout_s,
                        json_val **result_out, char *err, size_t ecap);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_SANDBOX_H */
