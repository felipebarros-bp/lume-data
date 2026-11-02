/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Schema index for Lume Intelligence (large databases: 10k+ tables)
 * =================================================================
 *
 * The assistant never gets a whole large catalog. Lume keeps a local index of the structure
 * (schema, table, columns, types, keys, foreign keys and the descriptions the database keeps:
 * Oracle / PostgreSQL / MySQL comments, SQL Server MS_Description) and picks, per question, the few
 * tables that matter. Nothing here knows any particular application's schema: cryptic names are
 * explained by those descriptions, the workspace glossary and what the user's own queries use.
 *
 *   - words of the question against names and descriptions (accents, plural, abbreviations:
 *     "beneficiário" finds BNF / BENEF), descriptions weigh more than names;
 *   - the neighbours of the best tables by foreign key (one hop);
 *   - what the user is working on (tables of the open query, the report's cards: "boost").
 *
 * The model then sees those tables in full plus a summary of the rest, and may ask for more
 * (search_schema / describe_tables), answered from this index without touching the database.
 *
 * Building: the catalog is read with a few bulk queries (schidx_sql), run by the caller off the UI
 * thread (exec job, read-only), and fed in with schidx_add_*; nothing here runs SQL. The index is
 * plain data: tests build it directly.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SxCol { char *name, *type, *comment; bool pk, notnull; } SxCol;
typedef struct SxFk { char *cols, *ref_table; int ref; } SxFk;   /* ref: index of the referenced table, -1 unknown */
typedef struct SxTable {
    char *schema, *name, *comment;
    bool is_view;
    SxCol *cols; int ncols, capcols;
    SxFk *fks; int nfks, capfks;
    int nrefs;                 /* how many tables reference it (hubs) */
    char *usage;               /* words of the user's own queries that use it (query history): local only, never rendered */
    /* precomputed words for picking (built lazily, see schidx_words) */
    char (*w)[24]; unsigned char *wcat; int nw;
} SxTable;

typedef struct SchIdx {
    SxTable *t; int n, cap;
    bool words_ready;          /* every table's words are precomputed (picking is then a scan of short arrays) */
    char dialect[16];
    double built_at;           /* unix seconds */
    /* lookup: open addressing on "schema.name" (lower case) */
    int *slot; int nslot;
} SchIdx;

void schidx_init(SchIdx *x);
void schidx_free(SchIdx *x);
/* feeding (any order; columns may come before their table's comment) */
int schidx_table(SchIdx *x, const char *schema, const char *name, bool is_view);   /* finds or adds */
void schidx_table_comment(SchIdx *x, int ti, const char *comment);
void schidx_add_col(SchIdx *x, int ti, const char *name, const char *type, bool pk, bool notnull, const char *comment);
void schidx_set_col_comment(SchIdx *x, int ti, const char *col, const char *comment);
void schidx_add_fk(SchIdx *x, int ti, const char *cols, const char *ref_schema, const char *ref_name);
void schidx_finish(SchIdx *x);   /* resolves foreign keys, counts references */
/* the text of a query of the user's that uses this table (history): its words help picking; never sent */
void schidx_add_usage(SchIdx *x, int ti, const char *text);
int schidx_find(const SchIdx *x, const char *schema, const char *name);   /* name may be "schema.name"; -1 */

/* Catalog SQL for a dialect ("postgres", "oracle", "mysql", "mssql"): the k-th bulk read and how
 * its rows feed the index. schemas: allowed namespaces ("" = every non-system one).
 *   kind SXQ_COLS:     schema, table, is_view, column, type, pk (0/1), notnull (0/1), column comment
 *   kind SXQ_TCOMMENT: schema, table, comment
 *   kind SXQ_FKS:      schema, table, columns (comma list), ref schema, ref table
 * Returns false past the last query. */
enum { SXQ_COLS, SXQ_TCOMMENT, SXQ_FKS };
bool schidx_sql(const char *dialect, int k, const char *const *schemas, int nschemas, int *kind, char *out, size_t n);

/* Picking tables for a question. */
typedef struct SxPick { int t; float score; char why[64]; } SxPick;
typedef struct SxQuery {
    const char *question;      /* the user's words */
    const char *const *boost; int nboost;   /* tables in use ("schema.name" or "name") */
    int max_tables;            /* 0 = 30 */
} SxQuery;
int schidx_pick(const SchIdx *x, const SxQuery *q, SxPick *out, int max);   /* best first */
int schidx_search(const SchIdx *x, const char *terms, SxPick *out, int max);   /* search_schema */

/* The <schema> block for the model: the picked tables in full (columns, types, keys, short
 * descriptions, foreign keys), a summary of the rest (schemas and table counts) and how to ask
 * for more. Never longer than max_bytes. malloc'd. */
char *schidx_render(const SchIdx *x, const SxPick *p, int np, const char *conn_name, size_t max_bytes, int *rendered);
/* only names and one-line descriptions (search_schema results) */
char *schidx_render_list(const SchIdx *x, const SxPick *p, int np, size_t max_bytes);

/* Disk cache (<prefs>/cache): names and descriptions only, never rows. */
bool schidx_save(const SchIdx *x, const char *path);
bool schidx_load(SchIdx *x, const char *path);

/* exposed for tests */
int schidx_tokens(const char *text, char out[][32], int max);   /* normalised words */
void schidx_skeleton(const char *word, char *out, size_t n);    /* consonant skeleton ("beneficiario" -> "bnfcr") */
