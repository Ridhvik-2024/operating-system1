/* aios/hardening.h — M6: kernel-enforced sandbox hardening for tool-exec.
 *
 * Two independent kernel locks applied by the tool worker before it runs
 * any tool:
 *
 *   seccomp(SECCOMP_MODE_FILTER) — syscall allowlist: the worker can no
 *   longer fork, exec, open sockets, mount, or touch anything the tools
 *   should not need. Violations kill the worker (SIGSYS).
 *
 *   Landlock — unprivileged filesystem MAC: the worker's view of the
 *   filesystem is restricted to the jail directory (read/write) before
 *   it runs; every other path returns EACCES regardless of unix perms.
 *
 *   There is exactly ONE other place a worker may write, and it exists
 *   because of M3: when a machine's write is staged, the bytes go to
 *   <store>/staging/<id>/upper, which is deliberately OUTSIDE the jail
 *   (a staged change that could be written into the jail would not be
 *   staged). A Landlock ruleset that only allows the jail therefore turns
 *   every honest AI write into EACCES -- which is what happened: the
 *   staging path and the jail cannot both be "the only writable place"
 *   unless the sandbox is told about both.
 *
 * Both are best-effort: aios_hardening_apply() returns a bitmask of what
 * actually locked in; callers decide whether absence is fatal.
 */
#ifndef AIOS_HARDENING_H
#define AIOS_HARDENING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIOS_HARD_SECCOMP  0x1
#define AIOS_HARD_LANDLOCK 0x2

/* apply to the current process. jail may be NULL (skip landlock).
 * `extra_rw` is an optional SECOND writable root -- the staging directory, for
 * a machine write -- and may be NULL. It is added with the same access as the
 * jail and nothing more: one more door, not an open house.
 * returns the AIOS_HARD_* bits applied; *why collects failure text. */
int aios_hardening_apply(const char *jail, const char *extra_rw,
                         char *why, size_t whycap);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_HARDENING_H */
