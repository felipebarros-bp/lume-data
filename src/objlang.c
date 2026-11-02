/* SPDX-License-Identifier: AGPL-3.0-only */
/* Language service registry and the built-in SQL service: see objlang.h. */
#include "objlang.h"
#include <stdio.h>
#include <string.h>

#define LANG_MAX 16
static const LangService *services[LANG_MAX];
static int nservices; static bool builtin;

bool lang_register(const LangService *s) {
    if (!builtin) { builtin = true; services[nservices++] = &lang_sql; }
    if (!s || s->api_version != LANG_API || !s->id || !s->handles || !s->lex) return false;
    for (int i = 0; i < nservices; i++) if (!strcmp(services[i]->id, s->id)) { services[i] = s; return true; }
    if (nservices >= LANG_MAX) return false;
    services[nservices++] = s;
    return true;
}

const LangService *lang_for(int dialect, const char *ext) {
    if (!builtin) lang_register(NULL);
    for (int i = nservices - 1; i >= 0; i--) if (services[i]->handles(dialect, ext)) return services[i];
    return NULL;
}

/* built-in SQL */
static bool sql_handles(int d, const char *ext) {
    if (ext) return !strcmp(ext, "sql") || !strcmp(ext, "pks") || !strcmp(ext, "pkb") || !strcmp(ext, "pls");
    return d >= 0 && d < dialect_count();
}
static bool sql_fmt(const char *t, int len, int d, const SqlStyle *st, int s, int e, FmtResult *out, const LumeCancel *c) {
    if (lume_cancelled(c)) { memset(out, 0, sizeof *out); snprintf(out->note, sizeof out->note, "Cancelled."); out->refused = true; return false; }
    return sql_format(t, len, d, st, s, e, out);
}
static int sql_diag(const char *t, int len, const ObjDiagCtx *ctx, ObjDiag *out, int max, const LumeCancel *c) {
    if (lume_cancelled(c)) return 0;
    return obj_diagnose(t, len, ctx, out, max);
}
const LangService lang_sql = { LANG_API, "lume.sql", "SQL", sql_handles, sql_lex, sql_fmt, sql_diag };
