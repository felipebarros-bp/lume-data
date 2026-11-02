/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Lume Intelligence for reports: typed tool contract, context consent and the model's answer
 * (LI-009..014 / 028 / 032 / 051 / 059 / 079 / 080 / 084 / 089 / 090). Headless.
 *
 *   The model gets the semantic catalog (and, when the user ticks them, the report structure, the
 *   selected card or its aggregated values) and must answer with ONE JSON object: a clarifying
 *   question, a proposal of typed report actions, or "cannot answer". It never receives raw rows
 *   (LI-032.1), never writes SQL (queries are compiled by sem.c from catalog ids), and nothing it
 *   returns runs before Lume validated it and the user approved the plan (LI-012). Text from the
 *   catalog / report is wrapped as untrusted data; instructions inside it change nothing because the
 *   answer is parsed against a closed schema and every action is revalidated (LI-084).
 *
 *   Tools (spec 7.1) are a registry with typed arguments, level, operation, approval and cost.
 *   Calls are validated before reaching any driver (LI-090.2); unknown tools are refused.
 */
#include <stdbool.h>
#include <stddef.h>
#include "json.h"
#include "sem.h"
#include "rpt.h"

/* ---- tool registry ---- */
enum { TL_METADATA, TL_POLICY, TL_DATA, TL_LOCAL, TL_SERVICE };
enum { TO_READ, TO_DRAFT, TO_VALIDATE, TO_PREVIEW, TO_EXPORT, TO_PUBLISH };
enum { AT_STRING, AT_INT, AT_BOOL, AT_OBJECT, AT_ARRAY };
typedef struct AgArg { const char *name; int type; bool required; int maxlen; const char *const *one_of; } AgArg;
typedef struct AgTool {
    const char *name, *purpose;
    int level, op;
    bool approval;             /* the user approves each run (data reads, exports) */
    bool available;            /* false: needs a backend this desktop does not have (publish_report) */
    const char *cost;          /* what it costs: "metadata", "one bounded query", ... */
    const AgArg *args; int nargs;
} AgTool;
int ag_tools(const AgTool **out);
const AgTool *ag_tool(const char *name);
/* arguments (an object node) against the tool's schema: required, types, lengths, enums, no extra keys */
bool ag_validate_call(const char *tool, const Json *j, int args, char *why, int n);
const char *ag_level_name(int l);

/* ---- context the user sees and edits before sending (LI-010 / 079) ---- */
enum { CX_CATALOG, CX_DIMENSIONS, CX_TABLES, CX_REPORT, CX_CARD, CX_VALUES, CX_SCHEMA, CX_CATVALUES, CX_EDITOR, CX_RESULT, CX_N };
typedef struct AgContext {
    bool on[CX_N];             /* defaults: catalog, dimensions, report structure, selected card on; table names and values off */
    const char *question;
    const char *card_id;       /* selected card, "" none */
    const char *values_json;   /* aggregated values of the selected card (only when on[CX_VALUES]) */
    const char *history;       /* earlier turns of this conversation (the user's words and the assistant's replies), "" none */
    const char *design_text;   /* the report's current look (theme summary, logo colours as hex), "" none */
    const char *schema_text;   /* the structure of the report's connection (tables, columns, types, keys; never rows), when on[CX_SCHEMA] */
    const char *glossary;      /* the workspace glossary (business terms -> where they live), "" none */
} AgContext;
const char *ag_ctx_label(int item);
bool ag_ctx_sensitive(int item);   /* needs a separate consent (values, table names) */
void ag_ctx_default(AgContext *x);
/* the exact user message that would be sent (the preview shows this string, LI-010.2) */
char *ag_message(const SemCatalog *c, const Report *r, int page, const AgContext *x);
char *ag_message_core(const SemCatalog *c, const Report *r, int page, const AgContext *x);   /* without the glossary */
const char *ag_system_prompt(void);
/* ---- the assistant in a query tab (Lume Intelligence foundation) ---- */
typedef struct AgSqlCtx {
    const char *question, *history;
    const char *dialect, *conn_name, *tab_title;
    const char *sql, *selection;          /* the tab's text (when on[CX_EDITOR]), the selected part ("" none) */
    const char *last_error, *last_sql;    /* the last run of this tab, when it failed */
    const char *result_cols;              /* the last result's column names and types ("" none) */
    const char *result_summary;           /* per-column counts / nulls / min / max of the current result (its own consent) */
    const char *plan;                     /* the execution plan captured in Lume for this connection (sanitized), NULL none */
    const char *validation;               /* results of the checks Lume ran for the model (plans, results, times), NULL none */
    const char *schema_text, *glossary;   /* structure picked for the question (consented); workspace glossary */
} AgSqlCtx;
const char *ag_sql_system_prompt(void);
char *ag_sql_message(const AgSqlCtx *x);

/* ---- the model's answer ---- */
enum { AA_NONE, AA_CLARIFY, AA_PROPOSAL, AA_CANNOT, AA_NEED_SCHEMA, AA_SEARCH_SCHEMA, AA_DESCRIBE_TABLES, AA_EDIT_SQL, AA_NEW_TAB, AA_EXPLAIN, AA_DESIGN_TABLE, AA_CREATE_REPORT, AA_VALIDATE };
typedef struct AgAnswer {
    int type;
    char question[300];        /* clarify */
    char options[6][48]; int nopt;
    char summary[300], rationale[600], reason[300];
    char terms[300];
    char target[16];           /* edit_sql: selection | statement | all */
    char *text;                /* edit_sql / new_tab: the SQL; explain: the explanation (malloc'd) */           /* search_schema: words; describe_tables: comma-separated SCHEMA.TABLE names */
    char period[24];           /* proposal: optional report period preset */
    char *proposal;            /* the proposal object (malloc'd) for rpt_apply_proposal */
    int nactions;
    char err[300];             /* why the answer was refused */
} AgAnswer;
/* parses the model text: exactly one JSON object of a known type, options that are catalog metric
 * ids, a known period preset, and actions whose tool exists. false with a.err otherwise. */
bool ag_parse_answer(const char *text, const SemCatalog *c, AgAnswer *a);
void ag_answer_free(AgAnswer *a);
bool ag_period_ok(const char *preset);
/* edit_sql / new_tab / explain / clarify (free options) / search_schema / describe_tables / cannot_answer */
bool ag_parse_sql_answer(const char *text, AgAnswer *a);
