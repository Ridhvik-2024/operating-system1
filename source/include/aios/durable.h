/* aios/durable.h — M8: crash-consistent durable state on the ext2 side disk.
 *
 * The OS keeps its live state under /data (an ext2 side disk mounted by
 * aios-init). Everything the durable store holds survives reboot and power
 * loss; everything else stays RAM-backed exactly as in m7 (and the whole
 * layer degrades to a no-op when no disk is present, so an m8 image without
 * a data disk behaves like an m7 image).
 *
 * Store layout under the durable root (default /data/aios):
 *   store/            the OS store dir handed to aiosh (--store this dir)
 *     memory.journal  3-tier memory JSONL (fsync'd appends by mem.c)
 *     audit.log       tool-call audit JSONL (tools.c)
 *     jobs.jsonl      job spawn/exit records (shell.c, fsync'd appends)
 *     jobs/<id>.json  per-job result documents
 *   var/              mirrors of /var/lib/aios (autopilot conf/state/journal)
 *   autopilot/        governor write-through (state.json, journal.jsonl)
 *   boots.jsonl       one line per boot (append-only boot log)
 *
 * Crash-consistency rules enforced here (and relied on everywhere):
 *   - atomic replacement: write sibling temp file, fsync file, rename,
 *     fsync the directory — a reader never sees a half-written file
 *   - journals: O_APPEND write + fsync; the replay reader treats a line
 *     that does not end in '\n' as torn (crash mid-append) and drops it
 *
 * All paths are relative to the durable root; tests redirect the root via
 * aios_dur_set_root() or the AIOS_DUR_ROOT environment variable. */
#ifndef AIOS_DURABLE_H
#define AIOS_DURABLE_H

#include "aios/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIOS_DUR_DEFAULT_ROOT "/data/aios"

/* Redirect the durable root (tests, or `dur root` rebinding). Copies the
 * string. Passing NULL restores the default (env AIOS_DUR_ROOT, else
 * AIOS_DUR_DEFAULT_ROOT). */
void        aios_dur_set_root(const char *root);

/* The active durable root (never NULL). */
const char *aios_dur_root(void);

/* True when the root exists as a directory and accepts writes. All other
 * calls return AIOS_ERR_IO harmlessly when this is false. */
bool        aios_dur_available(void);

/* fsync a single already-existing file by path (result documents etc.). */
aios_status aios_dur_fsync_path(const char *path);

/* Atomic replacement write into the store: rel is store-relative. */
aios_status aios_dur_write_atomic(const char *rel, const char *data,
                                  size_t len);

/* Append-only journal write: O_APPEND + fsync + directory fsync. */
aios_status aios_dur_append(const char *rel, const char *data, size_t len);

/* C4: the boot ring PID 1 writes and `log` reads. Fixed size on purpose: a log
 * that grows without bound on an 8 MiB data disk eventually becomes the reason a
 * boot fails. Shared here so init and the shell cannot disagree about it, which
 * they did while it was a literal in both files. */
#define AIOS_BOOT_LOG_REL  "boot.log"
#define AIOS_BOOT_LOG_CAP  (16 * 1024)

/* C4: does the store file exist? Lets a caller say "empty" rather than "error"
 * for a file that has simply never been written. */
bool aios_dur_exists(const char *rel);

/* Read a store file into out (up to cap bytes); *out_len gets the size. */
aios_status aios_dur_read(const char *rel, char *out, size_t cap,
                          size_t *out_len);

/* Copy a live (RAM-backed) file into the store atomically — write-through
 * for state files that continue to live at their canonical tmpfs path. */
aios_status aios_dur_mirror(const char *live_path, const char *rel);

/* Copy a store file back out to a live path (atomic replace at dest). */
aios_status aios_dur_restore_file(const char *rel, const char *dest);

/* Copy every regular file of store dir <root>/<rel> into dest (flat).
 * Returns the number of files restored, or -1 when the store dir is
 * absent or unreadable. */
int         aios_dur_restore_dir(const char *rel, const char *dest);

/* Torn-tail-safe journal scan: counts only lines terminated by '\n'
 * (a trailing partial line is a crash artifact and is ignored), and
 * copies the last complete line into last/cap (without the newline;
 * pass NULL to skip). Returns AIOS_OK even for an absent file (n=0). */
aios_status aios_dur_lines(const char *rel, size_t *n,
                           char *last, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AIOS_DURABLE_H */
