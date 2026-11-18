/* aios/stage.h — M3: no AI write lands in reality.
 *
 * A machine-originated write goes to a staging directory, is shown as a diff,
 * and only enters the jail when a human promotes it. A delete does not unlink:
 * it moves the file into a trash directory and leaves a tombstone, so it can be
 * reversed.
 *
 * Layout (all under <store>/staging):
 *
 *   <store>/staging/<intent>-<seq>/        one staging session
 *         manifest.json                    written BEFORE any promote rename
 *         upper/                           the files that would be created
 *         trash/                           the files that were removed
 *
 * Crash safety, and why it is shaped this way:
 *   - a directory with no manifest.json is garbage; gc removes it.
 *   - promote writes the manifest (state="promoting"), then renames each file,
 *     then rewrites the manifest (state="done"), then fsyncs. An interrupted
 *     promote is a manifest saying "promoting" while the jail is part-way --
 *     re-running promote completes the remaining renames, because rename(2)
 *     over an existing file is idempotent for this purpose.
 *   - nothing is ever renamed into the jail before the manifest that records
 *     the intent to do so exists.
 *
 * There is no fsync-of-directory primitive that works identically on ext4
 * inside the image and on the host filesystems used by tests; we do it where
 * the platform allows and record the failure rather than pretend.
 */
#ifndef AIOS_STAGE_H
#define AIOS_STAGE_H

#include "aios/action.h"   /* aios_origin (M7) */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIOS_STAGE_PATH_MAX 512
#define AIOS_STAGE_MAX_FILES 64

typedef enum {
    STAGE_OP_ADD    = 0,   /* file does not exist in the jail            */
    STAGE_OP_MODIFY = 1,   /* file exists and its content changes        */
    STAGE_OP_DELETE = 2    /* file exists and would be removed           */
} stage_op;

typedef enum {
    STAGE_MANIFEST_NONE   = 0,  /* no manifest: abandoned, never promotes  */
    STAGE_MANIFEST_OPEN   = 1,  /* collecting changes                      */
    STAGE_MANIFEST_PROMOTING = 2,  /* promote started, not finished       */
    STAGE_MANIFEST_DONE   = 3   /* promote finished                        */
} stage_state;

const char *stage_op_str(stage_op op);
const char *stage_state_str(stage_state st);

/* One row of a diff. `bytes` is the signed size delta of the file (0 for a
 * delete). `plus`/`minus` are line counts, computed for text files only. */
typedef struct {
    char   path[AIOS_STAGE_PATH_MAX];
    stage_op op;
    long   bytes;
    int    plus;
    int    minus;
} stage_diff_entry;

typedef struct {
    stage_diff_entry *entries;
    size_t            n;
    size_t            cap;
} stage_diff;

/* A staging session. Owned by the caller; stage_end() releases it. */
typedef struct {
    char  dir[AIOS_STAGE_PATH_MAX];   /* <store>/staging/<intent>-<seq>   */
    char  upper[AIOS_STAGE_PATH_MAX]; /* dir/upper                        */
    char  trash[AIOS_STAGE_PATH_MAX]; /* dir/trash                        */
    char  jail[AIOS_STAGE_PATH_MAX];  /* absolute jail                    */
    char  store[AIOS_STAGE_PATH_MAX]; /* absolute store                   */
    char  manifest[AIOS_STAGE_PATH_MAX];
    uint64_t intent_hash;
    uint64_t seq;
    bool  own_dir;
} stage_session;

/* Begin a session. `jail` and `store` must be absolute paths. Creates
 * <store>/staging/<intent>-<seq>/{upper,trash} and an OPEN manifest.
 * `seq` disambiguates two sessions with the same intent hash (the same command
 * run twice) — pass 0 to auto-increment from the store. */
bool stage_begin(const char *jail, const char *store, uint64_t intent_hash,
                 uint64_t seq, stage_session *out, char *err, size_t ecap);

/* Re-attach to an existing session directory (used by promote/discard, which
 * run in a different process from the one that staged). */
bool stage_open(const char *dir, stage_session *out, char *err, size_t ecap);

void stage_end(stage_session *s);

/* The directory a machine-originated write must be redirected into. Files are
 * written at upper/<relpath>, mirroring the jail. */
const char *stage_upper(const stage_session *s);

/* Resolve `rel` (a jail-relative path, already validated by the policy layer)
 * into the staged location. Returns false on a bad or escaping path. */
bool stage_path_for(const stage_session *s, const char *rel,
                    char *out, size_t ocap);

/* Record the intent to touch `rel`. Called as each file is staged. Idempotent.
 * Does not write the manifest by itself; stage_manifest_sync() does. */
bool stage_note(stage_session *s, const char *rel, stage_op op,
                char *err, size_t ecap);

/* Write the manifest to disk atomically. The manifest is the only thing that
 * makes a staging directory "live" to promote and gc. */
bool stage_manifest_sync(stage_session *s, stage_state st, char *err, size_t ecap);

/* Compare staging against the jail. Reads both sides; does not mutate either.
 * Caller frees with stage_diff_free(). */
bool stage_diff_compute(stage_session *s, stage_diff *out, char *err, size_t ecap);
void stage_diff_free(stage_diff *d);

/* Render a diff as human-readable text: one line per file, plus a summary.
 * Caller frees. */
char *stage_diff_text(const stage_diff *d);

/* Move one staged file into the jail. Fails rather than partially writing: the
 * destination is written via tmp+rename. */
bool stage_promote_one(stage_session *s, const char *rel,
                       char *err, size_t ecap);

/* Promote every note in the manifest. Idempotent and re-runnable: a second call
 * after an interruption completes the remaining renames.
 *
 * M7: the origin is REQUIRED. Promotion is the moment a machine's proposal
 * becomes real, so the one function that makes it real must be told who is
 * asking. A shell with an AI-origin line gets a refusal here even if some future
 * caller forgets to check -- the guard is in the implementation, not only in the
 * dispatcher that happens to call it today. There is no default argument and no
 * `stage_promote_trusted()`; a caller that cannot say who it is cannot promote. */
bool stage_promote(stage_session *s, aios_origin origin, size_t *n_promoted,
                   char *err, size_t ecap);

/* Discard: move the whole directory under <store>/staging/.trash/<seq>-<intent>.
 * The jail is never touched. */
bool stage_discard(stage_session *s, char *err, size_t ecap);

/* Snapshot `rel` from the jail into trash/ and remove it from the jail, leaving
 * a tombstone note. Reversed by stage_undelete(). */
bool stage_delete(stage_session *s, const char *rel, char *err, size_t ecap);

/* Restore a trashed file from the staging session's trash/ back into the jail. */
bool stage_undelete(stage_session *s, const char *rel, char *err, size_t ecap);

/* ---- garbage collection ---- */

typedef struct {
    size_t   removed;
    size_t   kept;
} stage_gc_result;

/* Remove staging directories that cannot ever be promoted: those with no
 * manifest, or an OPEN manifest older than max_age_ms. A directory whose
 * manifest says PROMOTING is never removed by gc — it may be the only record
 * of a half-finished promote.
 * `now_ms` is a parameter so this is testable without waiting. */
bool stage_gc(const char *store, long long now_ms, long long max_age_ms,
              stage_gc_result *out, char *err, size_t ecap);

/* Promote with the files locked down.
 *
 * `stage_promote()` trusts the staging directory to contain only what the
 * staging layer put there. A human who can write into that directory can
 * therefore promote an arbitrary file into the jail -- the one path around the
 * policy engine that does not look like one.
 *
 * This variant loads every note first, then writes files named by notes it
 * validated, and only from inside upper/. Anything else in the directory is
 * ignored and reported. Use this one wherever a human is the trigger. */
bool stage_promote_locked(stage_session *s, aios_origin origin,
                          size_t *n_promoted,
                          char *err, size_t ecap);

/* ---- M7.5: pending is a LIST, and nothing is silently lost ----
 *
 * M3 through M7 treated the staging area as if at most one change were ever
 * waiting. Two things were true and neither was checked:
 *
 *   1. `promote` and `staged` each answered "which change do you mean?" by
 *      picking ONE session out of the directory -- the newest by mtime, ties
 *      broken by readdir order. Two pending changes were indistinguishable from
 *      one, and the one not picked could never be named.
 *   2. A session can hold a file that no note names. The shell pre-notes the
 *      effects it declared; a second write folded into an already-live session
 *      never got its own note, so its bytes sat in upper/ described by nothing.
 *      The diff iterates over NOTES, so those bytes were invisible to `staged`
 *      and were never promoted. The human read "1 file" while two had been
 *      written.
 *
 * A diff that shows less than the staging area holds is worse than no diff: it
 * is a review that can be passed without reviewing. The rule this file now
 * enforces is the boring one -- what is shown is what would move, and what
 * would move is named in the manifest.
 */

#define AIOS_STAGE_MAX_PENDING 32

typedef struct {
    char        dir[AIOS_STAGE_PATH_MAX];
    char        id[80];               /* <intent>-<seq>: the name under staging/ */
    stage_state state;
    size_t      n_notes;              /* rows the manifest records             */
    size_t      n_files;              /* files actually in upper/ (recursive)  */
    size_t      n_unnoted;            /* files in upper/ that no note names    */
    long long   mtime_ms;             /* used only to order the list           */
} stage_pending;

typedef struct {
    stage_pending items[AIOS_STAGE_MAX_PENDING];
    size_t        n;
    bool          truncated;          /* more sessions pending than we can list */
} stage_pending_list;

/* Enumerate every session that is still promotable (manifest OPEN or
 * PROMOTING), newest first. A missing staging directory is an empty list, not
 * an error: "nothing is staged" is an answer.
 *
 * PROMOTING sessions are included on purpose. A promote that died half-way is
 * the one change that most needs to be seen -- it is the only record that the
 * jail may be part-way between two states. */
bool stage_pending_scan(const char *store, stage_pending_list *out,
                        char *err, size_t ecap);
void stage_pending_free(stage_pending_list *l);

/* Resolve what a human typed after `promote`/`discard` into a session dir.
 *
 * Accepts an id (`<intent>-<seq>`, as printed by `staged`), a session
 * directory path, or `latest`. `latest` resolves only when the answer is
 * unambiguous -- it is a convenience for "the one change that is pending", not
 * a way to pick one of several. Ambiguity is an ERROR here rather than a
 * choice, because a promote is not a place to guess: see the file comment.
 * On error the reason names the ids that were pending. */
bool stage_resolve(const char *store, const char *what,
                   char *dir_out, size_t cap, char *err, size_t ecap);

/* Files inside the session's upper/ that the manifest does not name, as
 * jail-relative paths. `names` may be NULL to only count. Returns the number
 * found (capped at `cap`, but the return value counts only what it stored when
 * `names` is non-NULL).
 *
 * These files are not promotable and must not be described as if they were; the
 * shell prints them separately and refuses a promote that would leave them. */
size_t stage_unnoted(stage_session *s,
                     char (*names)[AIOS_STAGE_PATH_MAX], size_t cap,
                     char *err, size_t ecap);

/* Record every note in `paths`/`ops` the session does not already have.
 * Used by the shell to keep extending one live session across several AI
 * writes without re-noting what is already there. */
bool stage_note_many(stage_session *s,
                     const char (*paths)[AIOS_STAGE_PATH_MAX],
                     const stage_op *ops, size_t n, char *err, size_t ecap);

/* ---- introspection, used by the shell and by tests ---- */

/* Read the manifest state of a directory without opening a session. */
stage_state stage_manifest_state(const char *dir);

/* Count of notes recorded in the manifest (0 if unreadable). */
size_t stage_manifest_count(const char *dir);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_STAGE_H */
