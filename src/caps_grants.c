/* SPDX-License-Identifier: AGPL-3.0-only */
/* The write grants of a connection's login (BL-051), from the engine's catalogs: see caps.h. Kept apart
 * from caps.c, which the headless checks link without the result-set code. */
#include "caps.h"
#include "conn.h"
#include "db.h"
#include "rs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- write grants of the login (BL-051) ---- */
#define MY_GRANTEE "CONCAT('''', SUBSTRING_INDEX(CURRENT_USER(),'@',1), '''@''', SUBSTRING_INDEX(CURRENT_USER(),'@',-1), '''')"
int caps_write_grants(Connection *c, char *note, int n) {
    note[0] = 0;
    if (!c || !c->db || c->db->busy) { snprintf(note, (size_t)n, "not connected"); return AG_UNKNOWN; }
    const char *d = c->driver, *sql = NULL;
    if (!strcmp(d, "sqlite")) {
        /* no logins: read-only only as the engine's query_only of "read-only in Lume" */
        if (c->read_only && c->ro_level == RO_ENGINE) { snprintf(note, (size_t)n, "SQLite with query_only: the engine refuses writes"); return AG_READONLY; }
        snprintf(note, (size_t)n, "SQLite has no logins: mark the connection read-only in Lume"); return AG_WRITES;
    }
    if (!strcmp(d, "postgres"))
        sql = "SELECT (CASE WHEN r.rolsuper THEN 1000 ELSE 0 END) + (SELECT count(*) FROM information_schema.role_table_grants g "
              "WHERE g.privilege_type IN ('INSERT','UPDATE','DELETE','TRUNCATE') AND g.table_schema NOT IN ('pg_catalog','information_schema')) "
              "FROM pg_roles r WHERE r.rolname = current_user";
    else if (!strcmp(d, "mysql") || !strcmp(d, "mariadb"))
        sql = "SELECT (SELECT count(*) FROM information_schema.user_privileges WHERE grantee = " MY_GRANTEE " AND privilege_type IN ('INSERT','UPDATE','DELETE','DROP','ALTER','CREATE','SUPER')) + "
              "(SELECT count(*) FROM information_schema.schema_privileges WHERE grantee = " MY_GRANTEE " AND privilege_type IN ('INSERT','UPDATE','DELETE','DROP','ALTER','CREATE')) + "
              "(SELECT count(*) FROM information_schema.table_privileges WHERE grantee = " MY_GRANTEE " AND privilege_type IN ('INSERT','UPDATE','DELETE','DROP','ALTER'))";
    else if (!strcmp(d, "mssql"))
        sql = "SELECT ISNULL(IS_SRVROLEMEMBER('sysadmin'),0) * 1000 + ISNULL(IS_ROLEMEMBER('db_owner'),0) * 100 + ISNULL(IS_ROLEMEMBER('db_datawriter'),0) * 10 + "
              "(SELECT count(*) FROM fn_my_permissions(NULL, 'DATABASE') WHERE permission_name IN ('INSERT','UPDATE','DELETE','ALTER','CONTROL','CREATE TABLE'))";
    else if (!strcmp(d, "oracle"))
        sql = "SELECT (SELECT count(*) FROM session_privs WHERE privilege IN ('INSERT ANY TABLE','UPDATE ANY TABLE','DELETE ANY TABLE','DROP ANY TABLE','ALTER ANY TABLE','CREATE ANY TABLE')) + "
              "(SELECT count(*) FROM user_tab_privs WHERE privilege IN ('INSERT','UPDATE','DELETE','ALTER') AND grantee = USER) + "
              "(SELECT count(*) FROM role_tab_privs WHERE privilege IN ('INSERT','UPDATE','DELETE','ALTER')) + (SELECT count(*) FROM user_tables) FROM dual";
    if (!sql) { snprintf(note, (size_t)n, "no grant check for this engine"); return AG_UNKNOWN; }
    ResultSet rs; c->db->drv->exec(c->db, sql, -1, 1, &rs);
    int st = AG_UNKNOWN;
    if (rs.ok && rs.nrows > 0 && rs_cell(&rs, 0, 0)) {
        long v = atol(rs_cell(&rs, 0, 0));
        if (v == 0) { st = AG_READONLY; snprintf(note, (size_t)n, "no write grants found for this login"); }
        else { st = AG_WRITES; snprintf(note, (size_t)n, "%s%ld write grant(s) found for this login", v >= 1000 ? "superuser / admin; " : "", v % 1000); }
    } else snprintf(note, (size_t)n, "the grants could not be read (%.120s)", rs.error[0] ? rs.error : "no result");
    rs_free(&rs);
    return st;
}
