/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "lume.h"
#include "db.h"

/* SQL completion. Reads only the in-memory schema cache (never the database) and ranks text
 * matches first, with per-connection usage as the tie-breaker. See docs/autocomplete-rules.md. */

/* Candidate kinds; with a typed prefix, equal matches are ordered by kind in this order. */
enum { CAND_COLUMN, CAND_ALIAS, CAND_TABLE, CAND_VIEW, CAND_FUNC, CAND_KEYWORD };
#define COMPLETE_MAX 48   /* size of the candidate array the editor passes to complete_query */

/* One completion proposal. */
typedef struct Cand {
    char text[80];      /* inserted into the editor */
    char detail[96];    /* right-hand hint: type, owning table, "keyword" */
    char key[128];      /* usage key, empty when not learnable */
    int kind;           /* CAND_* */
    int match;          /* text match score, higher is better */
    float usage;        /* usage score, tie-breaker only */
} Cand;

typedef struct Completer Completer;

/* Creates an empty completer (no schema, usage not persisted). Free with complete_destroy, which
 * also saves pending usage. The schema passed to complete_set_schema is never freed here. */
Completer *complete_create(void);
void complete_destroy(Completer *c);
/* Points the completer at the schema cache (owned by the caller) and loads the usage file
 * for `location`. Pass NULL/0 to clear. */
void complete_set_schema(Completer *c, DbTable *tables, int ntables, const char *location,
                         bool (*load_cols)(void *ctx, DbTable *t), void *ctx);
/* Learns object usage and token n-grams from SQL text (one or more statements). */
void complete_learn_statement(Completer *c, const char *sql, int len);
/* Learns from a proposal accepted in the popup. */
void complete_learn_choice(Completer *c, const Cand *cand);
/* Fills candidates for `stmt` with the cursor at byte `cursor`. `explicit` is true when the
 * user asked for the popup (⌃Space); otherwise an empty prefix in a free context yields none.
 * `*prefix_len` receives the number of bytes before the cursor the proposal replaces. */
int complete_query(Completer *c, const char *stmt, int len, int cursor, bool explicit, Cand *out, int max, int *prefix_len);
/* Saves the usage file when changes are pending and the debounce elapsed. Returns true while a
 * save is still pending (caller should wake up again later). */
bool complete_tick(Completer *c);
/* Extra proposals from another module (the object editor: the routine's parameters and variables,
 * OBJ-017). fn fills up to max entries for the active tab; NULL removes it. */
typedef struct CompleteExtra { char text[80]; char detail[96]; } CompleteExtra;
void complete_set_extra(int (*fn)(void *ud, CompleteExtra *out, int max), void *ud);
