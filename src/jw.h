/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * JSON helpers for documents Lume writes and reads back (semantic catalog, reports, AI proposals):
 * a growable writer with correct string escaping, and typed lookups over json.h's node table.
 * Pure C, no SDL.
 */
#include <stdbool.h>
#include <stddef.h>
#include "json.h"

typedef struct Jw { char *b; size_t w, cap; bool comma[64]; int depth; bool oom, ak; } Jw;

void jw_init(Jw *j);
char *jw_take(Jw *j);                    /* the text (malloc'd, caller frees); j is reset */
void jw_free(Jw *j);
void jw_obj(Jw *j);                      /* { */
void jw_end_obj(Jw *j);                  /* } */
void jw_arr(Jw *j);                      /* [ */
void jw_end_arr(Jw *j);                  /* ] */
void jw_key(Jw *j, const char *k);       /* "k": (next value follows) */
void jw_str(Jw *j, const char *s);       /* string value (NULL = null) */
void jw_int(Jw *j, long long v);
void jw_num(Jw *j, double v);            /* shortest round-trip form; NaN / inf = null */
void jw_bool(Jw *j, bool v);
void jw_null(Jw *j);
void jw_raw(Jw *j, const char *s);       /* a value already in JSON form */
/* key + value shortcuts */
void jw_kstr(Jw *j, const char *k, const char *s);
void jw_kint(Jw *j, const char *k, long long v);
void jw_knum(Jw *j, const char *k, double v);
void jw_kbool(Jw *j, const char *k, bool v);

/* ---- reading (node indices of a parsed Json) ---- */
int jr_get(const Json *j, int obj, const char *key);           /* member node, -1 when absent / not an object */
int jr_child(const Json *j, int arr, int i);                   /* i-th element, -1 */
bool jr_str(const Json *j, int obj, const char *key, char *out, size_t n);   /* false when absent / not a string ("" written) */
char *jr_strdup(const Json *j, int obj, const char *key);      /* malloc'd, NULL when absent */
bool jr_int(const Json *j, int obj, const char *key, long long *out);
bool jr_num(const Json *j, int obj, const char *key, double *out);
bool jr_bool(const Json *j, int obj, const char *key, bool *out);
void jr_node_str(const Json *j, int node, char *out, size_t n); /* a string node's value */
/* true when every member key of obj is in allowed (NULL-terminated); first unknown in bad */
bool jr_keys_allowed(const Json *j, int obj, const char *const *allowed, char *bad, size_t bn);
/* first top-level {...} object in free text (model answers may wrap JSON in prose / fences) */
bool jr_find_object(const char *text, size_t *off, size_t *len);
