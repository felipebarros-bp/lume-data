/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "lume.h"
/* SQL text editor (editor.c), one per editor tab. Positions passed in and out are byte offsets
 * into the whole text joined with '\n' unless stated otherwise; strings returned as malloc'd
 * are freed by the caller with free(). Main thread only. */

/* Creates an empty editor; free it with editor_destroy. */
Editor *editor_create(void);
void editor_destroy(Editor *e);
/* Replaces the whole text (NUL-terminated), clears undo history, caret at the top. */
void editor_set_text(Editor *e, const char *text);
void editor_insert(Editor *e, const char *s, int n);     /* at cursor, replaces selection */
char *editor_text(Editor *e, int *len);                   /* malloc'd */
char *editor_selection(Editor *e, int *len);              /* malloc'd or NULL */
char *editor_statement_at_cursor(Editor *e, int *len);    /* malloc'd or NULL */
bool editor_statement_lines(Editor *e, int *first, int *last); /* lines of the statement under the cursor, 0-based inclusive; false if none */
/* Edit-menu actions; cut / copy / paste go through the system clipboard. */
void editor_select_all(Editor *e);
void editor_cut(Editor *e);
void editor_copy(Editor *e);
void editor_paste(Editor *e);
void editor_cursor_pos(Editor *e, int *line, int *col);   /* 1-based, col in codepoints */
/* Execution correlation (OUT-008): the revision counter bumps on every text change; a
 * statement run is recorded with its revision and byte offsets, so "Go to SQL" can select
 * the exact text when the buffer is unchanged and say so when it is not. */
uint32_t editor_revision(Editor *e);
bool editor_select_range(Editor *e, int start, int end);          /* byte offsets; false when out of range */
char *editor_statement_at_cursor_pos(Editor *e, int *len, int *start);   /* like editor_statement_at_cursor, with its offset */
int editor_selection_start(Editor *e);                             /* byte offset of the selection start, -1 none */
/* Undo / redo one step (typing within a second is grouped into one step). */
/* Diagnostic marks (object editor): byte ranges with a severity (0 error, 1 warning, 2 info);
 * drawn only while the text is at the revision they were set for. */
#define EDITOR_MARKS_MAX 64
typedef struct EdMark { int start, end, severity; } EdMark;
void editor_set_marks(Editor *e, const EdMark *m, int n);
/* Object definition mode: no "statement under the cursor" band (the text is never run as a console). */
void editor_set_object_mode(Editor *e, bool on);
bool editor_object_mode(const Editor *e);
/* Read-only: typing, paste, cut, undo / redo and editing keys do nothing; selection and copy work. */
void editor_set_read_only(Editor *e, bool on);
bool editor_read_only(const Editor *e);
/* Highlighting rules of a dialect (DIALECT_* in sqllex.h): quoting, comments, batch separators. */
void editor_set_dialect(Editor *e, int dialect);
bool editor_goto_offset(Editor *e, int byte_offset);        /* caret to a byte offset, scrolled into view */
void editor_replace_all(Editor *e, const char *text, int len);   /* one undoable step (Format) */
void editor_undo(Editor *e);
void editor_redo(Editor *e);
/* Per frame: handles this frame's input when the editor is focused and draws it in `bounds`
 * (render pixels). Clicking it takes focus (a->focus = FOCUS_EDITOR). */
void editor_update(Editor *e, App *a, Rect bounds);
/* false: the editor draws only its text area (the shell draws tabs and the run hint) */
void editor_set_strip(Editor *e, bool draw);
/* Draws the completion popup; call after every panel so it stays on top. */
void editor_draw_overlay(Editor *e, App *a);
/* Opens the completion popup on demand (⌃Space). */
void editor_complete(Editor *e, App *a);
void editor_dump_completion(const Editor *e);
void editor_metadata_arrived(Editor *e, App *a);   /* objcat: columns arrived; a popup that waited for them is recomputed */   /* harness: completestate (the popup's proposals) */
void editor_scroll_state(const Editor *e, float *sx, float *widest, float *view_w);   /* harness: horizontal scroll, widest line, text width (px) */
