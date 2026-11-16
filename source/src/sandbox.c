/* aios/sandbox.c — M4: parent-side sandbox runner.
 * Spawns `tool-exec` in a fresh process, pipes the request, enforces the
 * wall-clock deadline from the parent side (SIGTERM then SIGKILL) so a
 * wedged worker can never hang the agent runtime. */
#include "sandbox.h"
#include "aios/types.h"
#include "aios/json.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>

#define REQ_MAX  (64u * 1024)
#define RESP_MAX (2u * 1024 * 1024)

static const char *resolve_worker(char *buf, size_t cap) {
    const char *env = getenv("AIOS_TOOL_EXEC");
    if (env && access(env, X_OK) == 0) return env;

    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0) {
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (slash) {
            snprintf(slash + 1, (size_t)(self + sizeof self - slash - 1),
                     "tool-exec");
            if (access(self, X_OK) == 0) {
                snprintf(buf, cap, "%s", self);
                return buf;
            }
        }
    }
    if (access("./tool-exec", X_OK) == 0) return "./tool-exec";
    return NULL;
}

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

aios_status sandbox_run(const char *jail, const char *request_json,
                        size_t req_len, double timeout_s,
                        json_val **result_out, char *err, size_t ecap) {
    if (!request_json || req_len == 0 || req_len > REQ_MAX || !result_out || !jail)
        return set_err(err, ecap, "bad request"), AIOS_ERR_ARG;

    char wbuf[PATH_MAX];
    const char *worker = resolve_worker(wbuf, sizeof wbuf);
    if (!worker) return set_err(err, ecap, "tool-exec worker not found"), AIOS_ERR_NOTFOUND;

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0)  return set_err(err, ecap, "pipe: %s", strerror(errno)), AIOS_ERR_IO;
    if (pipe(out_pipe) != 0) { close(in_pipe[0]); close(in_pipe[1]);
        return set_err(err, ecap, "pipe: %s", strerror(errno)), AIOS_ERR_IO; }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return set_err(err, ecap, "fork: %s", strerror(errno)), AIOS_ERR_IO;
    }
    if (pid == 0) {
        /* child: wire pipes, exec worker (jail passed via argv[1]) */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        /* stderr passthrough suppressed: worker writes diagnostics there */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execl(worker, "tool-exec", jail, (char *)NULL);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    /* write request */
    bool wok = true;
    size_t off = 0;
    while (off < req_len) {
        ssize_t w = write(in_pipe[1], request_json + off, req_len - off);
        if (w <= 0) { if (errno == EINTR) continue; wok = false; break; }
        off += (size_t)w;
    }
    close(in_pipe[1]);
    if (!wok) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(out_pipe[0]);
        return set_err(err, ecap, "failed writing request to worker"), AIOS_ERR_IO;
    }

    /* read response with deadline */
    double deadline = aios_mono_ms() + (timeout_s + 2.0) * 1000.0;
    char *resp = (char *)malloc(RESP_MAX);
    if (!resp) {
        kill(pid, SIGKILL); waitpid(pid, NULL, 0); close(out_pipe[0]);
        return set_err(err, ecap, "oom"), AIOS_ERR_NOMEM;
    }
    size_t got = 0;
    bool timed_out = false, eof = false;
    while (!eof) {
        double remain = deadline - aios_mono_ms();
        if (remain <= 0) { timed_out = true; break; }
        struct pollfd pf = { out_pipe[0], POLLIN, 0 };
        int pr = poll(&pf, 1, (int)(remain > 1000.0 ? 1000.0 : remain));
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue; /* poll tick; re-check deadline */
        ssize_t r = read(out_pipe[0], resp + got, RESP_MAX - got - 1);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) { eof = true; break; }
        got += (size_t)r;
        if (got >= RESP_MAX - 1) break;
    }
    close(out_pipe[0]);
    resp[got] = 0;

    int wstatus = 0;
    if (timed_out) {
        kill(pid, SIGTERM);
        /* grace, then SIGKILL */
        for (int i = 0; i < 20; i++) {
            if (waitpid(pid, &wstatus, WNOHANG) == pid) goto done_wait;
            usleep(50000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, &wstatus, 0);
    done_wait:
        free(resp);
        return set_err(err, ecap, "tool timed out after %.1fs (worker killed)",
                       timeout_s), AIOS_ERR_TIMEOUT;
    }

    /* reap with a safety deadline even when worker finished normally */
    for (int i = 0; i < 40; i++) {
        if (waitpid(pid, &wstatus, WNOHANG) == pid) break;
        usleep(25000);
        if (i == 39) { kill(pid, SIGKILL); waitpid(pid, &wstatus, 0); }
    }

    if (got == 0) {
        free(resp);
        return set_err(err, ecap, "worker produced no output (status %d)",
                       WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1), AIOS_ERR_TOOL;
    }

    json_val *v = json_parse(resp, got);
    free(resp);
    if (!v) return set_err(err, ecap, "worker returned unparsable output"), AIOS_ERR_TOOL;

    json_val *ok = json_obj_get(v, "ok");
    if (!ok || !json_bool(ok)) {
        json_val *e = json_obj_get(v, "error");
        set_err(err, ecap, "%s", e && json_str(e) ? json_str(e) : "worker reported failure");
        json_free(v);
        return AIOS_ERR_TOOL;
    }
    /* keep only {"ok":true,"result":...} shape; result may be null */
    *result_out = v;
    return AIOS_OK;
}
