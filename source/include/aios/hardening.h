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
 * returns the AIOS_HARD_* bits applied; *why collects failure text. */
int aios_hardening_apply(const char *jail, char *why, size_t whycap);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_HARDENING_H */
