/* aios/json.h — M1 minimal JSON (parse + build + serialize). Zero deps. */
#ifndef AIOS_JSON_H
#define AIOS_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ
} json_type;

typedef struct json_val json_val;

/* ---- parse ---- */
json_val  *json_parse(const char *s, size_t len);      /* NULL on error  */
json_val  *json_parse_file(const char *path);          /* NULL on error  */

/* ---- constructors (take ownership of children) ---- */
json_val  *json_new_null(void);
json_val  *json_new_bool(int b);                       /* 0/1            */
json_val  *json_new_num(double d);
json_val  *json_new_str(const char *s);                /* copies         */
json_val  *json_new_str_len(const char *s, size_t n);  /* copies         */
json_val  *json_new_arr(void);
json_val  *json_new_obj(void);

/* ---- building ---- */
void       json_obj_set(json_val *obj, const char *key, json_val *v);
void       json_arr_push(json_val *arr, json_val *v);

/* ---- access ---- */
json_type  json_type_of(const json_val *v);
json_val  *json_obj_get(const json_val *obj, const char *key);
/* Remove and return the member value (caller frees). NULL if absent. */
json_val  *json_obj_take(json_val *obj, const char *key);
const char *json_str(const json_val *v);               /* NULL if not str*/
double     json_num(const json_val *v);                /* 0 if not num   */
int        json_bool(const json_val *v);
size_t     json_arr_len(const json_val *arr);
json_val  *json_arr_get(const json_val *arr, size_t i);

/* ---- serialize ---- */
char      *json_dump(const json_val *v);               /* malloc'd       */

/* ---- helpers ---- */
/* args convenience: {"expr":"1+2"} -> json_obj_set(o,"expr",json_new_str(...)) */
void       json_free(json_val *v);

#ifdef __cplusplus
}
#endif
#endif /* AIOS_JSON_H */
