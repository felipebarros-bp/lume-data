/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Line diff (object source editor, OBJ-034 / 037 / 066)
 * =====================================================
 *
 * Myers' O((N + M) * D) algorithm over lines. The result is a list of operations, each a run
 * of equal, deleted (only in A) or inserted (only in B) lines, in order. A budget on D (the
 * number of edits) keeps a pathological input from running away: past it the diff stops and
 * says so (the caller shows a plain "too different" view, never a wrong one).
 *
 * Whitespace: compared exactly by default; ignore_ws compares lines with all whitespace
 * removed, for viewing only (the review and the deployment always use the exact text).
 *
 * Three-way (conflicts): diff3_conflicts tells which lines of base were changed by both local
 * and remote, the "both changed" regions shown by the conflict view.
 *
 * Pure C, no SDL; safe on a worker thread.
 */
#include <stdbool.h>

enum { DIFF_EQ, DIFF_DEL, DIFF_INS };
typedef struct DiffOp { int kind; int a, b, n; } DiffOp;   /* lines a.. of A / b.. of B, n lines */

typedef struct TextLines { const char *text; int len; int *start; int n; } TextLines;   /* line i = text[start[i] .. start[i+1]) */
void lines_split(TextLines *l, const char *text, int len);
void lines_free(TextLines *l);
int line_len(const TextLines *l, int i);   /* without the line break */

typedef struct Diff {
    DiffOp *ops; int nops;
    int added, removed;          /* lines */
    bool too_large;              /* budget exceeded: ops empty */
    bool cancelled;              /* stopped through the cancel flag: ops empty */
} Diff;

bool text_diff(const TextLines *a, const TextLines *b, bool ignore_ws, int max_edits, Diff *out);
/* Same, stopping early when *cancel becomes non-zero (worker threads, AT-08). */
bool text_diff_ex(const TextLines *a, const TextLines *b, bool ignore_ws, int max_edits, Diff *out, const volatile int *cancel);
void diff_free(Diff *d);
/* Writes a unified-style text of the diff (for export / Output), "-" / "+" / " " prefixes. */
char *diff_unified(const TextLines *a, const TextLines *b, const Diff *d, int context);

/* Three-way: base vs local and base vs remote; marks each base line touched by both sides
 * (conflict[i] = true). Returns how many base lines conflict; -1 when a diff was too large. */
int diff3_conflicts(const TextLines *base, const TextLines *local, const TextLines *remote, bool *conflict, int max_edits);
