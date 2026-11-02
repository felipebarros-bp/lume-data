/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Object source provider and deployment service (OBJ-004 / 037 / 038 / 041 / 042 / 061 / 074 / AT-05 / 07)
 * =========================================================================================================
 *
 * The only code that sends the object editor's SQL to a connection. Every operation is a job
 * that runs on the executor's worker thread and is advanced by objsrc_poll from the UI frame:
 * nothing here blocks the SDL loop, and every job can be cancelled.
 *
 *   load      the adapter's catalog SQL -> an ObjSnapshot (definition, fingerprint, columns,
 *             dependents, what a drop would lose, Oracle status / errors)
 *   list      objects of a schema for the Explorer
 *   describe  the columns of a view query, compiled by the server as a temporary view inside
 *             BEGIN ... ROLLBACK (validates names and types without running the query)
 *   apply     a reviewed plan: re-reads the definition and refuses when it changed since the
 *             review (AT-04); resolves a PostgreSQL routine's identity first (AT-03); runs the
 *             statements in order, stopping at the first failure, with a final ROLLBACK that
 *             always runs for transactional plans; then reads the definition (and the Oracle
 *             status / errors) back. A connection lost while DDL may have run leaves the outcome
 *             UNKNOWN: nothing is retried automatically (AT-07).
 *
 * Safety (OBJ-080): catalog / list / status SQL from an adapter is refused when the write
 * classifier says it may write. Only apply runs DDL, and only a plan given by the caller after
 * the review was confirmed.
 */
#include "db.h"
#include "exec.h"
#include "objengine.h"

enum { OJ_LOAD, OJ_LIST, OJ_DESCRIBE, OJ_APPLY };
enum { OJS_RUNNING, OJS_DONE, OJS_FAILED, OJS_CANCELLED, OJS_UNKNOWN };
/* apply phases */
enum { OJP_PRECHECK, OJP_RUN, OJP_REFRESH, OJP_END };

typedef struct ObjJob {
    int kind, state, phase;
    const ObjEngine *eng;
    Db *db;
    ObjKey key;
    ExecJob *ex;
    char err[600];                 /* why it failed / was refused (no SQL parameter values, no secrets) */
    double started;                /* monotonic seconds */
    /* results */
    ObjSnapshot snap;              /* load; apply: the definition read back after running */
    ObjListItem *items; int nitems;
    ObjColumn *cols; int ncols;    /* describe */
    /* apply */
    ObjPlan plan;                  /* owned copy */
    uint64_t expect_fp;            /* the fingerprint the review was made against */
    bool transactional;
    int ran;                       /* plan statements that completed */
    int failed_at;                 /* plan statement that failed, -1 none */
    char failed_error[512]; int failed_offset;   /* driver error and byte offset inside that statement */
    double run_ms;
    bool conflict;                 /* precheck: the remote definition changed */
    bool compiled_invalid;         /* Oracle: the object is INVALID after Apply */
    bool ddl_ran;                  /* at least one DDL statement completed (or may have) */
} ObjJob;

ObjJob *objsrc_load(Db *db, const ObjEngine *e, const ObjKey *k);
ObjJob *objsrc_list(Db *db, const ObjEngine *e, const char *schema, const char *conn_id);
ObjJob *objsrc_describe(Db *db, const ObjEngine *e, const char *query, int len);
/* plan is copied; expect_fp: base fingerprint at review time */
ObjJob *objsrc_apply(Db *db, const ObjEngine *e, const ObjKey *k, const ObjPlan *plan, uint64_t expect_fp);
/* Advances the job; true once it is finished (state != OJS_RUNNING). UI thread. */
bool objsrc_poll(ObjJob *j);
void objsrc_cancel(ObjJob *j);
void objsrc_free(ObjJob *j);
/* Runs a job to completion on the calling thread (tests / scripts); false on failure. */
bool objsrc_wait(ObjJob *j, int timeout_ms);
/* True when a driver error means the connection is gone (outcome of a statement unknown). */
bool objsrc_connection_lost(const char *error);
const char *objsrc_state_name(int s);
