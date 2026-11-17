/* test_supervisor.c — M5: PID 1 owns rollback.
 *
 * The milestone says rollback is "owned by PID 1 -- not by the component that
 * takes AI input". test_snapshot.c proves the mechanism works; this proves it is
 * reachable only through the supervisor's socket, that the protocol refuses
 * what it should, and that a real client gets a real answer over a real socket.
 *
 * The protocol is tested two ways on purpose:
 *   - sup_handle_line() directly, for every error case (no socket, no timing)
 *   - a live server on a temporary socket, for the framing that a client sees
 */
#include "t.h"
#include "aios/supervisor.h"
#include "aios/snapshot.h"
#include "aios/json.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROOT "build/test-sup"

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputs(content, f);
    fclose(f);
}

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
        *q = 0;
        mkdir(tmp, 0755);
        *q = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

/* One request, one response -- the unit-level entry point. */
static bool ask(const char *req, char *out, size_t cap) {
    int exit_after = 0;
    return sup_handle_line(req, ROOT "/jail", ROOT "/store", out, cap, &exit_after);
}

/* ---------------- 1. the error cases ---------------- */

static void test_protocol_errors(void) {
    printf("the protocol refuses what it cannot understand\n");
    char out[8192];

    /* An empty line gets a refusal, not silence: a client that gets nothing back
     * cannot tell "you sent me nothing" from "the supervisor is not there". */
    CHECK(ask("", out, sizeof out), "an empty request is answered");
    CHECK_STR_CONTAINS(out, "\"ok\":false", "with a refusal");
    CHECK_STR_CONTAINS(out, "empty", "that says the request was empty");

    CHECK(ask("not json", out, sizeof out), "malformed JSON gets an answer");
    CHECK_STR_CONTAINS(out, "\"ok\":false", "which is a failure");
    CHECK_STR_CONTAINS(out, "JSON", "and says the request was not JSON");

    CHECK(ask("[1,2,3]", out, sizeof out), "a JSON array is answered");
    CHECK_STR_CONTAINS(out, "\"ok\":false", "and refused: the protocol is objects");

    CHECK(ask("{}", out, sizeof out), "an object with no op is answered");
    CHECK_STR_CONTAINS(out, "op", "and the error names the missing field");

    /* The important one: a typo must not look like a completed rollback. */
    CHECK(ask("{\"op\":\"restrore\",\"id\":\"latest\"}", out, sizeof out),
          "an unknown op is answered");
    CHECK_STR_CONTAINS(out, "\"ok\":false", "and refused, not silently ignored");
    CHECK_STR_CONTAINS(out, "unknown op", "and named");
    CHECK_STR_CONTAINS(out, "restrore", "with the word that was actually sent");

    CHECK(ask("{\"op\":\"kill\"}", out, sizeof out), "kill with no job is answered");
    CHECK_STR_CONTAINS(out, "\"ok\":false", "and refused");
    CHECK_STR_CONTAINS(out, "job", "and says what is missing");

    CHECK(ask("{\"op\":\"kill\",\"job\":-3}", out, sizeof out), "a negative pid is refused");
    CHECK_STR_CONTAINS(out, "\"ok\":false", "with a failure");

    /* A restored id that does not exist must not be reported as success. */
    CHECK(ask("{\"op\":\"restore\",\"id\":\"nosuchthing\"}", out, sizeof out),
          "restoring an unknown id is answered");
    CHECK_STR_CONTAINS(out, "\"ok\":false", "and fails");
}

/* ---------------- 2. a label cannot break the framing ---------------- */

static void test_label_escaping(void) {
    printf("a label with quotes or newlines cannot break the response\n");
    rmrf(ROOT);
    mkdirs(ROOT "/jail");
    mkdirs(ROOT "/store");
    write_file(ROOT "/jail/f.txt", "x\n");

    char out[8192];
    /* A newline inside the label would split one response line into two and
     * desynchronise any client reading line by line. */
    CHECK(ask("{\"op\":\"snap\",\"label\":\"a\\\"b\\\\c\"}", out, sizeof out),
          "a label with a quote and a backslash is accepted");
    CHECK_STR_CONTAINS(out, "\"ok\":true", "the snapshot is taken");
    CHECK(strchr(out, '\n') == NULL, "the response is one line, with no raw newline");

    /* And the response must still parse as JSON. */
    json_val *v = json_parse(out, strlen(out));
    CHECK(v != NULL, "the response parses as JSON");
    if (v) {
        CHECK(json_str(json_obj_get(v, "id")) != NULL, "and carries the id");
        json_free(v);
    }
}

/* ---------------- 3. snap / list / restore over the protocol ---------------- */

static void test_round_trip(void) {
    printf("snap, list and restore work through the supervisor\n");
    rmrf(ROOT);
    mkdirs(ROOT "/jail");
    mkdirs(ROOT "/store");
    write_file(ROOT "/jail/f.txt", "original\n");

    char out[8192];
    CHECK(ask("{\"op\":\"ping\"}", out, sizeof out), "ping is answered");
    CHECK_STR_CONTAINS(out, "\"ok\":true", "and succeeds");
    CHECK_STR_CONTAINS(out, "\"pid\":", "and reports the pid that owns it");

    CHECK(ask("{\"op\":\"snap\",\"label\":\"before\"}", out, sizeof out),
          "snap is answered");
    CHECK_STR_CONTAINS(out, "\"ok\":true", "and takes a snapshot");
    char id[AIOS_SNAP_ID_MAX] = {0};
    {
        json_val *v = json_parse(out, strlen(out));
        const char *s = v ? json_str(json_obj_get(v, "id")) : NULL;
        if (s) snprintf(id, sizeof id, "%s", s);
        if (v) json_free(v);
    }
    CHECK(id[0] != 0, "the response carries the id");

    CHECK(ask("{\"op\":\"list\"}", out, sizeof out), "list is answered");
    CHECK_STR_CONTAINS(out, "\"n\":1", "and counts one snapshot");
    CHECK_STR_CONTAINS(out, id, "and includes its id");

    write_file(ROOT "/jail/f.txt", "wrecked\n");
    char req[256];
    snprintf(req, sizeof req, "{\"op\":\"restore\",\"id\":\"%s\"}", id);
    CHECK(ask(req, out, sizeof out), "restore is answered");
    CHECK_STR_CONTAINS(out, "\"ok\":true", "and succeeds");
    CHECK_STR_CONTAINS(out, "replaced", "and reports where the old state went");

    FILE *f = fopen(ROOT "/jail/f.txt", "rb");
    char got[64] = {0};
    if (f) { size_t n = fread(got, 1, sizeof got - 1, f); (void)n; fclose(f); }
    CHECK(strcmp(got, "original\n") == 0, "and the bytes are back");
}

/* ---------------- 4. a real socket, a real client ---------------- */

/* Connect, send one line, read one line. Returns true on a complete response. */
static bool sock_ask(const char *path, const char *line, char *out, size_t cap) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return false; }
    (void)!write(fd, line, strlen(line));

    size_t n = 0;
    struct pollfd p = { fd, POLLIN, 0 };
    while (n + 1 < cap) {
        if (poll(&p, 1, 5000) <= 0) break;
        ssize_t r = read(fd, out + n, cap - n - 1);
        if (r <= 0) break;
        n += (size_t)r;
        if (memchr(out, '\n', n)) break;
    }
    out[n] = 0;
    close(fd);
    return n > 0;
}

static void test_live_socket(void) {
    printf("a client reaches the supervisor over its socket\n");
    rmrf(ROOT);
    mkdirs(ROOT "/jail");
    mkdirs(ROOT "/store");
    write_file(ROOT "/jail/f.txt", "sock\n");

    char sockpath[256];
    snprintf(sockpath, sizeof sockpath, "%s/sup.sock", ROOT);

    pid_t pid = fork();
    CHECK(pid >= 0, "fork for the server");
    if (pid == 0) {
        /* The server exits on its own after 3 requests: a test that has to kill
         * its server leaks one when it fails, and a stale socket on a fixed path
         * then breaks the NEXT run for an unrelated reason. */
        char err[256] = {0};
        int rc = sup_serve(sockpath, ROOT "/jail", ROOT "/store", 3, err, sizeof err);
        _exit(rc == 0 ? 0 : 1);
    }
    if (pid < 0) return;

    /* Wait for the socket to appear rather than sleeping a fixed time. */
    bool up = false;
    for (int i = 0; i < 200 && !up; i++) {
        if (access(sockpath, F_OK) == 0) { up = true; break; }
        usleep(10000);
    }
    CHECK(up, "the socket appears");

    char out[8192] = {0};
    CHECK(sock_ask(sockpath, "{\"op\":\"ping\"}\n", out, sizeof out),
          "a client gets an answer over the socket");
    CHECK_STR_CONTAINS(out, "\"ok\":true", "and it is a success");
    CHECK(strchr(out, '\n') != NULL, "and it is newline-terminated");

    memset(out, 0, sizeof out);
    CHECK(sock_ask(sockpath, "{\"op\":\"snap\",\"label\":\"over socket\"}\n",
                   out, sizeof out), "snap over the socket is answered");
    CHECK_STR_CONTAINS(out, "\"ok\":true", "and succeeds");

    memset(out, 0, sizeof out);
    CHECK(sock_ask(sockpath, "{\"op\":\"exit\"}\n", out, sizeof out),
          "exit is answered");
    int st = 0;
    bool reaped = false;
    for (int i = 0; i < 300; i++) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) { reaped = true; break; }
        usleep(10000);
    }
    if (!reaped) kill(pid, SIGKILL);
    CHECK(reaped && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "the server stops cleanly when told to");
    CHECK(access(sockpath, F_OK) != 0, "and removes its socket on the way out");
}

/* ---------------- 5. the socket's permissions ---------------- */

static void test_socket_permissions(void) {
    printf("the socket is not world-writable\n");
    char sockpath[256];
    snprintf(sockpath, sizeof sockpath, "%s/perm/sup.sock", ROOT);
    rmrf(ROOT "/perm");

    pid_t pid = fork();
    if (pid == 0) {
        char err[256] = {0};
        (void)sup_serve(sockpath, ROOT "/jail", ROOT "/store", 1, err, sizeof err);
        _exit(0);
    }
    if (pid < 0) { CHECK(0, "fork for the server"); return; }

    bool up = false;
    for (int i = 0; i < 200 && !up; i++) {
        if (access(sockpath, F_OK) == 0) { up = true; break; }
        usleep(10000);
    }
    CHECK(up, "the socket appears");

    struct stat st;
    CHECK(stat(sockpath, &st) == 0, "it can be stat'd");
    /* 0600: the socket is a capability, and a capability that anyone can pick up
     * is not one. */
    CHECK((st.st_mode & 0777) == 0600, "the socket is 0600");
    if ((st.st_mode & 0777) != 0600)
        printf("      mode is %04o\n", st.st_mode & 0777);

    struct stat dst;
    char dir[256];
    snprintf(dir, sizeof dir, "%s/perm", ROOT);
    CHECK(stat(dir, &dst) == 0, "its directory can be stat'd");
    CHECK((dst.st_mode & 0777) == 0700, "and the directory is 0700");
    if ((dst.st_mode & 0777) != 0700)
        printf("      dir mode is %04o\n", dst.st_mode & 0777);

    /* Let the server finish by using its one request. */
    char out[1024] = {0};
    (void)sock_ask(sockpath, "{\"op\":\"ping\"}\n", out, sizeof out);
    int status = 0;
    bool reaped = false;
    for (int i = 0; i < 300; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid) { reaped = true; break; }
        usleep(10000);
    }
    if (!reaped) kill(pid, SIGKILL);
    CHECK(reaped, "the server finished after its request budget");
}

int main(void) {
    printf("== test_supervisor ==\n");
    rmrf(ROOT);
    mkdirs(ROOT "/jail");
    mkdirs(ROOT "/store");

    test_protocol_errors();
    test_label_escaping();
    test_round_trip();
    test_live_socket();
    test_socket_permissions();

    printf("%s: %d checks, %d failed\n", g_failed ? "FAIL" : "PASS",
           g_checks, g_failed);
    rmrf(ROOT);
    return g_failed ? 1 : 0;
}
