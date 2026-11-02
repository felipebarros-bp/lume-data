/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Database object model (object source editor, spec section 7)
 * ============================================================
 *
 * Shared types: what an object is (kind + key), what an engine can do with it (capabilities),
 * what was read from the server (snapshot), what the planner decided (plan) and what the local
 * analysis found (diagnostics). Plain data, no SDL, no driver handles: the provider (objsrc.c)
 * fills snapshots from result sets, the planner (objplan.c) and diagnostics (objdiag.c) are pure
 * functions, the UI (objed.c) owns documents.
 */
#include <stdbool.h>
#include <stdint.h>

enum {
    OBJK_VIEW, OBJK_MATVIEW, OBJK_FUNCTION, OBJK_PROCEDURE, OBJK_PKG_SPEC, OBJK_PKG_BODY,
    OBJK_TRIGGER, OBJK_TYPE_SPEC, OBJK_TYPE_BODY,
    OBJK_OPERATOR, OBJK_EVENT_TRIGGER, OBJK_VTABLE,   /* read-only kinds (OBJ-058): shown, never published */
    OBJK_COUNT
};
const char *objkind_name(int k);        /* "VIEW", "PACKAGE BODY", ... */
const char *objkind_word(int k);        /* lower-case noun for messages: "view", "package body" */

/* Stable identity of an object (OBJ-002): never only its display name. */
typedef struct ObjKey {
    char conn_id[16];
    char schema[128];       /* "" when the engine has none (SQLite main) */
    char name[128];
    int kind;               /* OBJK_* */
    char signature[512];    /* PostgreSQL identity argument types, e.g. "integer, text" ("" otherwise) */
    char native_id[32];     /* oid / object_id when known; never used alone */
    char table[128];        /* triggers: the table they belong to */
} ObjKey;
/* "FUNCTION public.calc(integer)" etc. */
void objkey_display(const ObjKey *k, char *out, int n);
bool objkey_equal(const ObjKey *a, const ObjKey *b);   /* conn, schema, name, kind, signature */

/* What an engine offers for a kind (OBJ-058 / 059 / 060): actions are shown only when true. */
typedef struct ObjCaps {
    bool available;                 /* a Lume driver exists for the engine */
    bool source_read, source_edit, format, static_validate;
    bool server_validate;           /* a server check that changes nothing (PG: LIMIT 0 describe in a read-only transaction) */
    bool server_compile;            /* explicit compile (Oracle ALTER ... COMPILE) */
    bool replace_in_place;          /* CREATE OR REPLACE / CREATE OR ALTER exists for this kind */
    bool transactional_ddl;         /* the deployment can run inside one transaction */
    bool dependency_graph;          /* dependents come from the catalog (confirmed) */
    bool fingerprint;               /* the remote definition can be re-read before Apply */
    bool refresh;                   /* materialized view refresh */
    bool enable_disable;            /* triggers */
} ObjCaps;
ObjCaps objcaps_for(int dialect, int kind);

/* One dependent object: "VIEW public.v_sales" etc.; how: "view", "trigger", "routine", "index", "approximate". */
typedef struct ObjDep { char name[200]; char kind[24]; char how[24]; } ObjDep;
/* One column of a view (contract checks). */
typedef struct ObjColumn { char name[96]; char type[96]; } ObjColumn;
/* A compile / server error. */
typedef struct ObjError { int line, col; char code[24]; char text[400]; char unit[16]; } ObjError;

/* What the provider read (OBJ-003 / 007): immutable once loaded. */
typedef struct ObjSnapshot {
    char *original;           /* the source as the server keeps it (may be a body only) */
    char *ddl;                /* complete executable DDL rebuilt from it, or NULL when not possible */
    char missing[200];        /* what the DDL could not include, "" when complete */
    uint64_t fingerprint;     /* hash of the definition text as read (+ last DDL time on Oracle) */
    char owner[96];
    char acl[1024];           /* PostgreSQL aclitem[] text, "" none */
    char result_type[256];    /* functions: the declared return type */
    char options[256];        /* view reloptions / trigger "BEFORE INSERT ON t" etc. */
    ObjColumn *cols; int ncols;   /* view columns */
    char status[16];          /* Oracle VALID / INVALID, "" otherwise */
    ObjError *errors; int nerrors;
    bool enabled;             /* triggers */
    bool system, encrypted;   /* read-only reasons */
    char ro_reason[200];      /* why it is read-only, in words (extension member, aggregate, ...); "" none */
    int server_version;       /* e.g. 160002, 0 unknown */
    bool exists;              /* false: the catalog has no such object (dropped elsewhere) */
    /* objects that depend on this one (OBJ-065); deps_known false = the engine cannot tell */
    ObjDep *deps; int ndeps; bool deps_known;
    /* statements that recreate what a DROP would lose (indexes of a materialized view, INSTEAD OF
     * triggers of a SQLite view, grants, comment); appended by drop + create plans (OBJ-070) */
    char **recreate; int nrecreate;
} ObjSnapshot;
void objsnap_free(ObjSnapshot *s);
uint64_t obj_hash(const char *s, int n);   /* FNV-1a 64 */

/* The CREATE header of an edited source, as the planner and diagnostics read it. */
typedef struct ObjHeader {
    bool found;               /* a CREATE / ALTER of a known kind was found */
    int start;                /* offset of CREATE / ALTER */
    int verb_end;             /* offset just past "CREATE [OR REPLACE | OR ALTER]" / "ALTER" */
    bool or_replace, or_alter, alter_only, if_not_exists;
    int kind;                 /* OBJK_* */
    char schema[128], name[128];
    int name_start, name_end; /* the (qualified) name in the text */
    int args_start, args_end; /* routines: inside the parentheses, -1 none */
    char returns[256];        /* functions: RETURNS clause as written, "" none */
    char table[128];          /* triggers: ON table */
    int select_start;         /* views: offset of the query after AS, -1 */
    int body_end;             /* end of the definition (before a trailing ';' / '/' / GO) */
    int extra_statements;     /* significant statements after the definition (views / PG) */
} ObjHeader;
bool obj_parse_header(const char *text, int len, int dialect, ObjHeader *h);
/* PostgreSQL: identity argument types of an argument list ("a int, OUT b text" -> "integer"),
 * normalised like oidvectortypes(); procedures keep OUT arguments. false when unsure. */
bool pg_identity_types(const char *args, int len, bool procedure, char *out, int n);
/* PostgreSQL type name normalised for comparisons: "int4" -> "integer", typmods dropped. */
void pg_norm_type(const char *t, int len, char *out, int n);

/* Planner output (OBJ-035 / 039 / 040). */
enum { RISK_NON_DESTRUCTIVE, RISK_POTENTIAL, RISK_DESTRUCTIVE, RISK_UNKNOWN };
enum { ATOM_TRANSACTIONAL, ATOM_AUTOCOMMIT, ATOM_NON_ATOMIC, ATOM_UNKNOWN };
const char *risk_name(int r);
const char *atomicity_name(int a);
#define PLAN_MAX_SQL 12
#define PLAN_MAX_NOTES 16
typedef struct ObjPlan {
    char *sql[PLAN_MAX_SQL]; int nsql;     /* statements in execution order (owned) */
    int create_index;                      /* which one is the user's CREATE (maps errors to the buffer), -1 none */
    int risk, atomicity;
    char impact[PLAN_MAX_NOTES][240]; int nimpact;
    char pre[PLAN_MAX_NOTES][240]; int npre;   /* preconditions checked before Apply */
    bool blocked; char block_reason[400];  /* nothing may run */
    bool destructive;                      /* DROP / recreate: needs the specific consent */
    bool script_only;                      /* engine without a Lume driver: export only */
    char strategy[120];                    /* "CREATE OR REPLACE VIEW", "DROP + CREATE in one transaction", ... */
    char probe_signature[600];             /* PostgreSQL: regprocedure text to resolve on the server */
    bool conflict;                         /* the remote definition changed since the editor opened it (AT-04) */
    bool destructive_available;            /* blocked, but an explicit drop + create plan can be requested (AT-02) */
    bool identity_unsure;                  /* the signature could not be read locally: confirmed on the server first */
    bool no_changes;                       /* the edited text equals the stored definition */
} ObjPlan;
void objplan_free(ObjPlan *p);

/* What the planner needs (all read-only). remote: the definition re-read just before planning
 * (NULL = not re-read; the plan then states that the fingerprint was not checked). */
typedef struct ObjPlanInput {
    int dialect;
    const ObjKey *key;
    const ObjSnapshot *base;      /* what the editor was opened from */
    const ObjSnapshot *remote;    /* re-read now, or NULL */
    const char *local; int local_len;
    const ObjColumn *new_cols; int n_new_cols;   /* PG views: columns of the edited query (server describe), NULL unknown */
    bool allow_destructive;       /* the user asked for the explicit drop + create plan */
    bool has_driver;              /* a Lume driver can run it (false: export only) */
    bool read_only; const char *ro_reason;   /* connection / object cannot be written (AT-10) */
    bool tx_open;                 /* the session has an open transaction (Oracle / MySQL DDL would commit it) */
    int server_version;           /* PG 160002, Oracle 23, MSSQL 16, MariaDB 101100 ... 0 unknown */
    bool mariadb;                 /* MySQL family: MariaDB semantics */
} ObjPlanInput;
/* Builds the plan. Never runs anything. Returns false only on bad input; a refused change is a
 * plan with blocked = true and the reason. */
bool objplan_make(const ObjPlanInput *in, ObjPlan *out);
/* Column contract of a PostgreSQL view (OBJ-047): true when `now` keeps every column of `before`
 * by position, name and type (new columns may only be appended). why: the first difference. */
bool pg_view_contract_ok(const ObjColumn *before, int nb, const ObjColumn *now, int nn, char *why, int n);
/* The statements joined with ";\n" (MSSQL: "\nGO\n" shown, but sent one by one). */
char *objplan_script(const ObjPlan *p, int dialect);

/* Local diagnostics (OBJ-022 / 024 / 026 / 027). */
enum { DIAG_ERROR, DIAG_WARNING, DIAG_INFO, DIAG_HINT };
enum { DSRC_PARSER, DSRC_METADATA, DSRC_DRIVER, DSRC_SERVER };
typedef struct ObjDiag {
    int source, severity;
    char code[24];
    char message[300];
    int start, end;           /* byte offsets in the buffer, -1 unknown */
    int line, col;            /* 1-based, 0 unknown */
    char unit[16];            /* "SPEC" / "BODY" / "MAIN" */
    int confidence;           /* 0 exact, 1 mapped, 2 approximate */
    bool has_fix; char fix_label[120]; int fix_start, fix_end; char fix_text[400];   /* quick fix (OBJ-028) */
} ObjDiag;
const char *diag_severity_name(int s);
const char *diag_source_name(int s);
