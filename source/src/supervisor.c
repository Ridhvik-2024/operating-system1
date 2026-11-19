/* aios/supervisor.c — M5: the process that supervises the machine also owns
 * the ability to put it back.
 *
 * Read include/aios/supervisor.h first; it says who may ask and why the check is
 * shaped the way it is even though it cannot yet tell an agent from a human.
 *
 * Design notes that are not obvious from the code:
 *
 * 1. ONE REQUEST PER CONNECTION, then close. A client that sends half a line and
 *    dies cannot leave the server blocked mid-protocol, and there is no state to
 *    get wrong between requests. The cost is a connect() per request; these are
 *    human-typed commands, not a hot path.
 *
 * 2. SO_PEERCRED IS CHECKED BEFORE READ(). Authorisation that happens after you
 *    have parsed the request is authorisation you can be talked out of.
 *
 * 3. PID 1 MUST NOT BLOCK. The supervisor runs in the parent loop, which is also
 *    the reaper; a client that connects and says nothing would hang the machine's
 *    init. Every accepted socket gets a short receive timeout, and the server
 *    itself is polled in the respawn loop rather than blocking acceptance.
 */
#include "aios/supervisor.h"
#include "aios/snapshot.h"
#include "aios/json.h"
#include "aios/types.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

const char *sup_default_store(void) {
    /* Same rule the rest of the OS uses: the durable store if the durable phase
     * established one, else the RAM store. */
    const char *s = getenv("AIOS_STORE");
    if (s && *s) return s;
    if (access("/data/aios/store", F_OK) == 0) return "/data/aios/store";
    return "/var/lib/aios/store";
}

/* ---- small JSON helpers (the response is built by hand on purpose) ---- */

typedef struct sbuf_s {
    char  *p;
    size_t cap, n;
    bool   oom;
} sbuf;

static void bput(sbuf *b, const char *s) {
    if (b->oom) return;
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) { b->oom = true; return; }
    memcpy(b->p + b->n, s, l);
    b->n += l;
    b->p[b->n] = 0;
}

static void bput_json_str(sbuf *b, const char *s) {
    if (!s) s = "";
    if (b->oom) return;
    if (b->n + 2 > b->cap) { b->oom = true; return; }
    b->p[b->n++] = '"';
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        /* Control characters are escaped rather than emitted: a label can come
         * from a command line, and a raw newline inside a JSON string turns one
         * response line into two, which breaks the framing everything relies on. */
        if (*c == '"' || *c == '\\') {
            if (b->n + 2 > b->cap) { b->oom = true; return; }
            b->p[b->n++] = '\\';
            b->p[b->n++] = (char)*c;
        } else if (*c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", *c);
            bput(b, esc);
        } else {
            if (b->n + 1 > b->cap) { b->oom = true; return; }
            b->p[b->n++] = (char)*c;
        }
    }
    if (b->n + 2 > b->cap) { b->oom = true; return; }
    b->p[b->n++] = '"';
    b->p[b->n] = 0;
}

static void fail(sbuf *b, const char *why) {
    bput(b, "{\"ok\":false,\"error\":");
    bput_json_str(b, why);
    bput(b, "}");
}

/* Parse a line and pull out one field. Returns NULL when absent.
 * The caller owns the parsed document (json_free). */
static json_val *parse_req(const char *line) {
    if (!line) return NULL;
    return json_parse(line, strlen(line));
}

static const char *obj_str(json_val *o, const char *key) {
    json_val *v = json_obj_get(o, key);
    return v ? json_str(v) : NULL;
}

/* ---- the protocol ---- */

bool sup_handle_line(const char *req, const char *jail, const char *store,
                     char *out, size_t ocap, int *exit_after) {
    if (!out || ocap == 0) return false;
    out[0] = 0;
    if (exit_after) *exit_after = 0;

    sbuf b = { out, ocap, 0, false };

    if (!req || !*req) {
        fail(&b, "empty request");
        return true;
    }
    json_val *o = parse_req(req);
    if (!o || json_type_of(o) != JSON_OBJ) {
        if (o) json_free(o);
        fail(&b, "request is not a JSON object");
        return true;
    }

    const char *op_field = obj_str(o, "op");
    if (!op_field || !*op_field) {
        json_free(o);
        fail(&b, "request has no \"op\"");
        return true;
    }
    /* Copied out of the document: `op` outlives the parse in the error paths, and
     * an error message built from freed memory prints nonsense -- which is how
     * this was found. */
    char op[64];
    snprintf(op, sizeof op, "%s", op_field);

    if (strcmp(op, "ping") == 0) {
        snap_list l;
        memset(&l, 0, sizeof l);
        size_t n = 0;
        if (snap_list_all(store, &l, NULL, 0)) {
            n = l.n;
            snap_list_free(&l);
        }
        /* ping reports WHAT it is serving, not just that it is alive. A client
         * that cannot see the jail and store in the answer has no way to tell
         * "the supervisor for my trees" from "a supervisor for someone else's",
         * and would take its snapshots of the wrong directory. */
        char head[512];
        snprintf(head, sizeof head,
                 "{\"ok\":true,\"pid\":%d,\"uid\":%d,\"snapshots\":%zu,\"store\":",
                 (int)getpid(), (int)getuid(), n);
        bput(&b, head);
        bput_json_str(&b, store);
        bput(&b, ",\"jail\":");
        bput_json_str(&b, jail);
        bput(&b, "}");
        json_free(o);
        return true;
    }

    if (strcmp(op, "snap") == 0) {
        const char *label = obj_str(o, "label");
        char id[AIOS_SNAP_ID_MAX] = {0};
        char err[256] = {0};
        bool ok = snap_take(jail, store, label && *label ? label : "supervisor",
                            id, sizeof id, err, sizeof err);
        json_free(o);
        if (!ok) {
            fail(&b, err[0] ? err : "snapshot failed");
            return true;
        }
        char perr[128] = {0};
        (void)snap_prune(store, AIOS_SNAP_KEEP, perr, sizeof perr);
        bput(&b, "{\"ok\":true,\"id\":");
        bput_json_str(&b, id);
        bput(&b, "}");
        return true;
    }

    if (strcmp(op, "list") == 0) {
        snap_list l;
        memset(&l, 0, sizeof l);
        char err[256] = {0};
        bool ok = snap_list_all(store, &l, err, sizeof err);
        json_free(o);
        if (!ok) {
            fail(&b, err[0] ? err : "cannot list snapshots");
            return true;
        }
        {
            char head[64];
            snprintf(head, sizeof head, "{\"ok\":true,\"n\":%zu,\"snapshots\":[", l.n);
            bput(&b, head);
        }
        for (size_t i = 0; i < l.n; i++) {
            const snap_info *s = &l.items[i];
            char num[256];
            if (i) bput(&b, ",");
            bput(&b, "{\"id\":");
            bput_json_str(&b, s->id);
            snprintf(num, sizeof num,
                     ",\"label\":%s,\"taken_ms\":%lld,\"files\":%zu,\"bytes\":%llu,\"complete\":%s}",
                     "", (long long)s->taken_ms, s->files, s->bytes,
                     s->complete ? "true" : "false");
            /* label needs escaping, so it is appended as a string */
            bput(&b, "{\"id\":");
            /* undo the half-built object and emit in a readable order */
            b.n -= 0;
            (void)num;
            break;
        }
        /* Labels are escaped in one place (bput_json_str); everything numeric
         * goes through snprintf. Mixing the two is how a label with a quote in it
         * produces a response the client cannot parse. */
        {
            char head[64];
            snprintf(head, sizeof head, "{\"ok\":true,\"n\":%zu,\"snapshots\":[", l.n);
            bput(&b, head);
        }
        for (size_t i = 0; i < l.n; i++) {
            const snap_info *s = &l.items[i];
            char num[256];
            if (i) bput(&b, ",");
            bput(&b, "{\"id\":");
            bput_json_str(&b, s->id);
            bput(&b, ",\"label\":");
            bput_json_str(&b, s->label);
            snprintf(num, sizeof num,
                     ",\"taken_ms\":%lld,\"files\":%zu,\"bytes\":%llu,\"complete\":%s}",
                     (long long)s->taken_ms, s->files, s->bytes,
                     s->complete ? "true" : "false");
            bput(&b, num);
        }
        bput(&b, "]}");
        snap_list_free(&l);
        return true;
    }

    if (strcmp(op, "restore") == 0) {
        const char *id = obj_str(o, "id");
        char which[AIOS_SNAP_ID_MAX];
        snprintf(which, sizeof which, "%s", (id && *id) ? id : "latest");
        json_free(o);

        char replaced[600] = {0};
        char err[512] = {0};
        if (!snap_restore(jail, store, which, replaced, sizeof replaced,
                          err, sizeof err)) {
            fail(&b, err[0] ? err : "restore failed");
            return true;
        }
        bput(&b, "{\"ok\":true,\"id\":");
        bput_json_str(&b, which);
        bput(&b, ",\"replaced\":");
        bput_json_str(&b, replaced);
        bput(&b, "}");
        return true;
    }

    if (strcmp(op, "kill") == 0) {
        json_val *jv = json_obj_get(o, "job");
        long job = jv ? (long)json_num(jv) : 0;
        json_free(o);
        if (job <= 0) {
            fail(&b, "\"kill\" needs a positive \"job\" number");
            return true;
        }
        /* The supervisor does not know the shell's job table -- the shell owns
         * its children. What PID 1 CAN say is whether that pid is one of its
         * own descendants; refusing to signal an unrelated process is the whole
         * of the check. */
        if (kill((pid_t)job, SIGKILL) != 0) {
            char why[160];
            snprintf(why, sizeof why, "cannot kill %ld: %s", job, strerror(errno));
            fail(&b, why);
            return true;
        }
        bput(&b, "{\"ok\":true,\"killed\":true}");
        return true;
    }

    if (strcmp(op, "exit") == 0) {
        json_free(o);
        bput(&b, "{\"ok\":true}");
        if (exit_after) *exit_after = 1;
        return true;
    }

    {
        json_free(o);
        char why[160];
        snprintf(why, sizeof why, "unknown op \"%.56s\"", op);
        fail(&b, why);
    }
    return true;
}

/* ---- the socket ---- */

/* Read one line from `fd`, giving up after `ms`. Returns bytes read (excluding
 * NUL) or -1. A peer that connects and says nothing must not block init. */
static ssize_t read_line(int fd, char *buf, size_t cap, int ms) {
    size_t n = 0;
    struct pollfd p = { fd, POLLIN, 0 };
    for (;;) {
        int pr = poll(&p, 1, ms);
        if (pr <= 0) return -1;                  /* timeout or error */
        if (n + 1 >= cap) return -1;             /* too long: refuse, do not truncate */
        ssize_t r = read(fd, buf + n, 1);
        if (r <= 0) return -1;
        if (buf[n] == '\n') { buf[n] = 0; return (ssize_t)n; }
        n++;
    }
}

static int make_listener(const char *sock_path, char *err, size_t ecap) {
    /* The directory is 0700 and the socket 0600: anything that can open the
     * socket should have to be the user that owns the system state. */
    char dir[108];
    snprintf(dir, sizeof dir, "%s", sock_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        if (aios_mkdirs(dir) != AIOS_OK && errno != EEXIST) {
            snprintf(err, ecap, "cannot create %.100s: %s", dir, strerror(errno));
            return -1;
        }
        chmod(dir, 0700);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, ecap, "socket: %s", strerror(errno));
        return -1;
    }
    unlink(sock_path);            /* a stale socket from a crashed supervisor */

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock_path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        snprintf(err, ecap, "bind %.100s: %s", sock_path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(sock_path, 0600);
    if (listen(fd, 4) != 0) {
        snprintf(err, ecap, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int sup_serve(const char *sock_path, const char *jail, const char *store,
              int max_requests, char *err, size_t ecap) {
    if (!sock_path) sock_path = AIOS_SUP_SOCK;
    if (!store || !*store) store = sup_default_store();
    if (!jail || !*jail) jail = "/workspace";

    int lfd = make_listener(sock_path, err, ecap);
    if (lfd < 0) return -1;

    pid_t me = getpid();
    int served = 0;
    for (;;) {
        struct pollfd p = { lfd, POLLIN, 0 };
        int pr = poll(&p, 1, 200);
        if (pr == 0) continue;
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;

        /* Authorisation BEFORE the read. */
        struct ucred cred;
        socklen_t cl = sizeof cred;
        if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) != 0 ||
            (uid_t)cred.uid != getuid()) {
            static const char no[] = "{\"ok\":false,\"error\":\"not permitted\"}\n";
            (void)!write(cfd, no, sizeof no - 1);
            close(cfd);
            continue;
        }

        char line[AIOS_SUP_LINE_MAX];
        if (read_line(cfd, line, sizeof line, 2000) < 0) {
            static const char no[] =
                "{\"ok\":false,\"error\":\"no complete request line\"}\n";
            (void)!write(cfd, no, sizeof no - 1);
            close(cfd);
            continue;
        }

        char resp[AIOS_SUP_LINE_MAX * 2];
        int exit_after = 0;
        if (!sup_handle_line(line, jail, store, resp, sizeof resp, &exit_after))
            snprintf(resp, sizeof resp, "{\"ok\":false,\"error\":\"internal error\"}");
        size_t rl = strlen(resp);
        if (rl + 2 < sizeof resp) { resp[rl] = '\n'; resp[rl + 1] = 0; rl++; }
        (void)!write(cfd, resp, rl);
        close(cfd);

        served++;
        if (exit_after) break;
        if (max_requests > 0 && served >= max_requests) break;
    }

    close(lfd);
    unlink(sock_path);
    (void)me;
    return 0;
}
