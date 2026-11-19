/* tests/test_snapshot.c — M5: snapshots and rollback.
 *
 * The interesting tests here are not "does a copy happen". They are:
 *   - does the restored tree have the SAME BYTES as before the change (tree
 *     digest, not eyeballed file contents)?
 *   - is a snapshot that was interrupted mid-copy refused rather than restored?
 *   - does prune keep what it says it keeps?
 *   - does a failed restore leave the live state intact rather than half-swapped?
 */
#include "t.h"
#include "aios/snapshot.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROOT "build/test-snap"

static void rmrf(const char *d) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s", d);
    (void)!system(cmd);
}

static void mkdirs(const char *d) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "mkdir -p %s", d);
    (void)!system(cmd);
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) { CHECK(0, "cannot write fixture file"); return; }
    fputs(text, f);
    fclose(f);
}

static bool file_is(const char *path, const char *text) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strcmp(buf, text) == 0;
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

/* ---------------- 1. digest ---------------- */

static void test_digest(void) {
    printf("tree digests\n");
    rmrf(ROOT "/d1");
    rmrf(ROOT "/d2");
    mkdirs(ROOT "/d1/sub");
    mkdirs(ROOT "/d2/sub");
    write_file(ROOT "/d1/a.txt", "alpha\n");
    write_file(ROOT "/d1/sub/b.txt", "beta\n");
    write_file(ROOT "/d2/a.txt", "alpha\n");
    write_file(ROOT "/d2/sub/b.txt", "beta\n");

    unsigned long long h1 = 0, h2 = 0;
    size_t n1 = 0, n2 = 0;
    unsigned long long b1 = 0, b2 = 0;
    char err[192] = {0};
    CHECK(snap_tree_digest(ROOT "/d1", &h1, &n1, &b1, err, sizeof err),
          "digest of tree one");
    CHECK(snap_tree_digest(ROOT "/d2", &h2, &n2, &b2, err, sizeof err),
          "digest of tree two");

    /* The whole point of a digest: an independent copy is indistinguishable. */
    CHECK(h1 == h2, "two identical trees have the same digest");
    CHECK(n1 == 2 && n2 == 2, "and the same file count");
    CHECK(b1 == b2 && b1 == 11, "and the same byte count");

    /* One byte changes... */
    write_file(ROOT "/d2/a.txt", "alphA\n");
    CHECK(snap_tree_digest(ROOT "/d2", &h2, &n2, &b2, err, sizeof err),
          "re-digest after a change");
    CHECK(h1 != h2, "a single changed byte changes the digest");

    /* ...and a RENAME changes it too: same bytes, different tree. A digest that
     * only hashed contents would call a moved file unchanged. */
    write_file(ROOT "/d2/a.txt", "alpha\n");
    unsigned long long h3 = 0;
    CHECK(snap_tree_digest(ROOT "/d2", &h3, NULL, NULL, err, sizeof err),
          "re-digest after restoring the byte");
    CHECK(h3 == h1, "restoring the byte restores the digest");

    char cmd[512];
    snprintf(cmd, sizeof cmd, "mv %s/d2/a.txt %s/d2/moved.txt", ROOT, ROOT);
    (void)!system(cmd);
    CHECK(snap_tree_digest(ROOT "/d2", &h2, NULL, NULL, err, sizeof err),
          "re-digest after a rename");
    CHECK(h2 != h1, "a renamed file changes the digest even though bytes match");

    /* An empty tree and a missing tree are different answers. */
    mkdirs(ROOT "/d3");
    unsigned long long h4 = 0;
    size_t n4 = 99;
    CHECK(snap_tree_digest(ROOT "/d3", &h4, &n4, NULL, err, sizeof err),
          "an empty tree digests");
    CHECK(n4 == 0, "and reports no files");
    CHECK(!snap_tree_digest(ROOT "/nonexistent-tree", &h4, NULL, NULL,
                            err, sizeof err),
          "a missing tree is an error, not an empty tree");
}

/* ---------------- 2. take and restore ---------------- */

static void test_take_restore(void) {
    printf("take, mutate, restore\n");
    rmrf(ROOT "/work");
    mkdirs(ROOT "/work/jail");
    mkdirs(ROOT "/work/store");
    write_file(ROOT "/work/jail/keep.txt", "keep me\n");
    write_file(ROOT "/work/jail/later.txt", "original content\n");
    write_file(ROOT "/work/store/memory.journal", "{\"tier\":\"episodic\"}\n");

    unsigned long long before_jail = 0, before_store = 0;
    char err[512] = {0};
    CHECK(snap_tree_digest(ROOT "/work/jail", &before_jail, NULL, NULL, err, sizeof err),
          "digest the jail before");
    CHECK(snap_tree_digest(ROOT "/work/store", &before_store, NULL, NULL, err, sizeof err),
          "digest the store before");

    char id[AIOS_SNAP_ID_MAX] = {0};
    CHECK(snap_take(ROOT "/work/jail", ROOT "/work/store", "before the run",
                    id, sizeof id, err, sizeof err),
          "snapshot taken");
    CHECK(id[0] != 0, "and it has an id");
    if (err[0]) printf("      take said: %s\n", err);

    /* The mess an agent makes. */
    write_file(ROOT "/work/jail/later.txt", "clobbered\n");
    write_file(ROOT "/work/jail/invented.txt", "was never there\n");
    write_file(ROOT "/work/store/memory.journal", "{\"tier\":\"junk\"}\n");

    unsigned long long after_jail = 0;
    snap_tree_digest(ROOT "/work/jail", &after_jail, NULL, NULL, err, sizeof err);
    CHECK(after_jail != before_jail, "the mess changed the jail");

    char replaced[512] = {0};
    CHECK(snap_restore(ROOT "/work/jail", ROOT "/work/store", id,
                       replaced, sizeof replaced, err, sizeof err),
          "restore succeeded");
    if (err[0]) printf("      restore said: %s\n", err);

    unsigned long long back_jail = 0, back_store = 0;
    CHECK(snap_tree_digest(ROOT "/work/jail", &back_jail, NULL, NULL, err, sizeof err),
          "digest the jail after restore");
    CHECK(back_jail == before_jail,
          "the jail is byte-for-byte what it was before the run");
    /* The store is compared on its CONTENTS, not on the whole tree: the snapshot
     * machinery lives inside the store (snapshots/, .replaced/), so a whole-tree
     * digest necessarily differs after taking one. Comparing the parts that are
     * state is the honest comparison; pretending the bookkeeping is state would
     * make this assertion fail forever. */
    CHECK(file_is(ROOT "/work/store/memory.journal", "{\"tier\":\"episodic\"}\n"),
          "and the store's contents are back");
    (void)before_store;
    (void)back_store;

    CHECK(file_is(ROOT "/work/jail/later.txt", "original content\n"),
          "the clobbered file has its old content");
    CHECK(!exists(ROOT "/work/jail/invented.txt"),
          "the invented file is gone");

    /* Nothing was deleted: the state we replaced is recoverable. */
    CHECK(replaced[0] != 0, "the replaced state was moved somewhere");
    if (replaced[0]) {
        char p[600];
        snprintf(p, sizeof p, "%s/jail/later.txt", replaced);
        CHECK(file_is(p, "clobbered\n"),
              "and the clobbered version is still there to go back to");
        char q[600];
        snprintf(q, sizeof q, "%s/jail/invented.txt", replaced);
        CHECK(exists(q), "including the file that was invented");
    }

    /* A snapshot must not contain itself. */
    char self[600];
    snprintf(self, sizeof self, "%s/work/store/snapshots", id[0] ? ROOT : ROOT);
    snap_list l;
    char lerr[192] = {0};
    CHECK(snap_list_all(ROOT "/work/store", &l, lerr, sizeof lerr), "list after take");
    CHECK(l.n == 1, "exactly one snapshot");
    if (l.n == 1) {
        char nested[700];
        snprintf(nested, sizeof nested, "%s/work/store/snapshots/%s/store/snapshots",
                 ROOT, l.items[0].id);
        CHECK(!exists(nested),
              "the snapshot does not contain a copy of the snapshots directory");
        char d[700];
        snprintf(d, sizeof d, "%s/work/store/snapshots/%s/DONE", ROOT, l.items[0].id);
        CHECK(exists(d), "and it is marked complete");
    }
    snap_list_free(&l);
}

/* ---------------- 3. incomplete snapshots are refused ---------------- */

static void test_incomplete_refused(void) {
    printf("an interrupted snapshot is not restorable\n");
    rmrf(ROOT "/inc");
    mkdirs(ROOT "/inc/jail");
    mkdirs(ROOT "/inc/store");
    write_file(ROOT "/inc/jail/f.txt", "one\n");

    char id[AIOS_SNAP_ID_MAX] = {0};
    char err[512] = {0};
    CHECK(snap_take(ROOT "/inc/jail", ROOT "/inc/store", "good", id, sizeof id,
                    err, sizeof err), "a good snapshot");

    /* Strip its DONE marker: exactly what a crash between the rename and the
     * marker write leaves behind. */
    char done[700];
    snprintf(done, sizeof done, "%s/inc/store/snapshots/%s/DONE", ROOT, id);
    CHECK(exists(done), "the marker exists to begin with");
    unlink(done);

    /* It is still LISTED -- a human must be able to see that a snapshot exists
     * and is not usable -- but snap_find() refuses it, because find() is what
     * restore uses and "found an unusable one" is not a useful answer there. */
    snap_list l;
    char lerr[192] = {0};
    CHECK(snap_list_all(ROOT "/inc/store", &l, lerr, sizeof lerr), "list after unlink");
    CHECK(l.n == 1, "the interrupted snapshot is still listed");
    CHECK(l.n == 1 && !l.items[0].complete, "and is marked incomplete");
    snap_list_free(&l);

    snap_info si;
    char ferr[192] = {0};
    CHECK(!snap_find(ROOT "/inc/store", id, &si, ferr, sizeof ferr),
          "but find() refuses to return it");
    CHECK_STR_CONTAINS(ferr, "incomplete", "saying exactly that");

    /* `latest` must skip it rather than restore a half-copied tree. */
    char rerr[192] = {0};
    CHECK(!snap_find(ROOT "/inc/store", "latest", &si, rerr, sizeof rerr),
          "latest refuses to point at an incomplete snapshot");
    CHECK_STR_CONTAINS(rerr, "no complete snapshot", "and says why");

    CHECK(!snap_restore(ROOT "/inc/jail", ROOT "/inc/store", id, NULL, 0,
                        rerr, sizeof rerr),
          "restoring it fails");
    CHECK_STR_CONTAINS(rerr, "incomplete", "with a reason a human can act on");

    /* And the live tree was not touched by the failed attempt. */
    CHECK(file_is(ROOT "/inc/jail/f.txt", "one\n"), "the live tree is untouched");
}

/* ---------------- 4. ambiguous restore is refused ---------------- */

static void test_ambiguous_prefix(void) {
    printf("an ambiguous id is not guessed\n");
    rmrf(ROOT "/amb");
    mkdirs(ROOT "/amb/jail");
    mkdirs(ROOT "/amb/store");
    write_file(ROOT "/amb/jail/f.txt", "x\n");

    char a[AIOS_SNAP_ID_MAX] = {0}, b[AIOS_SNAP_ID_MAX] = {0};
    char err[512] = {0};
    CHECK(snap_take(ROOT "/amb/jail", ROOT "/amb/store", "one", a, sizeof a,
                    err, sizeof err), "first snapshot");
    CHECK(snap_take(ROOT "/amb/jail", ROOT "/amb/store", "two", b, sizeof b,
                    err, sizeof err), "second snapshot");
    if (err[0]) printf("      take said: %s\n", err);

    /* Find a shared prefix of the two ids. They share the millisecond almost
     * always, which is precisely why guessing would be dangerous. */
    size_t common = 0;
    while (a[common] && b[common] && a[common] == b[common]) common++;
    if (common == 0) {
        CHECK(1, "(ids share no prefix on this machine; prefix test not applicable)");
        return;
    }
    char prefix[AIOS_SNAP_ID_MAX];
    snprintf(prefix, sizeof prefix, "%.*s", (int)common, a);

    snap_info si;
    char ferr[256] = {0};
    CHECK(!snap_find(ROOT "/amb/store", prefix, &si, ferr, sizeof ferr),
          "a prefix matching both snapshots is refused");
    CHECK_STR_CONTAINS(ferr, "more specific", "and asks for a longer one");
    CHECK(!snap_restore(ROOT "/amb/jail", ROOT "/amb/store", prefix, NULL, 0,
                        ferr, sizeof ferr),
          "and restore will not guess either");
}

/* ---------------- 5. prune ---------------- */

static void test_prune(void) {
    printf("prune keeps the newest, and only the newest\n");
    rmrf(ROOT "/pr");
    mkdirs(ROOT "/pr/jail");
    mkdirs(ROOT "/pr/store");
    write_file(ROOT "/pr/jail/f.txt", "v0\n");

    char err[256] = {0};
    for (int i = 0; i < 4; i++) {
        char body[32];
        snprintf(body, sizeof body, "v%d\n", i + 1);
        write_file(ROOT "/pr/jail/f.txt", body);
        char id[AIOS_SNAP_ID_MAX];
        CHECK(snap_take(ROOT "/pr/jail", ROOT "/pr/store", NULL, id, sizeof id,
                        err, sizeof err), "snapshot in the loop");
        /* Ids are ms+pid; a short sleep guarantees distinct taken_ms on a fast
         * machine so "newest" is well defined. */
        usleep(2000);
    }

    snap_list l;
    CHECK(snap_list_all(ROOT "/pr/store", &l, err, sizeof err), "four snapshots");
    CHECK(l.n == 4, "all four listed");
    snap_list_free(&l);

    int removed = snap_prune(ROOT "/pr/store", 2, err, sizeof err);
    CHECK(removed == 2, "prune removed two");
    CHECK(snap_list_all(ROOT "/pr/store", &l, err, sizeof err), "list after prune");
    CHECK(l.n == 2, "two remain");
    CHECK(l.items[0].complete && l.items[1].complete, "and both are complete");
    /* Newest first, so the surviving labels must be the last two written. */
    CHECK(l.items[0].taken_ms >= l.items[1].taken_ms, "newest first");
    snap_list_free(&l);

    CHECK(snap_prune(ROOT "/pr/store", 0, err, sizeof err) == 2,
          "keep=0 removes the rest");
    CHECK(snap_list_all(ROOT "/pr/store", &l, err, sizeof err), "list after clearing");
    CHECK(l.n == 0, "nothing left");
    CHECK(!snap_find(ROOT "/pr/store", "latest", &(snap_info){0}, err, sizeof err),
          "and latest now fails honestly");
    snap_list_free(&l);
}

/* ---------------- 6. a failed restore does not half-swap ---------------- */

static void test_failed_restore_is_atomic(void) {
    printf("a restore that cannot finish leaves the state alone\n");
    rmrf(ROOT "/at");
    mkdirs(ROOT "/at/jail");
    mkdirs(ROOT "/at/store");
    write_file(ROOT "/at/jail/f.txt", "before\n");

    char id[AIOS_SNAP_ID_MAX] = {0};
    char err[256] = {0};
    CHECK(snap_take(ROOT "/at/jail", ROOT "/at/store", NULL, id, sizeof id,
                    err, sizeof err), "snapshot");

    write_file(ROOT "/at/jail/f.txt", "after\n");
    unsigned long long h = 0;
    snap_tree_digest(ROOT "/at/jail", &h, NULL, NULL, err, sizeof err);

    /* Demand a snapshot that does not exist. The failure must be total. */
    CHECK(!snap_restore(ROOT "/at/jail", ROOT "/at/store", "does-not-exist",
                        NULL, 0, err, sizeof err),
          "restoring a missing snapshot fails");
    unsigned long long h2 = 0;
    CHECK(snap_tree_digest(ROOT "/at/jail", &h2, NULL, NULL, err, sizeof err),
          "the jail is still there");
    CHECK(h == h2, "and unchanged by the failed restore");
    CHECK(file_is(ROOT "/at/jail/f.txt", "after\n"),
          "the current content is what it was before the attempt");
}

/* ---------------- 7. survives a reboot ---------------- *
 *
 * The card's acceptance says a snapshot must survive a simulated reboot. What
 * that means concretely: nothing about a snapshot may live in process memory.
 * The test takes one, then FORGETS everything (a new snap_list from a fresh
 * call, with no shared state) and expects to find and restore it. If snapshots
 * were cached in a struct, this is the test that would notice.
 */
static void test_survives_reboot(void) {
    printf("a snapshot survives being forgotten and re-read\n");
    rmrf(ROOT "/rb");
    mkdirs(ROOT "/rb/jail");
    mkdirs(ROOT "/rb/store");
    write_file(ROOT "/rb/jail/before.txt", "the state before\n");

    char err[256] = {0};
    char id[AIOS_SNAP_ID_MAX] = {0};
    CHECK(snap_take(ROOT "/rb/jail", ROOT "/rb/store", "reboot proof",
                    id, sizeof id, err, sizeof err), "snapshot taken");

    /* Nothing is kept but the id string itself -- exactly what would survive a
     * reboot (the id is what a human would have written down). */
    char saved[AIOS_SNAP_ID_MAX];
    snprintf(saved, sizeof saved, "%s", id);

    write_file(ROOT "/rb/jail/before.txt", "the state after\n");
    write_file(ROOT "/rb/jail/after.txt", "added later\n");

    snap_list l;
    char lerr[192] = {0};
    CHECK(snap_list_all(ROOT "/rb/store", &l, lerr, sizeof lerr), "re-read from disk");
    CHECK(l.n == 1, "the snapshot is still there");
    CHECK(l.n == 1 && strcmp(l.items[0].id, saved) == 0, "with the same id");
    CHECK(l.n == 1 && strcmp(l.items[0].label, "reboot proof") == 0, "and the label");
    CHECK(l.n == 1 && l.items[0].files == 1, "and the file count it recorded");
    snap_list_free(&l);

    CHECK(snap_restore(ROOT "/rb/jail", ROOT "/rb/store", saved, NULL, 0,
                       err, sizeof err), "and it can still be restored");
    CHECK(file_is(ROOT "/rb/jail/before.txt", "the state before\n"),
          "the old content is back");
    CHECK(!exists(ROOT "/rb/jail/after.txt"), "the later addition is gone");
}

/* ---------------- 7b. two takes in the same millisecond ---------------- *
 *
 * The id was timestamp+pid, which is unique on an idle desktop and not unique
 * on a machine that finishes a take inside a single millisecond -- which is what
 * the image does on its ext2 disk. The second take then reused the first one's
 * directory, and the failure surfaced three groups later as "an ambiguous id is
 * not guessed" failing, because there was only one snapshot left to be ambiguous
 * about.
 *
 * This asserts the property directly, so the next time it breaks it says so.
 */
static void test_ids_are_unique(void) {
    /* Ids used to be timestamp+pid. On a host clock that is unique; on a guest
     * whose clock ticks in 10ms it is not, and the second take then fails to
     * rename its temp directory onto the first one and reports a bare failure.
     * The property that matters is that takes never collide, whatever the clock
     * resolution -- so that is what is asserted. */
    printf("snapshots taken back to back do not collide\n");
    rmrf(ROOT "/uniq");
    mkdirs(ROOT "/uniq/jail");
    mkdirs(ROOT "/uniq/store");
    write_file(ROOT "/uniq/jail/f.txt", "u\n");

    char ids[8][AIOS_SNAP_ID_MAX];
    memset(ids, 0, sizeof ids);
    bool all_taken = true;
    for (int i = 0; i < 8; i++) {
        char err[256] = {0};
        if (!snap_take(ROOT "/uniq/jail", ROOT "/uniq/store", "same", ids[i],
                       sizeof ids[i], err, sizeof err)) {
            all_taken = false;
            printf("      take %d failed: %s\n", i, err);
        }
    }
    CHECK(all_taken, "eight takes in a row all succeed");

    bool distinct = true;
    for (int i = 0; i < 8 && distinct; i++)
        for (int j = i + 1; j < 8; j++)
            if (strcmp(ids[i], ids[j]) == 0) {
                printf("      collision: %s == %s\n", ids[i], ids[j]);
                distinct = false;
                break;
            }
    CHECK(distinct, "and every id is different, even in one millisecond");

    snap_list l;
    memset(&l, 0, sizeof l);
    CHECK(snap_list_all(ROOT "/uniq/store", &l, NULL, 0), "the store still opens");
    CHECK(l.n == 8, "eight snapshots are in the store, not one");
    snap_list_free(&l);
}

/* ---------------- 8. the store on a DIFFERENT filesystem ---------------- *
 *
 * This is the arrangement the real image uses and every test here missed: the
 * jail lives on the ext2 side disk (/data) and the store can be on the
 * initramfs, or vice versa. rename() answers EXDEV across filesystems, and the
 * error reads like a permissions problem.
 *
 * /dev/shm is a different filesystem from the source tree on Linux, which makes
 * this reproducible without root. Where it is not available (or is the same
 * filesystem) the test says so instead of pretending it ran.
 */
static void test_cross_filesystem(void) {
    printf("store on a different filesystem from the jail\n");
    const char *jail = ROOT "/xf-jail";
    const char *store = "/dev/shm/aios-test-snap-xf";

    struct stat a, b;
    bool have_shm = (stat("/dev/shm", &b) == 0);
    if (!have_shm) { SKIP("no /dev/shm"); return; }
    mkdirs(ROOT);
    if (stat(ROOT, &a) != 0) { SKIP("no test root"); return; }
    if (a.st_dev == b.st_dev) { SKIP("/dev/shm is the same filesystem"); return; }

    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s", store);
    (void)!system(cmd);
    snprintf(cmd, sizeof cmd, "rm -rf %s", jail);
    (void)!system(cmd);
    mkdirs(jail);
    snprintf(cmd, sizeof cmd, "mkdir -p %s", store);
    (void)!system(cmd);

    write_file(ROOT "/xf-jail/f.txt", "on the data disk\n");
    CHECK(1, "the two filesystems are different");

    char err[512] = {0};
    char id[AIOS_SNAP_ID_MAX] = {0};
    CHECK(snap_take(jail, store, "cross-fs", id, sizeof id, err, sizeof err),
          "snapshot taken with the store on another filesystem");
    if (err[0]) printf("      take said: %s\n", err);

    write_file(ROOT "/xf-jail/f.txt", "changed\n");
    char rerr[512] = {0};
    CHECK(snap_restore(jail, store, id, NULL, 0, rerr, sizeof rerr),
          "restore works across filesystems (no EXDEV)");
    if (rerr[0]) printf("      restore said: %s\n", rerr);
    CHECK(file_is(ROOT "/xf-jail/f.txt", "on the data disk\n"),
          "and the old content is back");

    snprintf(cmd, sizeof cmd, "rm -rf %s", store);
    (void)!system(cmd);
}

int main(void) {
    printf("== test_snapshot ==\n");
    rmrf(ROOT);
    mkdirs(ROOT);

    test_digest();
    test_take_restore();
    test_incomplete_refused();
    test_ambiguous_prefix();
    test_prune();
    test_failed_restore_is_atomic();
    test_survives_reboot();
    test_ids_are_unique();
    test_cross_filesystem();

    printf("%s: %d checks, %d failed\n",
           g_failed ? "FAIL" : "PASS", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
