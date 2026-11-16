/* tests/test_json.c — M1 JSON unit tests. */
#include "t.h"
#include "aios/json.h"

static void run(void) {
    /* basic roundtrip */
    const char *src =
        "{\"name\":\"ai-os\",\"n\":-3.5,\"ok\":true,\"none\":null,"
        "\"list\":[1,\"two\",false,{\"deep\":\"value\\n\"}],"
        "\"uni\":\"\\u00e9\\u4e2d\"}";
    json_val *v = json_parse(src, strlen(src));
    CHECK(v != NULL, "parse valid json");
    CHECK_STR_CONTAINS(json_str(json_obj_get(v, "name")), "ai", "obj get str");
    json_val *n = json_obj_get(v, "n");
    CHECK(json_type_of(n) == JSON_NUM && json_num(n) == -3.5, "num value");
    CHECK(json_bool(json_obj_get(v, "ok")) == 1, "bool value");
    json_val *list = json_obj_get(v, "list");
    CHECK(json_arr_len(list) == 4, "array len");
    json_val *deep = json_arr_get(list, 3);
    CHECK_STR_CONTAINS(json_str(json_obj_get(deep, "deep")), "value\n",
                       "escape roundtrip");
    CHECK_STR_CONTAINS(json_str(json_obj_get(v, "uni")), "\xc3\xa9", "utf8 latin");
    CHECK_STR_CONTAINS(json_str(json_obj_get(v, "uni")), "\xe4\xb8\xad", "utf8 cjk");

    char *dump = json_dump(v);
    CHECK(dump != NULL, "dump");
    json_val *v2 = json_parse(dump, strlen(dump));
    CHECK(v2 != NULL, "reparse dump");
    char *dump2 = json_dump(v2);
    CHECK(dump && dump2 && strcmp(dump, dump2) == 0, "dump is canonical");
    free(dump);
    free(dump2);
    json_free(v);
    json_free(v2);

    /* invalid inputs */
    CHECK(json_parse("{", 1) == NULL, "unterminated obj rejected");
    CHECK(json_parse("[1,]", 4) == NULL, "trailing comma rejected");
    CHECK(json_parse("{\"a\":1} x", 9) == NULL, "trailing garbage rejected");
    CHECK(json_parse("nul", 3) == NULL, "short null rejected");

    /* building + obj_take */
    json_val *o = json_new_obj();
    json_obj_set(o, "a", json_new_num(1));
    json_obj_set(o, "b", json_new_str("bee"));
    json_obj_set(o, "a", json_new_num(2));      /* overwrite */
    json_val *taken = json_obj_take(o, "b");
    CHECK(taken != NULL && strcmp(json_str(taken), "bee") == 0, "obj_take");
    json_free(taken);
    CHECK(json_obj_get(o, "b") == NULL, "taken key gone");
    CHECK(json_num(json_obj_get(o, "a")) == 2, "overwrite worked");
    json_free(o);

    /* big-ish structure */
    json_val *arr = json_new_arr();
    for (int i = 0; i < 100; i++) json_arr_push(arr, json_new_num(i));
    CHECK(json_arr_len(arr) == 100, "arr push 100");
    char *ad = json_dump(arr);
    CHECK(ad && strlen(ad) > 200, "arr dump len");
    free(ad);
    json_free(arr);
}

TEST_MAIN()
