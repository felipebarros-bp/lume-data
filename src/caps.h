/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Driver capabilities (TBR-030)
 * =============================
 *
 * A declarative table of what each database engine can do. The toolbar, the menus and the
 * command registry decide what to show and enable from it, never from the driver's name
 * scattered in the code. Each value records what the engine supports AND whether Lume
 * implements it for that driver. Engines Lume has no driver for yet (MySQL / MariaDB, SQL
 * Server) are declared too, with available = false, so their rules are written down and
 * testable before a driver exists.
 *
 * Read-only (TBR-013) is described with the level of protection the engine really gives;
 * the UI shows that level and never promises more.
 */
#include "lume.h"

/* How strongly "read-only in Lume" is enforced on a connection. */
enum {
    RO_NONE,          /* not read-only */
    RO_ENGINE,        /* enforced by the engine for this connection (SQLite PRAGMA query_only) */
    RO_SESSION,       /* the session's default (PostgreSQL); SQL in the editor could switch it back */
    RO_LUME_CHECK,    /* only Lume's check before running (Oracle): the server still allows writes */
};

typedef struct DriverCaps {
    const char *driver;          /* DriverInfo id: "sqlite", "postgres", ... */
    const char *name;            /* display name */
    bool available;              /* Lume has a driver for it */
    bool schemas;                /* a default schema can be chosen for the session */
    bool attached_dbs;           /* SQLite: ATTACHed databases (shown, not a fictional schema) */
    bool transactions;           /* manual transactions (BEGIN / COMMIT / ROLLBACK or implicit) */
    bool isolation;              /* isolation level can be set */
    bool explain;                /* a plan can be shown */
    bool explain_analyze;        /* a plan with real execution (may have side effects) */
    bool cancel;                 /* a running statement can be interrupted */
    bool parameters;             /* :name / ? placeholders are filled by Lume (literals, quoted) */
    int read_only;               /* RO_* level this engine gives to "read-only in Lume" */
    const char *explain_prefix;  /* text put before the statement ("EXPLAIN QUERY PLAN ") */
    const char *explain_analyze_prefix;
    const char *set_schema_fmt;  /* printf format with one %s (quoted identifier), or NULL */
    const char *isolation_levels;   /* '|'-separated, or NULL */
    const char *read_only_sql;   /* RO_SESSION: the statement that makes the session read-only (NULL: PostgreSQL's) */
} DriverCaps;

const DriverCaps *caps_for(const char *driver_id);   /* NULL for an unknown id */
int caps_count(void);
const DriverCaps *caps_at(int i);
const char *caps_ro_text(int level);   /* one sentence for tooltips: what the protection really is */

/* Applies "read-only in Lume" to a freshly opened connection (UI thread, connection idle).
 * Returns the RO_* level obtained, RO_NONE when it is not read-only. */
int caps_apply_read_only(Connection *c);
/* Lume's own check for RO_LUME_CHECK drivers and PROD confirmations: true when the statement
 * may write. A conservative keyword test (anything not clearly a read is a write); the server's
 * permissions remain the real rule, the UI says so (TBR-029). */
bool caps_sql_may_write(const char *sql, int len);
/* What the connection's login may write, from the engine's own grant catalogs (BL-051): a warning
 * aid, not a proof (roles granted indirectly may be missed). note: what was found. Idle connection only. */
enum { AG_UNKNOWN, AG_READONLY, AG_WRITES };
int caps_write_grants(Connection *c, char *note, int n);
