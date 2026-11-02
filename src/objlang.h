/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Language services (OBJ-080): lexing, formatting and diagnostics behind one versioned contract
 * ============================================================================================
 *
 * The editor asks "the language service for this dialect / file" instead of calling the SQL
 * lexer, formatter and diagnostics directly, so a module can bring another language (or a
 * better SQL dialect) without changing the editor:
 *
 *   static const LangService my_lang = { LANG_API, "lume.markdown", "Markdown", handles, lex, NULL, diagnose };
 *   lang_register(&my_lang);       // from the module's init (panels_add_module)
 *
 * Rules of the contract:
 *   - inputs are text + dialect (+ server version, style, a cancel token); a service never sees
 *     the UI, a connection or the file system, and never runs SQL;
 *   - outputs are owned by the caller: FmtResult with fmt_free, diagnostics in the caller's array;
 *   - long work checks the cancel token and returns early (the result says it was cancelled);
 *   - api_version must equal LANG_API, or registration is refused.
 * The built-in "lume.sql" service covers every registered SQL dialect.
 */
#include "objdiag.h"
#include "sqlfmt.h"
#include "sqllex.h"

#define LANG_API 1

/* Set from the UI thread, read by the worker: the service stops at its next check. */
typedef struct LumeCancel { volatile int requested; } LumeCancel;
static inline bool lume_cancelled(const LumeCancel *c) { return c && c->requested; }

typedef struct LangService {
    int api_version;            /* LANG_API */
    const char *id;             /* "lume.sql" */
    const char *name;           /* "SQL" */
    /* true when the service handles this dialect (SQL) or file extension ("md"); either may be absent */
    bool (*handles)(int dialect, const char *ext);
    int (*lex)(const char *text, int len, int dialect, SqlTok *out, int max);
    /* NULL = no formatter: the editor offers no Format action (never a fictional one) */
    bool (*format)(const char *text, int len, int dialect, const SqlStyle *st, int sel_start, int sel_end, FmtResult *out, const LumeCancel *cancel);
    int (*diagnose)(const char *text, int len, const ObjDiagCtx *ctx, ObjDiag *out, int max, const LumeCancel *cancel);
} LangService;

bool lang_register(const LangService *s);
/* The most recently registered service that handles dialect / ext (modules override built-ins). */
const LangService *lang_for(int dialect, const char *ext);
extern const LangService lang_sql;
