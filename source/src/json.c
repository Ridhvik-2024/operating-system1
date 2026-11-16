/* aios/json.c — minimal JSON parser/serializer (C11, no deps).
 * Supports: null, true/false, numbers (double), strings with escapes
 * (incl. \uXXXX), arrays, objects. SerDe is recursive. */
#include "aios/json.h"
#include "aios/types.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct json_member { char *key; json_val *val; } json_member;
typedef struct json_node {
    json_type type;
    /* bool */  int b;
    /* num  */  double num;
    /* str  */  char *str; size_t str_len;
    /* arr  */  json_val **items; size_t n, cap;
    /* obj  */  json_member *members; size_t nmem, capmem;
} json_node;

struct json_val { json_node d; }; /* opaque wrapper */

/* ---------- internal constructors ---------- */
static json_val *node_new(json_type t) {
    json_val *v = (json_val *)calloc(1, sizeof(json_val));
    if (!v) return NULL;
    v->d.type = t;
    return v;
}

void json_free(json_val *v) {
    if (!v) return;
    free(v->d.str);
    for (size_t i = 0; i < v->d.n; i++) json_free(v->d.items[i]);
    free(v->d.items);
    for (size_t i = 0; i < v->d.nmem; i++) {
        free(v->d.members[i].key);
        json_free(v->d.members[i].val);
    }
    free(v->d.members);
    free(v);
}

/* ---------- parser ---------- */
typedef struct { const char *s; size_t len, pos; bool err; } parser;

static void pskip(parser *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static int ppeek(parser *p) {
    return p->pos < p->len ? (unsigned char)p->s[p->pos] : -1;
}

static json_val *pvalue(parser *p);

static size_t utf8_encode(char *out, unsigned cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static unsigned hex4(parser *p) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int c = ppeek(p);
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else { p->err = true; return 0; }
        p->pos++;
        v = v * 16 + d;
    }
    return v;
}

static char *pstring(parser *p, size_t *out_len) {
    if (ppeek(p) != '"') { p->err = true; return NULL; }
    p->pos++;
    char *buf = (char *)malloc(p->len - p->pos + 8);
    if (!buf) { p->err = true; return NULL; }
    size_t n = 0;
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->s[p->pos];
        if (c == '"') { p->pos++; buf[n] = 0; if (out_len) *out_len = n; return buf; }
        if (c == '\\') {
            p->pos++;
            int e = ppeek(p);
            switch (e) {
            case '"': buf[n++] = '"'; p->pos++; break;
            case '\\': buf[n++] = '\\'; p->pos++; break;
            case '/': buf[n++] = '/'; p->pos++; break;
            case 'b': buf[n++] = '\b'; p->pos++; break;
            case 'f': buf[n++] = '\f'; p->pos++; break;
            case 'n': buf[n++] = '\n'; p->pos++; break;
            case 'r': buf[n++] = '\r'; p->pos++; break;
            case 't': buf[n++] = '\t'; p->pos++; break;
            case 'u': {
                p->pos++;
                unsigned cp = hex4(p);
                if (p->err) goto fail;
                if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 1 < p->len &&
                    p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                    p->pos += 2;
                    unsigned lo = hex4(p);
                    if (p->err) goto fail;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                n += utf8_encode(buf + n, cp);
                break;
            }
            default: p->err = true; goto fail;
            }
        } else {
            buf[n++] = (char)c;
            p->pos++;
        }
        if (n + 8 >= p->len - p->pos + 8) { /* can't overflow by construction */ }
    }
fail:
    free(buf);
    p->err = true;
    return NULL;
}

static json_val *pnumber(parser *p) {
    size_t start = p->pos;
    if (ppeek(p) == '-') p->pos++;
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
            c == '+' || c == '-') p->pos++;
        else break;
    }
    if (p->pos == start) { p->err = true; return NULL; }
    char tmp[64];
    size_t n = p->pos - start;
    if (n >= sizeof tmp) { p->err = true; return NULL; }
    memcpy(tmp, p->s + start, n);
    tmp[n] = 0;
    char *end = NULL;
    double d = strtod(tmp, &end);
    if (!end || *end) { p->err = true; return NULL; }
    json_val *v = node_new(JSON_NUM);
    if (v) v->d.num = d;
    else p->err = true;
    return v;
}

static void grow_items(json_node *d, size_t need) {
    if (need <= d->cap) return;
    size_t cap = d->cap ? d->cap * 2 : 8;
    while (cap < need) cap *= 2;
    json_val **ni = (json_val **)realloc(d->items, cap * sizeof(*ni));
    if (!ni) return;
    d->items = ni; d->cap = cap;
}

static void grow_members(json_node *d, size_t need) {
    if (need <= d->capmem) return;
    size_t cap = d->capmem ? d->capmem * 2 : 8;
    while (cap < need) cap *= 2;
    json_member *nm = (json_member *)realloc(d->members, cap * sizeof(*nm));
    if (!nm) return;
    d->members = nm; d->capmem = cap;
}

static json_val *pvalue(parser *p) {
    pskip(p);
    int c = ppeek(p);
    if (c < 0) { p->err = true; return NULL; }
    if (c == '{') {
        p->pos++;
        json_val *o = node_new(JSON_OBJ);
        if (!o) { p->err = true; return NULL; }
        pskip(p);
        if (ppeek(p) == '}') { p->pos++; return o; }
        for (;;) {
            pskip(p);
            size_t klen = 0;
            char *key = pstring(p, &klen);
            if (!key) { json_free(o); return NULL; }
            pskip(p);
            if (ppeek(p) != ':') { free(key); json_free(o); p->err = true; return NULL; }
            p->pos++;
            json_val *val = pvalue(p);
            if (!val) { free(key); json_free(o); return NULL; }
            grow_members(&o->d, o->d.nmem + 1);
            o->d.members[o->d.nmem].key = key;
            o->d.members[o->d.nmem].val = val;
            o->d.nmem++;
            pskip(p);
            if (ppeek(p) == ',') { p->pos++; continue; }
            if (ppeek(p) == '}') { p->pos++; return o; }
            json_free(o); p->err = true; return NULL;
        }
    }
    if (c == '[') {
        p->pos++;
        json_val *a = node_new(JSON_ARR);
        if (!a) { p->err = true; return NULL; }
        pskip(p);
        if (ppeek(p) == ']') { p->pos++; return a; }
        for (;;) {
            json_val *val = pvalue(p);
            if (!val) { json_free(a); return NULL; }
            grow_items(&a->d, a->d.n + 1);
            a->d.items[a->d.n++] = val;
            pskip(p);
            if (ppeek(p) == ',') { p->pos++; continue; }
            if (ppeek(p) == ']') { p->pos++; return a; }
            json_free(a); p->err = true; return NULL;
        }
    }
    if (c == '"') {
        size_t slen = 0;
        char *s = pstring(p, &slen);
        if (!s) return NULL;
        json_val *v = node_new(JSON_STR);
        if (!v) { free(s); p->err = true; return NULL; }
        v->d.str = s; v->d.str_len = slen;
        return v;
    }
    if (c == 't' && p->pos + 3 < p->len && strncmp(p->s + p->pos, "true", 4) == 0) {
        p->pos += 4;
        json_val *v = node_new(JSON_BOOL);
        if (v) v->d.b = 1; else p->err = true;
        return v;
    }
    if (c == 'f' && p->pos + 4 < p->len && strncmp(p->s + p->pos, "false", 5) == 0) {
        p->pos += 5;
        json_val *v = node_new(JSON_BOOL);
        if (v) v->d.b = 0; else p->err = true;
        return v;
    }
    if (c == 'n' && p->pos + 3 < p->len && strncmp(p->s + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return node_new(JSON_NULL);
    }
    return pnumber(p);
}

json_val *json_parse(const char *s, size_t len) {
    if (!s) return NULL;
    parser p = { s, len, 0, false };
    json_val *v = pvalue(&p);
    if (!v || p.err) { json_free(v); return NULL; }
    pskip(&p);
    if (p.pos != p.len) { json_free(v); return NULL; } /* trailing garbage */
    return v;
}

/* ---------- constructors ---------- */
json_val *json_new_null(void)        { return node_new(JSON_NULL); }
json_val *json_new_bool(int b)       { json_val *v = node_new(JSON_BOOL); if (v) v->d.b = !!b; return v; }
json_val *json_new_num(double d)     { json_val *v = node_new(JSON_NUM); if (v) v->d.num = d; return v; }
json_val *json_new_arr(void)         { return node_new(JSON_ARR); }
json_val *json_new_obj(void)         { return node_new(JSON_OBJ); }

json_val *json_new_str_len(const char *s, size_t n) {
    if (!s) return NULL;
    json_val *v = node_new(JSON_STR);
    if (!v) return NULL;
    v->d.str = (char *)malloc(n + 1);
    if (!v->d.str) { free(v); return NULL; }
    memcpy(v->d.str, s, n);
    v->d.str[n] = 0;
    v->d.str_len = n;
    return v;
}

json_val *json_new_str(const char *s) { return s ? json_new_str_len(s, strlen(s)) : json_new_null(); }

void json_obj_set(json_val *obj, const char *key, json_val *v) {
    if (!obj || obj->d.type != JSON_OBJ || !key || !v) { json_free(v); return; }
    for (size_t i = 0; i < obj->d.nmem; i++) {
        if (strcmp(obj->d.members[i].key, key) == 0) {
            json_free(obj->d.members[i].val);
            obj->d.members[i].val = v;
            return;
        }
    }
    grow_members(&obj->d, obj->d.nmem + 1);
    obj->d.members[obj->d.nmem].key = strdup(key);
    obj->d.members[obj->d.nmem].val = v;
    obj->d.nmem++;
}

void json_arr_push(json_val *arr, json_val *v) {
    if (!arr || arr->d.type != JSON_ARR || !v) { json_free(v); return; }
    grow_items(&arr->d, arr->d.n + 1);
    arr->d.items[arr->d.n++] = v;
}

/* ---------- access ---------- */
json_type json_type_of(const json_val *v) { return v ? v->d.type : JSON_NULL; }

json_val *json_obj_get(const json_val *obj, const char *key) {
    if (!obj || obj->d.type != JSON_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->d.nmem; i++)
        if (strcmp(obj->d.members[i].key, key) == 0) return obj->d.members[i].val;
    return NULL;
}

const char *json_str(const json_val *v)   { return v && v->d.type == JSON_STR ? v->d.str : NULL; }

json_val *json_obj_take(json_val *obj, const char *key) {
    if (!obj || obj->d.type != JSON_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->d.nmem; i++) {
        if (strcmp(obj->d.members[i].key, key) == 0) {
            json_val *v = obj->d.members[i].val;
            free(obj->d.members[i].key);
            memmove(&obj->d.members[i], &obj->d.members[i + 1],
                    (obj->d.nmem - i - 1) * sizeof(*obj->d.members));
            obj->d.nmem--;
            return v;
        }
    }
    return NULL;
}
double      json_num(const json_val *v)   { return v && v->d.type == JSON_NUM ? v->d.num : 0.0; }
int         json_bool(const json_val *v)  { return v && v->d.type == JSON_BOOL ? v->d.b : 0; }
size_t      json_arr_len(const json_val *a){ return a && a->d.type == JSON_ARR ? a->d.n : 0; }
json_val   *json_arr_get(const json_val *a, size_t i) {
    return a && a->d.type == JSON_ARR && i < a->d.n ? a->d.items[i] : NULL;
}

/* ---------- serializer ---------- */
typedef struct { char *buf; size_t len, cap; bool err; } sbuf;

static void sb_put(sbuf *b, const char *s, size_t n) {
    if (b->err) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *nb = (char *)realloc(b->buf, cap);
        if (!nb) { b->err = true; return; }
        b->buf = nb; b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

static void sb_puts(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static void dump_str(sbuf *b, const char *s) {
    sb_put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\b': sb_puts(b, "\\b");  break;
        case '\f': sb_puts(b, "\\f");  break;
        case '\n': sb_puts(b, "\\n");  break;
        case '\r': sb_puts(b, "\\r");  break;
        case '\t': sb_puts(b, "\\t");  break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof esc, "\\u%04x", c);
                sb_puts(b, esc);
            } else {
                sb_put(b, (const char *)&c, 1);
            }
        }
    }
    sb_put(b, "\"", 1);
}

static void dump_val(sbuf *b, const json_val *v) {
    if (!v || b->err) { sb_puts(b, "null"); return; }
    char numbuf[40];
    switch (v->d.type) {
    case JSON_NULL: sb_puts(b, "null"); break;
    case JSON_BOOL: sb_puts(b, v->d.b ? "true" : "false"); break;
    case JSON_NUM: {
        double d = v->d.num;
        if (d == (long long)d && fabs(d) < 9.0e15)
            snprintf(numbuf, sizeof numbuf, "%lld", (long long)d);
        else
            snprintf(numbuf, sizeof numbuf, "%.17g", d);
        sb_puts(b, numbuf);
        break;
    }
    case JSON_STR: dump_str(b, v->d.str); break;
    case JSON_ARR:
        sb_put(b, "[", 1);
        for (size_t i = 0; i < v->d.n; i++) {
            if (i) sb_put(b, ",", 1);
            dump_val(b, v->d.items[i]);
        }
        sb_put(b, "]", 1);
        break;
    case JSON_OBJ:
        sb_put(b, "{", 1);
        for (size_t i = 0; i < v->d.nmem; i++) {
            if (i) sb_put(b, ",", 1);
            dump_str(b, v->d.members[i].key);
            sb_put(b, ":", 1);
            dump_val(b, v->d.members[i].val);
        }
        sb_put(b, "}", 1);
        break;
    }
}

char *json_dump(const json_val *v) {
    sbuf b = { NULL, 0, 0, false };
    dump_val(&b, v);
    if (b.err) { free(b.buf); return NULL; }
    if (!b.buf) { b.buf = (char *)malloc(1); if (!b.buf) return NULL; b.buf[0] = 0; }
    return b.buf;
}

json_val *json_parse_file(const char *path) {
    char *data = NULL;
    size_t len = 0;
    if (aios_read_file(path, &data, &len, 64u * 1024 * 1024) != AIOS_OK) return NULL;
    json_val *v = json_parse(data, len);
    free(data);
    return v;
}
