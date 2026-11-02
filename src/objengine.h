/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Engine adapters for the object source editor (spec section 4, OBJ-058 / 080)
 * ============================================================================
 *
 * Everything that differs between databases lives in one adapter per engine (obj_sqlite.c,
 * obj_pg.c, obj_oracle.c, obj_mysql.c, obj_mssql.c). Supporting another database means writing
 * one more adapter and registering it; the core (planner checks, diagnostics, diff, provider,
 * deployment, UI) does not change.
 *
 * What an adapter does:
 *   - declares its dialect's lexical rules (dialect_register) and its capabilities per object
 *     kind and server version: the UI shows only what is true (OBJ-059 / 060);
 *   - plans: turns a checked edit into statements, risk, atomicity, impact and preconditions;
 *   - writes the catalog SQL to read a definition, list objects and read the status after Apply,
 *     and parses the results into an ObjSnapshot.
 *
 * What an adapter can NOT do (OBJ-080): it never receives a connection. It only produces SQL
 * text. Catalog SQL is run by the provider (objsrc.c), which refuses any statement the write
 * classifier says may write; plan statements run only through the deployment service after the
 * review dialog was confirmed.
 *
 * Versioning and ownership: api_version must equal OBJ_ENGINE_API. Strings written into an
 * ObjSqlBuf belong to the caller (obj_sql_free). An ObjSnapshot filled by load_parse belongs to
 * the caller (objsnap_free); ResultSets passed in stay owned by the caller.
 */
#include "objmodel.h"
#include "rs.h"

#define OBJ_ENGINE_API 1

/* A list of SQL statements (owned). */
#define OBJ_SQL_MAX 8
typedef struct ObjSqlBuf { char *sql[OBJ_SQL_MAX]; int n; } ObjSqlBuf;
void obj_sql_add(ObjSqlBuf *b, const char *fmt, ...);
void obj_sql_free(ObjSqlBuf *b);

/* One object of a listing (Explorer object nodes, OBJ-064). */
/* args / returns / comment: routines, for signature help (OBJ-018); "" when the engine does not list them */
typedef struct ObjListItem { ObjKey key; char status[16]; bool system; char args[256]; char returns[96]; char comment[160]; char extension[64]; } ObjListItem;   /* extension: owning extension, "" none (OBJ-058) */

typedef struct ObjEngine {
    int api_version;               /* OBJ_ENGINE_API */
    const char *id;                /* the connection's driver id: "sqlite", "postgres", ... */
    int dialect;                   /* from dialect_register / the built-in ids */
    bool has_driver;               /* a Lume DbDriver can run it; false = export-only plans */
    ObjCaps (*caps)(int kind, int server_version);
    /* Engine part of the planner. The core already checked the header, the name, the extra
     * statements, conflicts and read-only; [cs, ce) is the definition inside in->local. */
    void (*plan)(const ObjPlanInput *in, const ObjHeader *h, int cs, int ce, ObjPlan *p);
    /* catalog: read-only SQL text + parsing of its results (all optional when !has_driver) */
    bool (*load_sql)(const ObjKey *k, ObjSqlBuf *out);
    bool (*load_parse)(const ObjKey *k, ResultSet *const *rs, int n, ObjSnapshot *s, char *err, int errn);
    bool (*list_sql)(const char *schema, ObjSqlBuf *out);
    int (*list_parse)(ResultSet *const *rs, int n, const char *conn_id, ObjListItem *out, int max);
    /* after Apply (Oracle compile status): statements and parsing into status / errors */
    bool (*status_sql)(const ObjKey *k, ObjSqlBuf *out);
    bool (*status_parse)(const ObjKey *k, ResultSet *const *rs, int n, ObjSnapshot *s);
    /* columns of a view query without running it fully (PG: LIMIT 0); NULL = not offered */
    bool (*describe_sql)(const char *query, int len, ObjSqlBuf *out);
} ObjEngine;

/* Registers an adapter; false when the API version does not match or the table is full. The
 * struct must outlive the app (a static in the module). Built-in adapters are registered on
 * first use. */
bool obj_engine_register(const ObjEngine *e);
const ObjEngine *obj_engine_for_dialect(int dialect);
const ObjEngine *obj_engine_for_driver(const char *driver_id);
int obj_engine_count(void);
const ObjEngine *obj_engine_at(int i);

/* Built-in adapters. */
extern const ObjEngine obj_engine_sqlite, obj_engine_pg, obj_engine_oracle, obj_engine_mysql, obj_engine_mariadb, obj_engine_mssql;
