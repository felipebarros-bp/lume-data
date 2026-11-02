/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Local diagnostics of an object source (OBJ-022 / 024 / 026 / 027 / 028 / 029 / 032)
 * ===================================================================================
 *
 * Runs without the server: lexical errors, the CREATE header against the object of the tab, the
 * verb the engine needs (with a quick fix), block balance in procedural code (inside $$ bodies
 * too), inspections (SELECT * in views, DELETE / UPDATE without WHERE, dynamic SQL, parameters
 * shadowing columns) and, when the caller passes the catalog, unknown table references.
 *
 * Every finding says where it comes from (parser / metadata), how sure it is (exact / mapped /
 * approximate) and, when a safe textual fix exists, the edit that applies it. Diagnostics never
 * change the text and never run SQL. Pure C, safe on a worker thread.
 */
#include "objmodel.h"

typedef struct ObjDiagCtx {
    int dialect;
    const ObjKey *key;
    const ObjSnapshot *base;               /* optional: signature / return type comparisons */
    /* catalog for reference checks (OBJ-027); NULL = not checked. Names as the engine stores
     * them, "name" and / or "schema.name". */
    const char *const *known; int nknown;
    /* columns of known tables for shadowing checks: "table.column" entries; NULL = skipped */
    const char *const *columns; int ncolumns;
} ObjDiagCtx;

/* Writes up to max diagnostics, most severe first in text order; returns how many. */
int obj_diagnose(const char *text, int len, const ObjDiagCtx *c, ObjDiag *out, int max);
/* Names a routine declares (OBJ-017): its parameters and the variables of its declaration
 * section (PL/pgSQL DECLARE inside the $$ body, PL/SQL IS / AS before BEGIN, T-SQL / MySQL
 * DECLARE). type is the declared type as written. */
typedef struct ObjSymbol { char name[80]; char type[80]; bool parameter, out_only; } ObjSymbol;   /* out_only: OUT parameter (not numbered as $n) */
int obj_symbols(const char *text, int len, int dialect, ObjSymbol *out, int max);
/* Refactorings (OBJ-021). Each returns the whole new text (malloc'd) or NULL with the reason in
 * why; the caller applies it as one undo step after showing what changes. */
/* The parenthesized subquery at [sel_start, sel_end) (in FROM / JOIN) becomes a CTE `name` of
 * its statement; an existing WITH gets one more entry. */
char *obj_extract_cte(const char *text, int len, int dialect, int sel_start, int sel_end, const char *name, char *why, int whyn);
/* PostgreSQL routines: $1, $2 ... in the body become the parameters' names (OUT ones are not
 * numbered); *count says how many were replaced. */
char *obj_named_params(const char *text, int len, int dialect, int *count, char *why, int whyn);
/* Unqualified column names become alias.column when exactly one table of the statement's FROM /
 * JOIN has that column (cols: "table.column" from the catalog). Ambiguous or unknown names are
 * left alone (never guessed); *count says how many were qualified. */
char *obj_qualify_columns(const char *text, int len, int dialect, const char *const *cols, int ncols, int *count, char *why, int whyn);
/* Applies a diagnostic's quick fix to text (malloc'd result, *out_len set); NULL when it has none. */
char *obj_apply_fix(const char *text, int len, const ObjDiag *dg, int *out_len);
