/* tests/test_sandbox.c — M4 tool registry + sandboxed execution. */
#include "t.h"
#include "aios/tools.h"
#include "aios/json.h"
#include <sys/stat.h>
#include <unistd.h>

static json_val *call(tool_reg *tr, const char *tool, const char *args_json,
                      aios_status *st_out, char *err, size_t ecap) {
    json_val *args = json_parse(args_json, strlen(args_json));
    if (!args) args = json_new_obj();
    json_val *res = NULL;
    aios_status st = tool_exec(tr, tool, args, &res, err, ecap);
    if (st_out) *st_out = st;
    return res;
}

static void run(void) {
    const char *jail = "build/test-jail";
    const char *store = "build/test-tool-store";
    (void)!system("rm -rf build/test-jail build/test-tool-store");
    (void)!system("mkdir -p build/test-jail build/test-tool-store");
    (void)!system("printf 'hello aios sandbox\\n' > build/test-jail/hello.txt");

    mem_ctx *mem = mem_open(store);
    CHECK(mem != NULL, "mem for memory tools");
    tool_reg *tr = tool_reg_create(jail, store, "tester", mem);
    CHECK(tr != NULL, "registry");
    tool_reg_install_default(tr);
    CHECK(tool_reg_count(tr) == 8, "default toolset size");

    char err[512];
    aios_status st;

    /* calc */
    json_val *res = call(tr, "calc", "{\"expr\":\"12*(7+5)\"}", &st, err, sizeof err);
    CHECK(st == AIOS_OK, "calc ok");
    CHECK(res && json_num(json_obj_get(res, "value")) == 144.0, "calc value 144");
    json_free(res);

    res = call(tr, "calc", "{\"expr\":\"2^10/4\"}", &st, err, sizeof err);
    CHECK(st == AIOS_OK && json_num(json_obj_get(res, "value")) == 256.0,
          "calc power/div");
    json_free(res);

    res = call(tr, "calc", "{\"expr\":\"1/0\"}", &st, err, sizeof err);
    CHECK(st != AIOS_OK, "calc div-by-zero rejected");
    json_free(res);

    /* fs roundtrip inside the jail */
    res = call(tr, "fs.write",
               "{\"path\":\"out/test.md\",\"content\":\"# by agent\\nline2\"}",
               &st, err, sizeof err);
    CHECK(st == AIOS_OK, "fs.write ok");
    json_free(res);

    res = call(tr, "fs.read", "{\"path\":\"out/test.md\"}", &st, err, sizeof err);
    CHECK(st == AIOS_OK, "fs.read ok");
    CHECK_STR_CONTAINS(json_str(json_obj_get(res, "content")), "line2",
                       "fs content roundtrip");
    json_free(res);

    res = call(tr, "fs.list", "{\"path\":\".\"}", &st, err, sizeof err);
    CHECK(st == AIOS_OK, "fs.list ok");
    json_val *entries = json_obj_get(res, "entries");
    CHECK(json_arr_len(entries) >= 2, "listing shows files");
    json_free(res);

    /* path traversal rejected */
    res = call(tr, "fs.read", "{\"path\":\"../build/test-jail/hello.txt\"}",
               &st, err, sizeof err);
    CHECK(st != AIOS_OK, "dotdot rejected");
    json_free(res);
    res = call(tr, "fs.read", "{\"path\":\"/etc/passwd\"}", &st, err, sizeof err);
    CHECK(st != AIOS_OK, "absolute path rejected");
    json_free(res);

    /* jail isolation: hello.txt visible, /tmp files not */
    res = call(tr, "fs.read", "{\"path\":\"hello.txt\"}", &st, err, sizeof err);
    CHECK(st == AIOS_OK, "jail file readable");
    CHECK_STR_CONTAINS(json_str(json_obj_get(res, "content")), "hello aios",
                       "jail content");
    json_free(res);

    /* native tools */
    res = call(tr, "proc.info", "{}", &st, err, sizeof err);
    CHECK(st == AIOS_OK, "proc.info ok");
    CHECK_STR_CONTAINS(json_str(json_obj_get(res, "version")), AIOS_VERSION,
                       "version");
    json_free(res);

    CHECK(mem_remember(mem, MEM_EPISODIC,
                       "the sandbox test wrote test.md into out/", NULL) == AIOS_OK,
          "seed memory");
    res = call(tr, "memory.search",
               "{\"query\":\"what did the sandbox write?\",\"k\":2}",
               &st, err, sizeof err);
    CHECK(st == AIOS_OK, "memory.search ok");
    json_val *results = json_obj_get(res, "results");
    CHECK(json_arr_len(results) >= 1, "memory.search returned hits");
    json_free(res);

    /* timeout: parent must kill a wedged worker (sleep 5s, budget 1s) */
    tool_set_sandbox_timeout(1.0);
    double t0 = aios_mono_ms();
    res = call(tr, "selftest.sleep", "{\"ms\":5000}", &st, err, sizeof err);
    double dt = aios_mono_ms() - t0;
    CHECK(st == AIOS_ERR_TIMEOUT, "wedged worker killed");
    CHECK_STR_CONTAINS(err, "timed out", "timeout error message");
    CHECK(dt < 4500.0, "timeout enforced promptly");
    json_free(res);
    tool_set_sandbox_timeout(10.0);

    /* unknown tool */
    res = call(tr, "no.such.tool", "{}", &st, err, sizeof err);
    CHECK(st == AIOS_ERR_NOTFOUND, "unknown tool notfound");
    json_free(res);

    /* audit log exists and has our calls */
    struct stat sb;
    char audit_path[256];
    snprintf(audit_path, sizeof audit_path, "%s/audit.log", store);
    CHECK(stat(audit_path, &sb) == 0 && sb.st_size > 100, "audit log written");

    tool_reg_free(tr);
    mem_close(mem);
}

TEST_MAIN()
