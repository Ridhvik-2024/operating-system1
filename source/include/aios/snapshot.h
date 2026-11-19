/* aios/snapshot.h — M5: rollback exists, and PID 1 owns it.
 *
 * A snapshot is a byte-for-byte copy of the two trees whose contents describe
 * everything a machine can change: the JAIL (the files an agent works on) and
 * the STORE (memory journal, audit log, jobs, staging). Taking one before every
 * agent run and before every promote means a bad run can be undone, and the
 * undo does not depend on the component that made the mess.
 *
 * Layout:
 *
 *   <store>/snapshots/<id>/            one snapshot
 *          manifest.json               id, ts, label, file counts, tree digests
 *          jail/                       copy of the jail
 *          store/                      copy of the store, EXCLUDING
 *                                      snapshots/ (never recursive) and
 *                                      staging/ (transient by definition)
 *          DONE                        present only once complete
 *
 * The DONE marker is the whole crash story: a snapshot is copied into a
 * temporary directory and renamed into place, and DONE is written last. A
 * directory without DONE was interrupted and is never offered for restore --
 * restoring a half-copied tree would be worse than having no snapshot, because
 * it looks like one.
 *
 * WHAT THIS IS NOT
 *   This copies. It is O(bytes) and needs disk for each snapshot. An 8 MiB data
 *   disk holds a handful, which is why the default retention is small and why
 *   pruning is not optional. overlayfs/btrfs subvolumes (L2) make snapshots
 *   cheap; until then, the honest trade is "few, and the count is enforced".
 *
 *   It also does not stop the machine writing DURING a copy. A snapshot taken
 *   while an agent is running may catch the jail mid-change -- consistent per
 *   file (each file is copied whole) but not per tree. That is why PID 1 takes
 *   the snapshot *before* starting the work: the window is a bug, not a feature,
 *   and M6's journal is what will make the ordering auditable.
 *
 * Restore is deliberately destructive and deliberately boring: it replaces both
 * trees from the snapshot, after moving the current ones aside, so a failed
 * restore leaves the current state recoverable rather than deleted.
 */
#ifndef AIOS_SNAPSHOT_H
#define AIOS_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIOS_SNAP_ID_MAX     64
#define AIOS_SNAP_PATH_MAX   512
#define AIOS_SNAP_LABEL_MAX  96
/* Retention default. Small on purpose: each snapshot is a full copy, and the
 * data disk in the image is 8 MiB. */
#define AIOS_SNAP_KEEP       5

typedef struct {
    char     id[AIOS_SNAP_ID_MAX];
    char     label[AIOS_SNAP_LABEL_MAX];
    long long taken_ms;
    size_t   files;
    unsigned long long bytes;
    unsigned long long jail_digest;   /* FNV-1a over the jail tree  */
    unsigned long long store_digest;  /* and the store, for compare */
    bool     complete;                /* DONE marker present        */
} snap_info;

typedef struct {
    snap_info *items;
    size_t     n;
    size_t     cap;
} snap_list;

/* Take a snapshot of `jail` and `store`, labelled `label` (may be NULL).
 * The id is written to `id_out` (AIOS_SNAP_ID_MAX bytes) and is a UTC-ish
 * timestamp plus a sequence, so two snapshots in the same millisecond differ.
 *
 * `store` must be absolute; the snapshot lives at <store>/snapshots/<id>.
 * Returns false and fills `err` on failure; on failure nothing is left behind
 * that could be restored. */
bool snap_take(const char *jail, const char *store, const char *label,
               char *id_out, size_t id_cap, char *err, size_t ecap);

/* List complete snapshots, newest first. Caller frees with snap_list_free(). */
bool snap_list_all(const char *store, snap_list *out, char *err, size_t ecap);
void snap_list_free(snap_list *l);

/* Look one up by id, or by prefix, or by `latest`. Returns false when absent.
 * A prefix that matches more than one snapshot is an error: restoring the wrong
 * one is not a mistake to resolve by guessing. */
bool snap_find(const char *store, const char *id_or_prefix, snap_info *out,
               char *err, size_t ecap);

/* Restore `jail` and `store` from snapshot `id_or_prefix`.
 *
 * Current contents are NOT deleted: they are moved to
 * <store>/snapshots/.replaced/<ts>/ first, so a restore that turns out to be the
 * wrong decision can itself be undone. Returns false if the snapshot is
 * incomplete or the trees cannot be swapped.
 *
 * This does NOT stop running agents. Stopping them is the caller's job, and in
 * the OS that caller is PID 1 -- see snap_note in shell.c. A restore while a job
 * is writing is a race this function cannot win and does not pretend to. */
bool snap_restore(const char *jail, const char *store, const char *id_or_prefix,
                  char *replaced_at, size_t replaced_cap,
                  char *err, size_t ecap);

/* Keep the newest `keep` complete snapshots, remove the rest (and any
 * interrupted ones). `keep` of 0 means "remove all", which is how a test proves
 * pruning works. Returns the number removed, or -1 on error. */
int snap_prune(const char *store, size_t keep, char *err, size_t ecap);

/* ---- introspection, for tests and for `snap` in the shell ---- */

/* Digest of a whole tree: every regular file's path and contents, in sorted
 * path order. Two trees with the same digest have the same files with the same
 * bytes. Symlinks are digested by target, not followed. */
bool snap_tree_digest(const char *root, unsigned long long *digest,
                      size_t *n_files, unsigned long long *n_bytes,
                      char *err, size_t ecap);

/* Render a list for humans. Caller frees. */
char *snap_list_text(const snap_list *l);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_SNAPSHOT_H */
