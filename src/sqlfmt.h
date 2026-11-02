/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Conservative SQL formatter (OBJ-012 / 013 / 015 / 016 / 077)
 * =============================================================
 *
 * Re-spaces SQL statements: keyword case, one clause per line (SELECT, FROM, WHERE, JOIN, ...),
 * indentation inside parentheses, spacing around commas and operators. It never touches what it
 * does not understand:
 *
 *   - strings (dynamic SQL included), quoted identifiers and comments are copied verbatim;
 *   - PostgreSQL $tag$ bodies (PL/pgSQL) are copied verbatim (reported in the note);
 *   - text holding a procedural block outside such a body (BEGIN ... END in PL/SQL, T-SQL or a
 *     SQLite trigger) is returned unchanged with an explanation.
 *
 * Safety net: the output is lexed again and must hold exactly the same significant tokens as the
 * input (identical text; keywords compared case-insensitively). Anything else is a formatter bug:
 * the original text is returned, refused = true, and the note says so. Formatting never runs SQL.
 *
 * Selection formatting widens the range to whole statements when it cuts one, and says so; text
 * outside the (widened) range is untouched.
 */
#include <stdbool.h>

typedef struct SqlStyle {
    int kw_case;          /* 0 UPPER, 1 lower, 2 as written */
    int indent;           /* spaces per level (ignored with tabs) */
    bool tabs;
    bool clause_newlines; /* SELECT / FROM / WHERE ... start a new line */
} SqlStyle;

typedef struct FmtResult {
    char *text; int len;      /* the whole document after formatting (malloc'd) */
    bool changed, refused;    /* refused: the original is returned */
    int range_start, range_end;   /* what was formatted (after widening), in the input */
    char note[240];           /* what happened, for the status bar / Output */
} FmtResult;

void sql_style_default(SqlStyle *s);
/* sel_start < 0: the whole document. */
bool sql_format(const char *text, int len, int dialect, const SqlStyle *st, int sel_start, int sel_end, FmtResult *out);
void fmt_free(FmtResult *r);
/* The equivalence check alone: true when a and b hold the same significant tokens. */
bool sql_same_tokens(const char *a, int alen, const char *b, int blen, int dialect);
