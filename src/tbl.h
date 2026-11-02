/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Visual Table Designer core (TBL-001..100): model, snapshot parsing, semantic diff, validation
 * and the DDL planner per dialect. Headless: no UI, no connection.
 * ==============================================================================================
 *
 *   TdTable     a table definition. Every column / key / index has a stable id, so a rename is a
 *               RENAME and never DROP + ADD (spec 7.1). Attributes Lume does not model are kept
 *               in `extra` / `raw` and either preserved (ALTER leaves them alone) or block the
 *               operations that would have to rewrite them (TBL-081).
 *   snapshot    td_snapshot_sql writes read-only catalog statements for one table; td_snapshot_parse
 *               turns their results into a TdTable plus a fingerprint (TBL-050 / 057).
 *   diff        td_diff compares original and draft by id: grouped changes (TBL-051).
 *   validate    local checks, no network (TBL-065 / 066).
 *   plan        td_plan writes the exact statements in a deterministic order, with per-step risk:
 *               destructive, blocking / rewrite, implicit commit, uncertain; the rollback class;
 *               SQLite's rebuild when ALTER cannot do it (TBL-052..060, 074..081).
 *
 * Dialects never pretend: an operation the engine cannot do (or Lume cannot do safely) blocks the
 * plan with a reason instead of generating approximate SQL (TBL-080).
 */
#include <stdbool.h>
#include <stdint.h>
#include "rs.h"

enum { TD_SQLITE, TD_PG, TD_ORACLE, TD_MYSQL, TD_MARIADB, TD_MSSQL, TD_NDIALECT };
int td_dialect_for_driver(const char *driver);   /* -1 unknown */
const char *td_dialect_name(int d);

/* capability values: unknown is not false (TBL-080) */
enum { TCAP_NO, TCAP_YES, TCAP_UNKNOWN };
typedef struct TdCaps {
    int rename_column, drop_column, alter_type, comments, identity, generated, generated_virtual, deferrable,
        partial_index, include_index, index_method, check, transactional_ddl, column_order, unlogged, temp,
        engine, collation, rename_constraint, rename_index, on_update_actions, add_column_after;
    int max_ident;           /* identifier length limit */
} TdCaps;
/* version: server version as major*10000 + minor*100 (0 = unknown) */
TdCaps td_caps(int dialect, int version);

/* key kinds */
enum { TK_PK, TK_UNIQUE, TK_FK, TK_CHECK, TK_INDEX, TK_NKINDS };
const char *td_key_kind_name(int k);

#define TD_MAXKC 32
typedef struct TdCol {
    int id;
    char name[128];
    char type[128];          /* as the engine spells it: "numeric(12,2)", "varchar(24)", "NUMBER(10)" */
    bool notnull;
    char def[512];           /* DEFAULT as SQL text, exactly ("'open'", "now()", "0"); "" none */
    int identity;            /* 0 none; 1 ALWAYS; 2 BY DEFAULT / AUTO_INCREMENT / IDENTITY(1,1) */
    char gen[512];           /* generated / computed expression; "" none */
    bool gen_stored;
    char comment[512];
    char collation[64];      /* "" default */
    char def_name[128];      /* SQL Server: name of its DEFAULT constraint */
    char extra[256];         /* attributes Lume does not model ("" none): kept, shown, preserved */
} TdCol;

typedef struct TdKey {
    int id, kind;
    char name[128];          /* "" = let the engine name it (new only) */
    int cols[TD_MAXKC]; bool desc[TD_MAXKC]; int ncols;   /* column ids, in order */
    /* index */
    bool unique;
    char method[32];         /* PG: btree / gin / brin...; "" default */
    char where[512];         /* partial / filtered index */
    int include[TD_MAXKC]; int ninclude;
    /* foreign key */
    char ref_schema[128], ref_table[128];
    char ref_cols[TD_MAXKC][128]; int nref;
    char on_delete[16], on_update[16];   /* "" = NO ACTION (the engine's default); CASCADE, SET NULL, SET DEFAULT, RESTRICT */
    bool deferrable, deferred;
    /* check */
    char check[1024];
    /* not modelled (expression index, exotic options): read-only, preserved */
    char raw[1024];
    bool backs_constraint;   /* an index that exists because of a PK / UNIQUE (not listed separately) */
} TdKey;

typedef struct TdTable {
    char schema[128], name[128];
    char comment[512];
    bool exists;             /* false = CREATE */
    bool temporary, unlogged;
    char engine[32], charset[32], collation[64], tablespace[64];   /* MySQL engine / charset; others' tablespace */
    char special[200];       /* partitioned / external / memory-optimized...: why it is read-only ("" none) */
    char extra[512];         /* table options not modelled: preserved, shown */
    TdCol *cols; int ncols, capcols;
    TdKey *keys; int nkeys, capkeys;
    int next_id;
    /* from the snapshot, for the review */
    char deps[1024];         /* dependants known (views, FKs from other tables), newline separated */
    char triggers[4096];     /* SQLite: CREATE TRIGGER texts, "\n;;\n" separated (a rebuild recreates them) */
    bool deps_partial;       /* the catalog could not tell everything (permissions) */
    bool can_alter;          /* the session may change it (owner / privilege), when known */
    char alter_why[200];
    uint64_t fingerprint;
    long long approx_rows;   /* catalog estimate, -1 unknown (never a COUNT) */
} TdTable;

void td_init(TdTable *t);
void td_free(TdTable *t);
void td_copy(TdTable *dst, const TdTable *src);   /* deep; dst is overwritten (call td_free first if used) */
bool td_equal(const TdTable *a, const TdTable *b);
bool td_col_same(const TdCol *a, const TdCol *b);                 /* field by field (not memcmp) */
bool td_key_same(const TdKey *a, const TdKey *b, bool with_name);
void td_type_risk(const char *from, const char *to, char *out, int n);   /* "" when nothing can be lost */
TdCol *td_col(TdTable *t, int id);
const TdCol *td_col_c(const TdTable *t, int id);
TdKey *td_key(TdTable *t, int id);
TdCol *td_add_col(TdTable *t, const char *name, const char *type);   /* new id */
TdKey *td_add_key(TdTable *t, int kind, const char *name);
void td_del_col(TdTable *t, int id);   /* also out of keys that used it (keys left with no column are removed) */
void td_del_key(TdTable *t, int id);
bool td_move_col(TdTable *t, int id, int delta);
int td_col_index(const TdTable *t, int id);
TdKey *td_pk(TdTable *t);
bool td_col_in_kind(const TdTable *t, int col_id, int kind);
/* default type for a new column per dialect (TBL-011) */
const char *td_default_type(int dialect);
/* the native types offered by the picker (TBL-017); NULL-terminated */
const char *const *td_types(int dialect);
/* proposes a name not used yet: base, base_2, ... (TBL-024) */
void td_unique_col_name(const TdTable *t, const char *base, char *out, int n);
/* naming convention for keys (TBL-040): {table} {cols} {kind}; never overrides a manual name */
void td_key_name(const TdTable *t, const TdKey *k, const char *pattern, char *out, int n);

/* ---- snapshot (TBL-050) ---- */
#define TD_SNAP_MAX 8
typedef struct TdSql { char *sql[TD_SNAP_MAX]; int n; } TdSql;
void td_sql_free(TdSql *s);
/* read-only catalog statements for one table (in the order td_snapshot_parse expects) */
bool td_snapshot_sql(int dialect, const char *schema, const char *name, TdSql *out);
/* parses their results; false with err when the table was not found or a result is missing (never an
 * empty "valid" table: TBL-002.3) */
bool td_snapshot_parse(int dialect, const char *schema, const char *name, ResultSet *const *rs, int n, TdTable *out, char *err, int errn);
uint64_t td_fingerprint(const TdTable *t);

/* ---- diff (TBL-051) ---- */
enum { TC_ADD_COL, TC_RENAME_COL, TC_ALTER_COL, TC_DROP_COL, TC_ADD_KEY, TC_RENAME_KEY, TC_ALTER_KEY, TC_DROP_KEY, TC_TABLE, TC_RENAME_TABLE, TC_REORDER };
typedef struct TdChange { int kind, id; char text[300]; } TdChange;
int td_diff(const TdTable *orig, const TdTable *draft, TdChange *out, int max);

/* ---- validation (TBL-065 / 066) ---- */
enum { TV_ERROR, TV_WARNING, TV_INFO };
typedef struct TdIssue { int level; int col_id, key_id; char field[24]; char text[300]; } TdIssue;
int td_validate(int dialect, int version, const TdTable *orig, const TdTable *draft, TdIssue *out, int max);

/* ---- plan (TBL-052..060) ---- */
enum { TS_DESTRUCTIVE = 1, TS_BLOCKING = 2, TS_REWRITE = 4, TS_IMPLICIT_COMMIT = 8, TS_UNCERTAIN = 16, TS_DATA_CHECK = 32, TS_REBUILD = 64 };
enum { TR_TRANSACTIONAL, TR_COMPENSATE, TR_IRREVERSIBLE };
enum { RISK_LOW, RISK_MEDIUM, RISK_HIGH };
#define TD_MAXSTEPS 96
typedef struct TdStep { char *sql; int flags; char why[300]; int change; } TdStep;
typedef struct TdPlan {
    TdStep steps[TD_MAXSTEPS]; int n;
    bool blocked; char block_reason[400];
    bool transactional;      /* the steps run inside BEGIN ... COMMIT / ROLLBACK */
    bool rebuild;            /* SQLite: copy into a new table */
    int rollback;            /* TR_* */
    int risk;                /* RISK_* */
    int flags;               /* union of the steps' flags */
    char warnings[8][300]; int nwarn;
    char compensate[4096];   /* proposed compensating script (never run by itself, TBL-060) */
    bool fk_toggle;          /* SQLite rebuild: PRAGMA foreign_keys OFF before, ON after (outside the transaction) */
} TdPlan;
typedef struct TdPlanOpts {
    int version;             /* server version, 0 unknown */
    char lock_timeout[16];   /* PG: "5s"; "" none */
    char using_expr[TD_MAXKC][256]; int using_col[TD_MAXKC]; int nusing;   /* PG USING per column id */
    bool sqlite_fk_on;       /* PRAGMA foreign_keys was on (a rebuild turns it off and back on) */
    bool no_compensate;      /* internal: the reverse plan of a compensating script */
} TdPlanOpts;
void td_plan(int dialect, const TdTable *orig, const TdTable *draft, const TdPlanOpts *o, TdPlan *p);
void td_plan_free(TdPlan *p);
/* the plan as one script (preview / export / console; TBL-052 / 058) */
char *td_plan_script(int dialect, const TdPlan *p);
/* hash of the plan text + draft revision: what Review approved (spec 7.3 ApprovalToken) */
uint64_t td_plan_hash(const TdPlan *p, uint64_t revision);

/* ---- identifiers ---- */
void td_quote(int dialect, const char *ident, char *out, int n);
void td_qname(int dialect, const char *schema, const char *name, char *out, int n);
void td_lit(int dialect, const char *s, char *out, int n);   /* string literal */

/* ---- draft files (TBL-009): text, no credentials ---- */
char *td_serialize(const TdTable *t);                       /* malloc'd */
bool td_deserialize(const char *text, TdTable *t);
