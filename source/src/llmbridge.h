/* llmbridge.h — internal shared z-ai chat bridge (M4 cortex + M5 router).
 * Not installed with the public headers: runtime-internal API. */
#ifndef AIOS_LLMBRIDGE_H
#define AIOS_LLMBRIDGE_H

#include "aios/types.h"

#include <stddef.h>

/* Run `z-ai chat -s SYSTEM -p PROMPT -o out.json` and return the model
 * message content (malloc'd, caller frees). Retries transient failures
 * with backoff (429-aware). Same bridge the M4 cortex uses. */
aios_status aios_llm_call(const char *system, const char *prompt,
                          char **content_out, char *err, size_t ecap);

/* Extract the first '{' .. last '}' span of s as a malloc'd string.
 * Returns NULL when s contains no JSON object. Caller frees. */
char *aios_llm_extract_json(const char *s);

#endif /* AIOS_LLMBRIDGE_H */
