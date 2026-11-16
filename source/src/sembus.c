/* aios/sembus.c — M3 implementation: in-process semantic message bus. */
#include "aios/sembus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sub {
    char *topic;
    sembus_fn fn;
    void *ud;
    struct sub *next;
} sub;

struct sembus {
    sub *head;
    size_t nsubs;
};

sembus *sembus_create(void) {
    return (sembus *)calloc(1, sizeof(sembus));
}

aios_status sembus_sub(sembus *b, const char *topic, sembus_fn fn, void *ud) {
    if (!b || !topic || !fn) return AIOS_ERR_ARG;
    sub *s = (sub *)calloc(1, sizeof(sub));
    if (!s) return AIOS_ERR_NOMEM;
    s->topic = strdup(topic);
    if (!s->topic) { free(s); return AIOS_ERR_NOMEM; }
    s->fn = fn;
    s->ud = ud;
    s->next = b->head;
    b->head = s;
    b->nsubs++;
    return AIOS_OK;
}

int sembus_pub(sembus *b, const char *topic, const char *json_payload) {
    if (!b || !topic || !json_payload) return AIOS_ERR;
    size_t plen = strlen(json_payload);
    int reached = 0;
    for (sub *s = b->head; s; s = s->next) {
        if (strcmp(s->topic, topic) == 0 || strcmp(s->topic, "*") == 0) {
            s->fn(topic, json_payload, plen, s->ud);
            reached++;
        }
    }
    return reached;
}

int sembus_pub_kv(sembus *b, const char *topic, const char *const *kv) {
    if (!b || !topic || !kv) return AIOS_ERR;
    /* build {"k":"v",...} with proper escaping for the common case */
    size_t cap = 64;
    for (const char *const *p = kv; *p && *(p + 1); p += 2)
        cap += strlen(*p) + strlen(*(p + 1)) + 8;
    char *buf = (char *)malloc(cap);
    if (!buf) return AIOS_ERR;
    size_t n = 0;
    buf[n++] = '{';
    for (const char *const *p = kv; *p && *(p + 1); p += 2) {
        if (n > 1) buf[n++] = ',';
        /* simple JSON string escape */
        n += (size_t)snprintf(buf + n, cap - n, "\"%s\":\"", *p);
        for (const char *q = *(p + 1); *q && n < cap - 8; q++) {
            if (*q == '"' || *q == '\\') buf[n++] = '\\';
            buf[n++] = *q;
        }
        buf[n++] = '"';
    }
    buf[n++] = '}';
    buf[n] = 0;
    int rc = sembus_pub(b, topic, buf);
    free(buf);
    return rc;
}

size_t sembus_topics(sembus *b) {
    if (!b) return 0;
    size_t n = 0;
    for (sub *s = b->head; s; s = s->next) {
        if (strcmp(s->topic, "*") == 0) continue;
        bool seen_before = false;
        for (sub *t = b->head; t != s; t = t->next)
            if (strcmp(t->topic, s->topic) == 0) { seen_before = true; break; }
        if (!seen_before) n++;
    }
    return n;
}

void sembus_destroy(sembus *b) {
    if (!b) return;
    sub *s = b->head;
    while (s) {
        sub *nx = s->next;
        free(s->topic);
        free(s);
        s = nx;
    }
    free(b);
}
