/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Object catalog for completion
 * =============================
 *
 * Every object the connection's user is allowed to see, by name only: tables, views, materialized
 * views, synonyms (with their target), procedures, functions, packages, sequences and types, in
 * every schema. The editor's completion proposes them (own schema unqualified, other schemas as
 * SCHEMA.NAME, "SCHEMA." lists one schema).
 *
 * Reading it can take minutes on a large Oracle database (ALL_OBJECTS checks grants object by
 * object), so it is read in a SECOND session opened just for that, on a worker thread: the
 * editor's session is never held. The result is cached on disk per connection (names and kinds
 * only, never data or columns), so the next connect proposes at once and refreshes in the
 * background. Settings > Connections can turn the second session off.
 *
 * The data half (objcat_sql, objcat_add, objcat_finish, objcat_save / load, lookups) runs no SQL
 * and is tested headless; the runtime half (objcat_pump, objcat_for) owns the worker.
 */
#include <stdbool.h>
#include <stddef.h>

enum { OC_TABLE, OC_VIEW, OC_MVIEW, OC_SYNONYM, OC_PROCEDURE, OC_FUNCTION, OC_PACKAGE, OC_SEQUENCE, OC_TYPE, OC_N };

typedef struct OcObj { char *schema, *name, *target; unsigned char kind; unsigned mask; } OcObj;   /* target: a synonym's "OWNER.NAME", else NULL; mask: objcat_mask(name) */
typedef struct OcSchema { char *name; int first, n; } OcSchema;                     /* objects [first, first + n) after objcat_finish */

typedef struct ObjCat {
    OcObj *o; int n, cap;
    OcSchema *s; int ns;          /* sorted by name, case-insensitive (built by objcat_finish) */
    char home[128];               /* the session's current schema: its objects are proposed unqualified */
} ObjCat;

/* The catalog queries of a driver ("oracle", "postgres", "mysql", "mariadb", "sqlserver"): i = 0 the current
 * schema (one row, one column), then i = 1.. object lists of four columns (schema, name, kind word,
 * target). alt = true: the fallback of query i for older servers (NULL when there is none).
 * NULL past the last one, and for drivers without a catalog (SQLite). */
const char *objcat_sql(const char *driver, int i, bool alt);
/* Kind of a kind word from those queries ("TABLE", "MATERIALIZED VIEW", ...); -1 unknown. */
int objcat_kind(const char *word);
const char *objcat_kind_name(int kind);   /* "table", "materialized view", ... */

/* Letters / digits / '_' present in s, as bits (case-insensitive): a name can only match a typed
 * prefix whose mask is a subset of its own (completion skips the rest without comparing text). */
unsigned objcat_mask(const char *s, int n);
void objcat_add(ObjCat *c, const char *schema, const char *name, int kind, const char *target);
/* Sorts by schema then name, drops duplicates (a materialized view also listed as its table keeps
 * the view) and builds the schema list. Call once after the adds. */
void objcat_finish(ObjCat *c);
void objcat_free(ObjCat *c);
/* The schema named by the n bytes at name (case-insensitive), or NULL. */
const OcSchema *objcat_schema(const ObjCat *c, const char *name, int n);

/* Disk cache: "# lume objcat 1", "home\t<schema>", then "kind\tschema\tname\ttarget" lines. */
bool objcat_save(const ObjCat *c, const char *path);
bool objcat_load(ObjCat *c, const char *path);

/* ---- runtime (the app) ---- */
#include "lume.h"
#include "db.h"
struct Connection;
enum { OCS_NONE, OCS_LOADING, OCS_LOADED, OCS_FAILED, OCS_UNAVAILABLE };   /* columns of a table */
void objcat_init(App *a);
/* Per frame: collects finished reads, hands the active connection's catalog to the completer. */
void objcat_pump(App *a);
/* The catalog completion should use for c (cached or fresh), or NULL. Starts the background read
 * the first time it is asked for a connected connection (once per connect). */
const ObjCat *objcat_for(App *a, struct Connection *c);
/* The table schema.name with its columns as far as known (a stable pointer, owned here); the
 * first call asks the catalog session for them (a synonym: its target's). *state is OCS_*:
 * LOADING until they arrive (objcat_pump then tells the editor and the Explorer), UNAVAILABLE
 * when no catalog session can be had (second session off, refused). NULL: no catalog for c. */
DbTable *objcat_table(App *a, struct Connection *c, const char *schema, const char *name, int *state);
/* Fills t's columns from the catalog session (t->name "SCHEMA.NAME", or unqualified for the
 * current schema): OCS_LOADED (filled now), OCS_LOADING (asked; filled on a later call), or
 * OCS_UNAVAILABLE / OCS_FAILED. Never blocks. */
int objcat_columns_into(App *a, struct Connection *c, DbTable *t);
/* Foreign keys of the table schema.name (a synonym: its target's), read by the catalog session the
 * first time; returns OCS_LOADED with *out / *n (borrowed, stable until the connection closes),
 * OCS_LOADING (asked; objcat_pump tells the editor when they arrive), OCS_FAILED, or
 * OCS_UNAVAILABLE (no catalog session: the caller may read them itself). */
int objcat_fks(App *a, struct Connection *c, const char *schema, const char *name, DbFk **out, int *n);
/* ---- package members: the subprograms a package declares (Oracle) ---- */
typedef struct OcMember { char *name, *args; bool function; } OcMember;   /* args: "P_ID, P_NAME" ("" when none / not listed) */
typedef struct OcMembers { OcMember *m; int n; int state; } OcMembers;   /* state OCS_* */
/* The query listing the members of package schema.name on `driver`, into out (out NULL: only
 * whether the driver has packages); false when it has none (every driver but Oracle).
 * Rows: name, 'FUNCTION' / 'PROCEDURE', args. */
bool objcat_members_sql(const char *driver, const char *schema, const char *name, char *out, size_t n);
void objcat_members_free(OcMembers *ms);
/* The members of package schema.name as far as known (a stable pointer, owned here); the first
 * call asks the catalog session for them. ->state is OCS_LOADING until they arrive (objcat_pump
 * then tells the Explorer), OCS_UNAVAILABLE when no catalog session can be had. NULL: no catalog
 * for c, or no packages on its driver. */
const OcMembers *objcat_members(App *a, struct Connection *c, const char *schema, const char *name);
/* "" when nothing is happening, else a short state for the status line ("reading", "failed: ...") */
const char *objcat_state(struct Connection *c);
void objcat_dump(App *a);   /* harness: objcatstate */
bool objcat_settled(App *a);   /* harness: objcatawait (the active connection's read ended) */
