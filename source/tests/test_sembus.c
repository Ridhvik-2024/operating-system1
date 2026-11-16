/* tests/test_sembus.c — M3 semantic IPC bus. */
#include "t.h"
#include "aios/sembus.h"

static char g_last[256];
static int g_calls = 0;

static void recv(const char *topic, const char *payload, size_t len, void *ud) {
    (void)len;
    snprintf(g_last, sizeof g_last, "%s|%s|%s", topic, payload, (const char *)ud);
    g_calls++;
}

static void run(void) {
    sembus *b = sembus_create();
    CHECK(b != NULL, "create");

    CHECK(sembus_sub(b, "proc.spawn", recv, (void *)"A") == AIOS_OK, "sub A");
    CHECK(sembus_sub(b, "*", recv, (void *)"W") == AIOS_OK, "sub wildcard");

    int n = sembus_pub(b, "proc.spawn", "{\"intent\":\"build m4\"}");
    CHECK(n == 2, "exact + wildcard both reached");
    CHECK(g_calls == 2, "two callbacks");
    CHECK_STR_CONTAINS(g_last, "proc.spawn|{\"intent\":\"build m4\"}|", "payload+topic");

    n = sembus_pub(b, "mem.consolidated", "{\"n\":3}");
    CHECK(n == 1, "only wildcard reached");
    CHECK(g_calls == 3, "third callback");

    /* with a wildcard sub present, every publish reaches it */
    n = sembus_pub(b, "no.exact.subscribers", "{}");
    CHECK(n == 1, "wildcard still reached");

    /* fresh bus with only an exact-topic sub: other topics reach zero */
    sembus *b2 = sembus_create();
    CHECK(sembus_sub(b2, "only.topic", recv, (void *)"E") == AIOS_OK, "sub E");
    n = sembus_pub(b2, "different.topic", "{}");
    CHECK(n == 0, "no matching subs -> zero reached");
    sembus_destroy(b2);

    n = sembus_pub_kv(b, "agent.done",
                      (const char *const[]){"agent", "architect", "result", "ok", NULL});
    CHECK(n == 1, "pub_kv reached wildcard");
    CHECK_STR_CONTAINS(g_last, "\"agent\":\"architect\"", "kv json built");

    CHECK(sembus_pub(b, "x", NULL) == AIOS_ERR, "null payload rejected");
    CHECK(sembus_sub(NULL, "t", recv, NULL) == AIOS_ERR_ARG, "null bus rejected");

    sembus_destroy(b);
}

TEST_MAIN()
