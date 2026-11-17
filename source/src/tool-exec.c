/* tool-exec.c — M4 sandboxed tool worker.
 *
 * Invoked by the agent runtime: `tool-exec <jail_dir>`.
 * Protocol: one JSON request on stdin  {"tool":"...","args":{...}}
 *           one JSON response on stdout {"ok":true,"result":...}
 *                                      or {"ok":false,"error":"..."}
 *
 * Self-imposed limits (the runtime enforces the wall clock):
 *   - chdir(jail) — all fs.* paths resolve inside the jail
 *   - rlimits: CPU, AS 256MB, NOFILE 64, NPROC 64, FSIZE 8MB
 *   - alarm(15) backstop
 *   - PR_SET_NO_NEW_PRIVS (prepares M6 seccomp/Landlock)
 * Path rules: relative, no ".." components, no symlinked final
 * component (O_NOFOLLOW). Deeper hardening lands in M6. */
#include "aios/types.h"
#include "aios/json.h"
#include "aios/ksched.h"
#include "aios/hardening.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#define FS_READ_MAX  (256u * 1024)
#define FS_WRITE_MAX (1024u * 1024)

/* Set from the request envelope before any tool runs. Default: a human
 * invocation, which writes through to the jail. */
static int  g_origin_ai = 0;
static char g_stage[1024];

/* ---------------- calc: recursive-descent evaluator ----------------
 * Grammar (C-style precedence):
 *   expr   := term  (('+'|'-') term)*
 *   term   := power (('*'|'/') power)*
 *   power  := primary ('^' power)?          right-assoc
 *   primary := ('+'|'-')* ( number | '(' expr ')' )
 */
typedef struct { const char *s; size_t pos; bool err; } cparser;

static void cskip(cparser *p) {
    while (p->s[p->pos] == ' ' || p->s[p->pos] == '\t') p->pos++;
}

static double cexpr(cparser *p, int depth);
static double cterm(cparser *p, int depth);

static double cnumber(cparser *p) {
    char *end = NULL;
    double v = strtod(p->s + p->pos, &end);
    if (end == p->s + p->pos) { p->err = true; return 0; }
    p->pos = (size_t)(end - p->s);
    return v;
}

static double cprimary(cparser *p, int depth) {
    if (depth > 32) { p->err = true; return 0; }
    cskip(p);
    int sign = 1;
    while (p->s[p->pos] == '+' || p->s[p->pos] == '-') {
        if (p->s[p->pos] == '-') sign = -sign;
        p->pos++;
        cskip(p);
    }
    double v;
    if (p->s[p->pos] == '(') {
        p->pos++;
        v = cexpr(p, depth + 1);
        cskip(p);
        if (p->s[p->pos] != ')') { p->err = true; return 0; }
        p->pos++;
    } else {
        v = cnumber(p);
    }
    return sign * v;
}

static double cpower(cparser *p, int depth) {
    double base = cprimary(p, depth);
    cskip(p);
    if (p->s[p->pos] == '^') {
        p->pos++;
        double expv = cpower(p, depth + 1); /* right-assoc */
        return pow(base, expv);
    }
    return base;
}

static double cterm(cparser *p, int depth) {
    if (depth > 32) { p->err = true; return 0; }
    double v = cpower(p, depth);
    for (;;) {
        cskip(p);
        if (p->s[p->pos] == '*') { p->pos++; v *= cpower(p, depth); }
        else if (p->s[p->pos] == '/') {
            p->pos++;
            double d = cpower(p, depth);
            if (d == 0) { p->err = true; return 0; }
            v /= d;
        } else break;
    }
    return v;
}

static double cexpr(cparser *p, int depth) {
    if (depth > 32) { p->err = true; return 0; }
    double v = cterm(p, depth);
    for (;;) {
        cskip(p);
        if (p->s[p->pos] == '+') { p->pos++; v += cterm(p, depth); }
        else if (p->s[p->pos] == '-') { p->pos++; v -= cterm(p, depth); }
        else break;
    }
    return v;
}

static json_val *tool_calc(json_val *args, char *err, size_t ecap) {
    const char *expr = json_str(json_obj_get(args, "expr"));
    if (!expr || !*expr) {
        snprintf(err, ecap, "missing \"expr\"");
        return NULL;
    }
    if (strlen(expr) > 4096) {
        snprintf(err, ecap, "expression too long");
        return NULL;
    }
    cparser p = { expr, 0, false };
    double v = cexpr(&p, 0);
    cskip(&p);
    if (p.err || p.s[p.pos] != 0 || !isfinite(v)) {
        snprintf(err, ecap, "cannot evaluate expression");
        return NULL;
    }
    json_val *res = json_new_obj();
    json_obj_set(res, "value", json_new_num(v));
    return res;
}

/* ---------------- fs path safety ---------------- */

static bool safe_relpath(const char *path) {
    if (!path || !*path) return false;
    if (path[0] == '/') return false;
    /* reject any ".." path component */
    const char *p = path;
    while (*p) {
        const char *sep = strchrnul(p, '/');
        size_t len = (size_t)(sep - p);
        if (len == 2 && p[0] == '.' && p[1] == '.') return false;
        p = *sep ? sep + 1 : sep;
    }
    return true;
}

/* ---------------- fs tools ---------------- */

static json_val *tool_fs_read(json_val *args, char *err, size_t ecap) {
    const char *path = json_str(json_obj_get(args, "path"));
    if (!path || !safe_relpath(path)) {
        snprintf(err, ecap, "fs.read needs a relative path inside the jail (no \"..\")");
        return NULL;
    }
    char *data = NULL;
    size_t len = 0;
    aios_status st = aios_read_file(path, &data, &len, FS_READ_MAX);
    if (st != AIOS_OK) {
        snprintf(err, ecap, "fs.read failed: %s", aios_status_str(st));
        return NULL;
    }
    json_val *res = json_new_obj();
    json_obj_set(res, "path", json_new_str(path));
    json_obj_set(res, "bytes", json_new_num((double)len));
    json_obj_set(res, "content", json_new_str(data));
    free(data);
    return res;
}

/* M3: where a write actually goes.
 *
 * `g_origin_ai` and `g_stage` come from the request envelope, not the
 * environment (see tools.h for why). The invariant this function exists to
 * enforce:
 *
 *     if the origin is AI, a write either lands in staging or it fails.
 *     It never falls back to the jail.
 *
 * That last clause is the whole point. A staging layer that silently writes
 * through when its directory is missing is worse than no staging layer,
 * because everything upstream believes the write is pending review. */
static json_val *tool_fs_write(json_val *args, char *err, size_t ecap) {
    const char *path = json_str(json_obj_get(args, "path"));
    const char *content = json_str(json_obj_get(args, "content"));
    if (!path || !safe_relpath(path) || !content) {
        snprintf(err, ecap, "fs.write needs {\"path\":rel,\"content\":str}");
        return NULL;
    }

    char dest[2048];
    if (g_origin_ai) {
        if (!g_stage[0]) {
            snprintf(err, ecap, "an AI write was refused: no staging directory "
                                "was supplied (a machine write may never land "
                                "directly)");
            return NULL;
        }
        /* jail-relative containment inside the staging dir, again: the parent
         * validated this, and we do not trust the parent.
         *
         * realpath() writes up to PATH_MAX bytes, so it gets its own
         * PATH_MAX-sized buffer. Handing it `dest` (2048) overflowed the stack
         * the first time this ran. */
        char stageabs[PATH_MAX];
        if (!realpath(g_stage, stageabs)) {
            snprintf(err, ecap, "an AI write was refused: staging dir %.120s "
                                "does not exist", g_stage);
            return NULL;
        }
        size_t alen = strlen(stageabs);
        size_t plen = strlen(path);
        if (alen + plen + 8 >= sizeof dest) {
            snprintf(err, ecap, "an AI write was refused: staged path too long");
            return NULL;
        }
        memcpy(dest, stageabs, alen);
        memcpy(dest + alen, "/upper/", 7);
        memcpy(dest + alen + 7, path, plen + 1);
    } else {
        snprintf(dest, sizeof dest, "%s", path);
    }
    size_t clen = strlen(content);
    if (clen > FS_WRITE_MAX) {
        snprintf(err, ecap, "content exceeds 1MB write limit");
        return NULL;
    }
    /* create parent directories so agents can write new paths */
    char dirbuf[2048];
    if (strlen(dest) >= sizeof dirbuf) {
        snprintf(err, ecap, "fs.write: path too long");
        return NULL;
    }
    memcpy(dirbuf, dest, strlen(dest) + 1);
    char *slash = strrchr(dirbuf, '/');
    if (slash && slash != dirbuf) {
        *slash = 0;
        if (aios_mkdirs(dirbuf) != AIOS_OK) {
            snprintf(err, ecap, "fs.write: cannot create directory %.200s", dirbuf);
            return NULL;
        }
    }
    FILE *f = fopen(dest, "wb");
    if (!f) {
        snprintf(err, ecap, "fs.write: cannot open %.200s (%s)", dest, strerror(errno));
        return NULL;
    }
    bool ok = fwrite(content, 1, clen, f) == clen;
    fclose(f);
    if (!ok) {
        snprintf(err, ecap, "fs.write: short write");
        return NULL;
    }
    json_val *res = json_new_obj();
    json_obj_set(res, "path", json_new_str(path));
    json_obj_set(res, "bytes", json_new_num((double)clen));
    json_obj_set(res, "staged", json_new_bool(g_origin_ai));
    return res;
}

static json_val *tool_fs_list(json_val *args, char *err, size_t ecap) {
    const char *path = json_str(json_obj_get(args, "path"));
    if (!path || !*path) path = ".";
    if (!safe_relpath(path)) {
        snprintf(err, ecap, "fs.list needs a relative path inside the jail (no \"..\")");
        return NULL;
    }
    DIR *d = opendir(path);
    if (!d) {
        snprintf(err, ecap, "fs.list: cannot open %s (%s)", path, strerror(errno));
        return NULL;
    }
    json_val *arr = json_new_arr();
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        json_val *o = json_new_obj();
        json_obj_set(o, "name", json_new_str(de->d_name));
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        struct stat sb;
        if (stat(full, &sb) == 0) {
            json_obj_set(o, "bytes", json_new_num((double)sb.st_size));
            json_obj_set(o, "is_dir", json_new_bool(S_ISDIR(sb.st_mode)));
        }
        json_arr_push(arr, o);
    }
    closedir(d);
    json_val *res = json_new_obj();
    json_obj_set(res, "path", json_new_str(path));
    json_obj_set(res, "entries", arr);
    return res;
}

static json_val *tool_selftest_sleep(json_val *args, char *err, size_t ecap) {
    long ms = (long)json_num(json_obj_get(args, "ms"));
    if (ms <= 0 || ms > 60000) {
        snprintf(err, ecap, "selftest.sleep needs ms in [1,60000]");
        return NULL;
    }
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    json_val *res = json_new_obj();
    json_obj_set(res, "slept_ms", json_new_num((double)ms));
    return res;
}

static json_val *tool_selftest_spin(json_val *args, char *err, size_t ecap) {
    (void)args;
    /* infinite busy loop; only the parent deadline/alarm ends this */
    volatile double x = 0;
    for (;;) {
        x += 1.0;
        if (x < 0) break;
    }
    (void)err; (void)ecap;
    return NULL;
}

/* ---------------- limits + main ---------------- */

static void apply_limits(void) {
    struct rlimit rl;
    rl.rlim_cur = 15; rl.rlim_max = 15;
    setrlimit(RLIMIT_CPU, &rl);
    rl.rlim_cur = 256UL * 1024 * 1024;
    rl.rlim_max = rl.rlim_cur;
    setrlimit(RLIMIT_AS, &rl);
    rl.rlim_cur = 64; rl.rlim_max = 64;
    setrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = 64; rl.rlim_max = 64;
    setrlimit(RLIMIT_NPROC, &rl);
    rl.rlim_cur = 8UL * 1024 * 1024;
    rl.rlim_max = rl.rlim_cur;
    setrlimit(RLIMIT_FSIZE, &rl);
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    alarm(15); /* backstop; runtime enforces the real wall clock */
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tool-exec <jail_dir>\n");
        return 2;
    }
    /* resolve the jail to an absolute path BEFORE entering it: Landlock
     * needs an absolute, pre-chdir reference to the jail directory. */
    char jailabs[4096];
    if (!realpath(argv[1], jailabs)) {
        fprintf(stderr, "tool-exec: realpath(%s): %s\n", argv[1], strerror(errno));
        return 2;
    }
    if (chdir(jailabs) != 0) {
        fprintf(stderr, "tool-exec: chdir(%s): %s\n", jailabs, strerror(errno));
        return 2;
    }
    umask(0077);
    apply_limits();

    /* M6 kernel bridge: this worker is BATCH work — the OS schedules it
     * below interactive work and (with cgroup v2) caps it at 50% CPU. */
    aios_ksched_apply(AIOS_KS_BATCH);
    aios_ksched_register(AIOS_KS_BATCH);
    aios_cgroup_move_pid((long)getpid(), AIOS_KS_BATCH);

    /* M6 hardening: seccomp allowlist + Landlock jail (env-gated). */
    if (getenv("AIOS_SANDBOX_HARD")) {
        char why[128] = {0};
        int bits = aios_hardening_apply(jailabs, why, sizeof why);
        fprintf(stderr, "tool-exec: hardening %s (%s)\n",
                (bits & 3) == 3 ? "full" : "partial", why);
    }

    /* read full request from stdin */
    char *buf = NULL;
    size_t cap = 0, len = 0;
    char tmp[4096];
    ssize_t r;
    while ((r = read(STDIN_FILENO, tmp, sizeof tmp)) > 0) {
        if (len + (size_t)r > 64u * 1024) { fprintf(stderr, "request too large\n"); return 3; }
        if (len + (size_t)r > cap) {
            cap = (len + (size_t)r + 4096) * 2;
            buf = (char *)realloc(buf, cap);
            if (!buf) return 3;
        }
        memcpy(buf + len, tmp, (size_t)r);
        len += (size_t)r;
    }
    if (!buf) { fprintf(stderr, "empty request\n"); return 3; }
    buf[len] = 0;

    json_val *req = json_parse(buf, len);
    free(buf);
    char errbuf[512] = {0};
    json_val *resp = json_new_obj();
    if (!req) {
        json_obj_set(resp, "ok", json_new_bool(false));
        json_obj_set(resp, "error", json_new_str("unparsable request"));
    } else {
        const char *tool = json_str(json_obj_get(req, "tool"));
        /* M3 envelope: the parent states the origin and the staging dir. */
        const char *origin = json_str(json_obj_get(req, "origin"));
        const char *stage_dir = json_str(json_obj_get(req, "stage"));
        g_origin_ai = origin && strcmp(origin, "ai") == 0;
        g_stage[0] = 0;
        if (stage_dir && *stage_dir && strlen(stage_dir) < sizeof g_stage)
            memcpy(g_stage, stage_dir, strlen(stage_dir) + 1);
        json_val *args = json_obj_take(req, "args");
        if (!args) args = json_new_obj();
        json_val *result = NULL;
        bool ok = true;
        if (!tool) {
            ok = false;
            snprintf(errbuf, sizeof errbuf, "missing \"tool\"");
        } else if (strcmp(tool, "calc") == 0) {
            result = tool_calc(args, errbuf, sizeof errbuf);
        } else if (strcmp(tool, "fs.read") == 0) {
            result = tool_fs_read(args, errbuf, sizeof errbuf);
        } else if (strcmp(tool, "fs.write") == 0) {
            result = tool_fs_write(args, errbuf, sizeof errbuf);
        } else if (strcmp(tool, "fs.list") == 0) {
            result = tool_fs_list(args, errbuf, sizeof errbuf);
        } else if (strcmp(tool, "selftest.sleep") == 0) {
            result = tool_selftest_sleep(args, errbuf, sizeof errbuf);
        } else if (strcmp(tool, "selftest.spin") == 0) {
            result = tool_selftest_spin(args, errbuf, sizeof errbuf);
        } else {
            ok = false;
            snprintf(errbuf, sizeof errbuf, "unknown tool \"%s\"", tool);
        }
        json_free(args);
        if (ok && result) {
            json_obj_set(resp, "ok", json_new_bool(true));
            json_obj_set(resp, "result", result);
        } else {
            json_obj_set(resp, "ok", json_new_bool(false));
            json_obj_set(resp, "error",
                         json_new_str(errbuf[0] ? errbuf : "tool failed"));
        }
        json_free(req);
    }

    char *out = json_dump(resp);
    json_free(resp);
    if (out) {
        size_t olen = strlen(out);
        ssize_t w = write(STDOUT_FILENO, out, olen);
        (void)w;
        ssize_t w2 = write(STDOUT_FILENO, "\n", 1);
        (void)w2;
        free(out);
    }
    return 0;
}
