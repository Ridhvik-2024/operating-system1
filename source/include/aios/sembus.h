/* aios/sembus.h — M3: semantic IPC bus.
 * In-process broker: publishers emit JSON payloads on topics; subscribers
 * get callbacks by exact topic or wildcard "*". Dispatch is synchronous;
 * contract: use within one thread (or guard externally). */
#ifndef AIOS_SEMBUS_H
#define AIOS_SEMBUS_H

#include "aios/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sembus sembus;

typedef void (*sembus_fn)(const char *topic, const char *json_payload,
                          size_t payload_len, void *ud);

sembus *sembus_create(void);

/* topic "*" receives everything. Returns AIOS_OK or AIOS_ERR_NOMEM. */
aios_status sembus_sub(sembus *b, const char *topic, sembus_fn fn, void *ud);

/* Publish: copies payload, dispatches to all matching subscribers.
 * Returns number of subscribers reached, or AIOS_ERR on bad args. */
int sembus_pub(sembus *b, const char *topic, const char *json_payload);

/* Convenience: publish a JSON object built from key/str pairs
 * (all values are strings). */
int sembus_pub_kv(sembus *b, const char *topic, const char *const *kv);

size_t sembus_topics(sembus *b); /* distinct topics that have subs */

void sembus_destroy(sembus *b);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_SEMBUS_H */
