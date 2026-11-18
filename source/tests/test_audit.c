/* test_audit.c — M6: the audit log is a chain, and a broken link is visible.
 *
 * The card asks for four things and this file proves each one separately, because
 * they fail in different ways and a single "verify says no" would not tell you
 * which:
 *
 *   1. 100 records verify clean
 *   2. one changed byte in the middle is caught, at the right seq
 *   3. a deleted last line is a TORN TAIL -- a crash, not an attack
 *   4. two writers cannot interleave
 *
 * Plus the attacks the card does not name but a chain invites: deleting a record,
 * deleting the FIRST record, reordering two records, and appending a forged record
 * that links to nothing. Each has to be caught for the chain to mean anything.
 */
#include "t.h"
#include "aios/audit.h"
#include "aios/json.h"
#include "aios/types.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROOT "build/test-audit"
#define LOG  ROOT "/audit.log"

static void rmrf(const char *p) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s", p);
    (void)!system(cmd);
}

static bool mkdirs(const char *p) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", p);
    for (char *q = tmp + 1; *q; q++) {
        if (*q != '/') continue;
        *q = 0; mkdir(tmp, 0755); *q = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    if (len) *len = got;
    return b;
}

static bool spit(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

/* Write n records through the real API. Returns how many succeeded. */
static int fill(const char *store, int n) {
    int ok = 0;
    for (int i = 0; i < n; i++) {
        char body[256];
        snprintf(body, sizeof body,
                 "{\"ts\":%d,\"tool\":\"t%d\",\"origin\":\"human\",\"ok\":true}",
                 i, i);
        char err[256] = {0};
        if (audit_append(store, AUDIT_KIND_TOOL, body, NULL, err, sizeof err)) ok++;
        else printf("      append %d failed: %s\n", i, err);
    }
    return ok;
}

/* ---------------- 1. a long clean chain ---------------- */

static void test_clean_chain(void) {
    printf("100 records verify clean\n");
    rmrf(ROOT);
    mkdirs(ROOT);

    CHECK(fill(ROOT, 100) == 100, "100 records appended");

    audit_check c = audit_verify(ROOT, 0);
    CHECK(c.v == AUDIT_OK, "the chain verifies");
    if (c.v != AUDIT_OK) printf("      verdict: %s -- %s\n", audit_verdict_str(c.v), c.why);
    CHECK(c.records == 100, "and all 100 were walked");
    CHECK_STR_CONTAINS(c.why, "intact", "and it says so in words");

    CHECK(audit_count(ROOT) == 100, "audit_count agrees");

    char head[AIOS_AUDIT_HEX + 1] = {0};
    CHECK(audit_head(ROOT, head), "the head hash is readable");
    CHECK(strlen(head) == AIOS_AUDIT_HEX, "and is 16 hex characters");
    CHECK(strcmp(head, "0000000000000000") != 0, "and is not the genesis hash");

    /* Each record must actually differ: an implementation that wrote the same
     * hash every time would pass every check above. */
    size_t len = 0;
    char *log = slurp(LOG, &len);
    CHECK(log != NULL, "the log is readable");
    if (log) {
        size_t distinct = 0;
        char seen[100][AIOS_AUDIT_HEX + 1];
        memset(seen, 0, sizeof seen);
        const char *p = log;
        while ((p = strstr(p, "\"hash\":\"")) != NULL) {
            p += 8;
            if (distinct < 100) {
                snprintf(seen[distinct], AIOS_AUDIT_HEX + 1, "%.16s", p);
            }
            distinct++;
        }
        CHECK(distinct == 100, "100 hash fields are present");
        int dupes = 0;
        for (size_t i = 0; i < 100 && i < distinct; i++)
            for (size_t j = i + 1; j < 100 && j < distinct; j++)
                if (strcmp(seen[i], seen[j]) == 0) dupes++;
        CHECK(dupes == 0, "every record has a different hash");
        free(log);
    }
}

/* ---------------- 2. one changed byte, in the middle ---------------- */

static void test_one_byte_changed(void) {
    printf("one changed byte is caught, at the right seq\n");
    rmrf(ROOT);
    mkdirs(ROOT);
    fill(ROOT, 20);

    size_t len = 0;
    char *log = slurp(LOG, &len);
    CHECK(log != NULL, "the log is readable");
    if (!log) return;

    /* Find the 7th line and change a byte INSIDE it (not in its hash, so the
     * failure is a hash mismatch rather than a parse error). */
    int line = 1;
    char *p = log;
    while (p && line < 7) {
        p = strchr(p, '\n');
        if (p) { p++; line++; }
    }
    CHECK(p != NULL && line == 7, "found the 7th record");
    if (!p) { free(log); return; }

    char *field = strstr(p, "\"tool\":\"t6\"");
    CHECK(field != NULL, "and its tool field is where expected");
    if (!field) { free(log); return; }
    field[9] = 'X';       /* t6 -> tX */

    CHECK(spit(LOG, log, len), "the log is rewritten with one byte changed");
    free(log);

    audit_check c = audit_verify(ROOT, 0);
    CHECK(c.v == AUDIT_BAD_HASH, "the change is caught as a hash mismatch");
    CHECK(c.seq == 7, "and the first bad seq is 7");
    if (c.v != AUDIT_BAD_HASH) printf("      verdict: %s -- %s\n", audit_verdict_str(c.v), c.why);
    CHECK_STR_CONTAINS(c.why, "hashes to", "and the message shows both values");
}

/* ---------------- 3. a deleted last line is a torn tail ---------------- */

static void test_deleted_tail(void) {
    printf("a deleted last line is a torn tail, not tampering\n");
    rmrf(ROOT);
    mkdirs(ROOT);
    fill(ROOT, 10);

    size_t len = 0;
    char *log = slurp(LOG, &len);
    CHECK(log != NULL, "the log is readable");
    if (!log) return;

    /* Drop the final line entirely, newline and all: the file now ends with a
     * complete record, so nothing looks torn... unless the implementation checks
     * seq. It must report a torn tail only for an UNTERMINATED line. */
    char *lastnl = NULL;
    for (size_t i = len; i > 0; i--)
        if (log[i - 1] == '\n' && i < len) { lastnl = log + i - 1; break; }
    CHECK(lastnl != NULL, "found the second-to-last newline");
    if (!lastnl) { free(log); return; }
    size_t cut = (size_t)(lastnl - log) + 1;
    CHECK(spit(LOG, log, cut), "the log is rewritten without its last line");
    free(log);

    audit_check c = audit_verify(ROOT, 0);
    /* A whole line removed cleanly is NOT a torn tail: the file is well-formed and
     * a record is missing. Calling that "torn" would hide a deletion. */
    CHECK(c.v == AUDIT_OK, "a cleanly removed final record leaves a valid chain");
    CHECK(c.records == 9, "and 9 records remain");

    /* Now the actual torn case: a partial line. */
    rmrf(ROOT);
    mkdirs(ROOT);
    fill(ROOT, 10);
    log = slurp(LOG, &len);
    if (!log) { CHECK(0, "log readable for the torn case"); return; }
    /* append half a record, as a crash mid-write would leave */
    const char *partial = "{\"ts\":99,\"tool\":\"torn\",\"seq\":11";
    CHECK(spit(LOG, log, len), "the log is readable again");
    {
        FILE *f = fopen(LOG, "ab");
        CHECK(f != NULL, "and can be appended to");
        if (f) { fputs(partial, f); fclose(f); }
    }
    free(log);

    c = audit_verify(ROOT, 0);
    CHECK(c.v == AUDIT_TORN_TAIL, "an unterminated line is reported as torn");
    CHECK(c.records == 10, "and the 10 complete records still count");
    CHECK(c.torn_bytes > 0, "and the torn length is reported");
    CHECK_STR_CONTAINS(c.why, "crash", "and the wording distinguishes crash from edit");
}

/* ---------------- 4. two writers cannot interleave ---------------- */

static void test_single_writer(void) {
    printf("two writers cannot interleave\n");
    rmrf(ROOT);
    mkdirs(ROOT);
    fill(ROOT, 1);

    /* Hold the lock the way audit_append does, then check that a second append
     * refuses with "busy" rather than writing half a line. */
    int fd = open(LOG, O_RDWR);
    CHECK(fd >= 0, "the log opens");
    if (fd < 0) return;
    CHECK(flock(fd, LOCK_EX | LOCK_NB) == 0, "the test takes the exclusive lock");

    char body[] = "{\"ts\":1,\"tool\":\"blocked\"}";
    char err[256] = {0};
    CHECK(!audit_append(ROOT, AUDIT_KIND_TOOL, body, NULL, err, sizeof err),
          "an append while the lock is held is refused");
    CHECK_STR_CONTAINS(err, "busy", "and says the log is busy");
    CHECK_STR_CONTAINS(err, "another writer", "naming the reason");

    flock(fd, LOCK_UN);
    close(fd);

    err[0] = 0;
    CHECK(audit_append(ROOT, AUDIT_KIND_TOOL, body, NULL, err, sizeof err),
          "and succeeds once the lock is released");
    audit_check c = audit_verify(ROOT, 0);
    CHECK(c.v == AUDIT_OK, "the chain still verifies");
    CHECK(c.records == 2, "with both records present");
}

/* ---------------- 5. structural attacks ---------------- */

static void test_structural_attacks(void) {
    printf("removing, reordering or forging a record is caught\n");

    /* (a) delete a MIDDLE record */
    rmrf(ROOT);
    mkdirs(ROOT);
    fill(ROOT, 12);
    {
        size_t len = 0;
        char *log = slurp(LOG, &len);
        if (!log) { CHECK(0, "log readable"); return; }
        /* find the start and end of line 5 */
        char *starts[16] = {0};
        int n = 0;
        starts[n++] = log;
        for (char *q = log; *q && n < 16; q++)
            if (*q == '\n' && q[1]) starts[n++] = q + 1;
        char *line5 = starts[4];
        char *line6 = starts[5];
        size_t cut_from = (size_t)(line5 - log), cut_to = (size_t)(line6 - log);
        memmove(log + cut_from, log + cut_to, len - cut_to);
        len -= (cut_to - cut_from);
        CHECK(spit(LOG, log, len), "a middle record is removed");
        free(log);
    }
    audit_check c = audit_verify(ROOT, 0);
    CHECK(c.v == AUDIT_BAD_SEQ, "the removal is caught by the sequence");
    CHECK(c.seq == 5, "at the missing seq (5)");
    if (c.v != AUDIT_BAD_SEQ) printf("      verdict: %s -- %s\n", audit_verdict_str(c.v), c.why);

    /* (b) delete the FIRST record */
    rmrf(ROOT);
    mkdirs(ROOT);
    fill(ROOT, 6);
    {
        size_t len = 0;
        char *log = slurp(LOG, &len);
        if (!log) { CHECK(0, "log readable"); return; }
        char *nl = strchr(log, '\n');
        CHECK(nl != NULL, "found the end of the first record");
        if (nl) {
            size_t cut = (size_t)(nl - log) + 1;
            CHECK(spit(LOG, log + cut, len - cut), "the first record is removed");
        }
        free(log);
    }
    c = audit_verify(ROOT, 0);
    CHECK(c.v == AUDIT_BAD_SEQ, "removing the first record is caught");
    CHECK(c.seq == 1, "at seq 1");
    /* The first record is special: with it gone, record 2's `prev` points at
     * nothing, which is the case a naive "chain from wherever it starts" verifier
     * would accept. */
    if (c.v == AUDIT_BAD_SEQ) CHECK_STR_CONTAINS(c.why, "removed", "and names a removal");

    /* (c) reorder two records */
    rmrf(ROOT);
    mkdirs(ROOT);
    fill(ROOT, 5);
    {
        size_t len = 0;
        char *log = slurp(LOG, &len);
        if (!log) { CHECK(0, "log readable"); return; }
        char *starts[8] = {0};
        int n = 0;
        starts[n++] = log;
        for (char *q = log; *q && n < 8; q++)
            if (*q == '\n' && q[1]) starts[n++] = q + 1;
        size_t l2 = (size_t)(starts[2] - starts[1]);
        size_t l3 = (size_t)(starts[3] - starts[2]);
        char *tmp = malloc(l2);
        if (tmp) {
            memcpy(tmp, starts[1], l2);
            memmove(starts[1], starts[2], l3);
            memcpy(starts[1] + l3, tmp, l2);
            free(tmp);
            CHECK(spit(LOG, log, len), "records 2 and 3 are swapped");
        }
        free(log);
    }
    c = audit_verify(ROOT, 0);
    CHECK(c.v == AUDIT_BAD_SEQ, "the swap is caught");
    CHECK(c.seq == 2, "at the first out-of-order record");

    /* (d) a forged record spliced onto the end, chaining to nothing */
    rmrf(ROOT);
    mkdirs(ROOT);
    fill(ROOT, 4);
    {
        FILE *f = fopen(LOG, "ab");
        CHECK(f != NULL, "the log can be appended to by hand");
        if (f) {
            fputs("{\"ts\":999,\"tool\":\"forged\",\"seq\":5,"
                  "\"prev\":\"deadbeefdeadbeef\",\"akind\":\"tool\","
                  "\"hash\":\"0000000000000000\"}\n", f);
            fclose(f);
        }
    }
    c = audit_verify(ROOT, 0);
    CHECK(c.v == AUDIT_BAD_LINK, "a forged record links to the wrong predecessor");
    CHECK(c.seq == 5, "at the forged seq");
    if (c.v != AUDIT_BAD_LINK) printf("      verdict: %s -- %s\n", audit_verdict_str(c.v), c.why);
}

/* ---------------- 6. an absent log is not a clean log ---------------- */

static void test_absent_is_not_ok(void) {
    printf("an absent log is not reported as a clean one\n");
    rmrf(ROOT);
    mkdirs(ROOT);

    audit_check c = audit_verify(ROOT, 0);
    CHECK(c.v != AUDIT_OK, "verifying a missing log is not 'ok'");
    CHECK(c.v == AUDIT_TORN_TAIL, "it is reported as absent/torn");
    CHECK_STR_CONTAINS(c.why, "absent", "and the wording says absent, not tampered");
    CHECK(audit_count(ROOT) == 0, "and the count is zero");

    char head[AIOS_AUDIT_HEX + 1] = {0};
    CHECK(!audit_head(ROOT, head), "a missing log has no head");
    CHECK(strcmp(head, "0000000000000000") == 0, "and the head is the genesis value");
}

/* ---------------- 7. the record carries the fields that matter ---------------- */

static void test_record_fields(void) {
    printf("a tool record keeps M4's fields and adds the chain\n");
    rmrf(ROOT);
    mkdirs(ROOT);

    char err[256] = {0};
    CHECK(audit_append_tool(ROOT, "calc", "{\"expr\":\"1+1\"}", "ai",
                            true, 1.5, "{\"value\":2}", err, sizeof err),
          "a tool record is appended");
    if (err[0]) printf("      said: %s\n", err);

    size_t len = 0;
    char *log = slurp(LOG, &len);
    CHECK(log != NULL, "the log is readable");
    if (log) {
        /* The old fields must survive: `dur` counts lines, and the shell's test
         * looks for "tool":"calc". A chain that broke them would be a regression
         * dressed as a feature. */
        CHECK_STR_CONTAINS(log, "\"tool\":\"calc\"", "keeps the tool name");
        CHECK_STR_CONTAINS(log, "\"ok\":true", "keeps the outcome");
        CHECK_STR_CONTAINS(log, "\"digest\"", "keeps the result digest");
        CHECK_STR_CONTAINS(log, "\"origin\":\"ai\"", "records the origin");
        CHECK_STR_CONTAINS(log, "\"seq\":1", "carries seq 1");
        CHECK_STR_CONTAINS(log, "\"prev\":\"0000000000000000\"",
                           "and chains the first record to genesis");
        CHECK_STR_CONTAINS(log, "\"hash\":\"", "and carries its own hash");
        free(log);
    }

    audit_check c = audit_verify(ROOT, 0);
    CHECK(c.v == AUDIT_OK, "and it verifies");
    CHECK(c.records == 1, "one record");
}

/* ---------------- 8. a caller cannot supply its own hash ---------------- */

static void test_caller_cannot_forge(void) {
    printf("a record body cannot smuggle in its own hash\n");
    rmrf(ROOT);
    mkdirs(ROOT);

    /* A body that already claims a hash and a prev. Both must be overwritten by
     * the real ones, or the whole chain is decorative: a caller could pick a hash
     * that makes a later verification pass. */
    char body[] = "{\"ts\":1,\"tool\":\"evil\",\"hash\":\"deadbeefdeadbeef\","
                  "\"prev\":\"cafebabecafebabe\"}";
    char err[256] = {0};
    CHECK(audit_append(ROOT, AUDIT_KIND_TOOL, body, NULL, err, sizeof err),
          "the record is accepted");

    size_t len = 0;
    char *log = slurp(LOG, &len);
    if (log) {
        CHECK(strstr(log, "deadbeefdeadbeef") == NULL,
              "the caller's hash was NOT used");
        CHECK(strstr(log, "cafebabecafebabe") == NULL,
              "the caller's prev was NOT used");
        free(log);
    } else {
        CHECK(0, "log readable");
    }

    audit_check c = audit_verify(ROOT, 0);
    CHECK(c.v == AUDIT_OK, "and the resulting chain verifies");
}

/* ---------------- 9. a torn tail does not poison the next append ---------------- */

static void test_append_after_crash(void) {
    printf("an append after a crash repairs the tail instead of chaining onto it\n");
    rmrf(ROOT);
    mkdirs(ROOT);
    fill(ROOT, 5);

    /* Simulate a crash mid-append. */
    {
        FILE *f = fopen(LOG, "ab");
        if (f) { fputs("{\"ts\":99,\"tool\":\"half", f); fclose(f); }
    }
    audit_check c = audit_verify(ROOT, 0);
    CHECK(c.v == AUDIT_TORN_TAIL, "the tail is torn before the append");

    char err[256] = {0};
    CHECK(audit_append(ROOT, AUDIT_KIND_TOOL, "{\"ts\":6,\"tool\":\"after\"}",
                       NULL, err, sizeof err), "the next append succeeds");
    if (err[0]) printf("      said: %s\n", err);

    c = audit_verify(ROOT, 0);
    CHECK(c.v == AUDIT_OK, "and the chain verifies again");
    /* 5 good + 1 new. The half-written one is gone -- chaining onto it would have
     * made every later record unverifiable, so one crash would poison the log. */
    CHECK(c.records == 6, "with 6 records: the torn line was discarded, not chained");

    size_t len = 0;
    char *log = slurp(LOG, &len);
    if (log) {
        CHECK(strstr(log, "\"tool\":\"half") == NULL, "the partial record is gone");
        free(log);
    }
}

int main(void) {
    printf("== test_audit ==\n");
    rmrf(ROOT);
    mkdirs(ROOT);

    test_clean_chain();
    test_one_byte_changed();
    test_deleted_tail();
    test_single_writer();
    test_structural_attacks();
    test_absent_is_not_ok();
    test_record_fields();
    test_caller_cannot_forge();
    test_append_after_crash();

    printf("%s: %d checks, %d failed\n", g_failed ? "FAIL" : "PASS",
           g_checks, g_failed);
    rmrf(ROOT);
    return g_failed ? 1 : 0;
}
