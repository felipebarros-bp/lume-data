/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Execution plans (EPL-001..084): normalized model, capture SQL per engine, parsers, layout,
 * evidence-based diagnostics, comparison and the sanitized context for Lume Intelligence.
 * Headless: no UI, no connection.
 *
 * Honesty rules (spec 1 / 4): a value is PRESENT, UNAVAILABLE or NOT_APPLICABLE - never a zero
 * standing in for "unknown". Costs are optimizer units of one engine (never milliseconds, never
 * compared across engines). Estimated plans carry no runtime. SQLite plans carry no costs, rows
 * or percentages. Unknown operators keep their original text.
 */
#include <stdbool.h>
#include <stdint.h>
#include "rs.h"

enum { PL_SQLITE, PL_PG, PL_ORACLE, PL_MYSQL, PL_MARIADB, PL_MSSQL, PL_NDIALECT };
enum { PM_ESTIMATED, PM_ACTUAL };
enum { PV_UNAVAILABLE, PV_PRESENT, PV_NA };
typedef struct PlanNum { uint8_t st; double v; } PlanNum;

int plan_dialect_for_driver(const char *driver);
const char *plan_dialect_name(int d);

/* EPL-021: what an engine (and version) can give */
typedef struct PlanCaps { bool estimated, actual, costs, rows, timing, buffers, warnings, raw; const char *why_no_actual; } PlanCaps;
PlanCaps plan_caps(int dialect);

typedef struct PlanProp { char key[64]; char *val; } PlanProp;
enum { PF_CARD = 1, PF_SCAN = 2, PF_SPILL = 4, PF_SORT = 8, PF_WARN = 16, PF_HOT = 32 };
typedef struct PlanNode {
    int id;                      /* index in Plan.nodes, stable for this snapshot */
    char pid[24];                /* provider's node id (PG: position; SQL Server NodeId; Oracle Id; SQLite id) */
    char op[128];                /* the engine's operator, unchanged */
    char kind[16];               /* scan, seek, join, sort, aggregate, filter, limit, temp, modify, result, other */
    char object[256];            /* table */
    char index[160];
    char predicate[1024];
    PlanNum est_rows, act_rows, loops, est_cost, est_startup, act_ms, act_first_ms, width, removed;
    int parent, first_child, next_sibling, nchildren, depth;
    PlanProp *props; int nprops;
    char warning[256];
    int flags;                   /* PF_* from the diagnostics */
    /* layout (canvas units) */
    float x, y, w, h;
    bool collapsed;
} PlanNode;

typedef struct Plan {
    int dialect, mode;
    char provider_version[64], adapter[48];
    char *sql; uint64_t sql_fp;
    char conn_name[64];
    int exec_id, stmt_id;
    double captured_at;          /* wall clock, seconds */
    PlanNode *nodes; int n, cap;
    char *raw; char raw_format[12];   /* json / xml / text / rows */
    PlanNum total_cost, planning_ms, exec_ms;
    char notes[1024];            /* limitations said to the user */
    bool partial;
} Plan;

void plan_init(Plan *p);
void plan_free(Plan *p);
int plan_add_node(Plan *p, int parent, const char *op);
void plan_prop(PlanNode *n, const char *key, const char *val);
const char *plan_prop_get(const PlanNode *n, const char *key);
void plan_num_fmt(PlanNum v, char *out, int n, int decimals);   /* "not available" / "n/a" / value */

/* ---- capture (EPL-001 / 010..020 / 071) ----
 * Statements to run (in order) and which result holds the plan; cleanup statements run even after
 * a failure (SQL Server SET ... OFF, Oracle plan_table rows). false with why when the mode is not
 * offered or the statement may not be explained that way. */
#define PLAN_MAXSQL 6
typedef struct PlanSql { char *sql[PLAN_MAXSQL]; int n; int result; bool finally_last; bool last_result; char note[300]; } PlanSql;   /* last_result: the plan is the statement's last result set (SQL Server) */
bool plan_capture_sql(int dialect, int mode, const char *stmt, int version, PlanSql *out, char *why, int whyn);
void plan_sql_free(PlanSql *s);
/* the statement writes (DML / DDL): Actual is refused unless explicitly allowed (EPL-072) */
bool plan_stmt_writes(const char *stmt);

/* ---- parse (EPL-009..022) ---- */
bool plan_parse(int dialect, int mode, ResultSet *const *rs, int n, int result, Plan *p, char *err, int errn);
/* import a plan file / pasted text (EPL-016 / 022): format "auto", "pg-json", "sqlplan", "mysql-json",
 * "mysql-tree", "mariadb-json", "sqlite-eqp", "oracle-text"; size-limited, no entity expansion */
bool plan_import(const char *text, size_t len, const char *format, Plan *p, char *err, int errn);

/* ---- layout (EPL-024 / 025 / 033) ---- */
void plan_layout(Plan *p, bool right_to_left);
/* SQL Server Management Studio layout (root left, each operator on its first input's row) */
void plan_layout_rows(Plan *p);
/* an operator's own cost: its total minus its inputs' totals (costs are cumulative in every engine Lume reads) */
double plan_self_cost(const Plan *p, int node, bool *ok);

/* ---- diagnostics (EPL-035..041 / 056) ---- */
enum { FD_CARDINALITY, FD_SCAN, FD_SPILL, FD_SORT, FD_STATS, FD_INDEX, FD_WARNING, FD_NESTED };
typedef struct PlanFinding {
    int kind, node;              /* node id */
    int confidence;              /* 0 low, 1 medium, 2 high */
    char title[200], evidence[600], missing[300], suggestion[600];
} PlanFinding;
typedef struct PlanDiagOpts { double card_ratio; double min_rows; } PlanDiagOpts;   /* 10x, 1000 rows by default */
int plan_diagnose(Plan *p, const PlanDiagOpts *o, PlanFinding *out, int max);
/* plan summary: engine, mode, totals that apply (EPL-035) */
void plan_summary(const Plan *p, char *out, int n);
/* a CREATE INDEX candidate for a finding (EPL-056 / 057); "" when none can be said */
bool plan_index_ddl(const Plan *p, const PlanFinding *f, char *out, int n, char *why, int whyn);

/* ---- comparison (EPL-064..066) ---- */
typedef struct PlanDiff { int changed_ops, only_a, only_b, card_changes; bool same_engine; char text[2000]; } PlanDiff;
void plan_compare(const Plan *a, const Plan *b, PlanDiff *d);

/* ---- context for the LLM (EPL-044 / 073 / 077) ---- */
typedef struct PlanShare { bool sql, literals, names, raw; int subtree; } PlanShare;   /* subtree -1 = whole plan */
char *plan_context(const Plan *p, const PlanShare *s);   /* malloc'd, sanitized */
void plan_mask_literals(const char *in, char *out, int n);   /* 'x' -> ?, numbers -> ? */

/* text of a plan for export / Raw view (EPL-016 / 069) */
char *plan_report_md(const Plan *p, const PlanFinding *f, int nf);
