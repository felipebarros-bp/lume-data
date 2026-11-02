/* SPDX-License-Identifier: AGPL-3.0-only */
/* Read-only query policy: see sqlguard.h. */
#include "sqlguard.h"
#include "sqllex.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAXTOK 16384

static void say(char *why, int n, const char *s) { if (why && n > 0) snprintf(why, (size_t)n, "%s", s); }

/* statement keywords that write, change structure or permissions, or run other code */
static const char *const deny_words[] = {
    "INSERT", "UPDATE", "DELETE", "MERGE", "UPSERT", "DROP", "CREATE", "ALTER", "TRUNCATE", "RENAME",
    "GRANT", "REVOKE", "DENY", "CALL", "EXEC", "EXECUTE", "COPY", "LOCK", "UNLOCK", "VACUUM", "ANALYZE",
    "ATTACH", "DETACH", "PRAGMA", "REINDEX", "CLUSTER", "REFRESH", "INTO", "SET", "DO", "HANDLER",
    "LOAD", "IMPORT", "BACKUP", "RESTORE", "SHUTDOWN", "KILL", "WAITFOR", "DECLARE", "BEGIN", "COMMIT",
    "ROLLBACK", "SAVEPOINT", "RELEASE", "PREPARE", "DEALLOCATE", "LISTEN", "NOTIFY", "DISCARD", "RESET",
    "SECURITY", "OUTFILE", "DUMPFILE", NULL
};
/* functions with side effects (time, files, network, sequences, locks, dynamic SQL, OS) */
static const char *const deny_funcs[] = {
    "pg_sleep", "pg_sleep_for", "pg_sleep_until", "pg_terminate_backend", "pg_cancel_backend", "pg_reload_conf",
    "pg_rotate_logfile", "pg_read_file", "pg_read_binary_file", "pg_ls_dir", "pg_stat_file", "pg_ls_logdir",
    "pg_ls_waldir", "pg_ls_tmpdir", "pg_file_write", "pg_file_unlink", "pg_file_rename", "lo_import", "lo_export",
    "lo_unlink", "lo_create", "lo_creat", "lo_from_bytea", "lo_put", "lowrite", "lo_open", "dblink", "dblink_exec",
    "dblink_connect", "dblink_send_query", "nextval", "setval", "set_config", "pg_advisory_lock",
    "pg_advisory_xact_lock", "pg_try_advisory_lock", "pg_try_advisory_xact_lock", "pg_advisory_lock_shared",
    "pg_notify", "txid_current", "pg_current_xact_id", "pg_create_restore_point", "pg_switch_wal", "pg_promote",
    "pg_logical_emit_message", "pg_create_logical_replication_slot", "pg_drop_replication_slot", "query_to_xml",
    "query_to_xml_and_xmlschema", "cursor_to_xml", "pg_stat_reset", "pg_backend_pid_kill",
    "sleep", "benchmark", "get_lock", "release_lock", "release_all_locks", "load_file", "sys_exec", "sys_eval",
    "master_pos_wait", "source_pos_wait", "uuid_short", "last_insert_id",
    "xp_cmdshell", "openrowset", "opendatasource", "openquery", "openxml",
    "load_extension", "readfile", "writefile", "edit", "fts3_tokenizer", "sqlite_compileoption_used_unsafe",
    NULL
};
/* function / package prefixes with side effects (Oracle packages, SQL Server extended procedures) */
static const char *const deny_prefixes[] = { "dbms_", "utl_", "xp_", "sp_", "owa_", "htp", "apex_", "ctx_ddl", NULL };

static bool word_is(const char *s, int len, const char *w) { return (int)strlen(w) == len && !strncasecmp(s, w, (size_t)len); }
static bool in_list(const char *s, int len, const char *const *l) { for (int i = 0; l[i]; i++) if (word_is(s, len, l[i])) return true; return false; }
static bool has_prefix(const char *s, int len, const char *const *l) { for (int i = 0; l[i]; i++) { int pl = (int)strlen(l[i]); if (len >= pl && !strncasecmp(s, l[i], (size_t)pl)) return true; } return false; }

bool sqlguard_check(const char *sql, int dialect, char *why, int whyn) {
    if (!sql) { say(why, whyn, "no statement"); return false; }
    int len = (int)strlen(sql);
    if (len == 0) { say(why, whyn, "empty statement"); return false; }
    if (len > SQLGUARD_MAX_LEN) { say(why, whyn, "statement too long for the read-only policy (64 KB)"); return false; }
    SqlTok *t = malloc(sizeof *t * MAXTOK);
    if (!t) { say(why, whyn, "out of memory"); return false; }
    int n = sql_lex(sql, len, dialect, t, MAXTOK);
    bool ok = false;
    if (n >= MAXTOK) { say(why, whyn, "statement too long for the read-only policy (tokens)"); goto done; }
    /* significant tokens only */
    int *sig = malloc(sizeof(int) * (size_t)(n + 1)); int ns = 0;
    if (!sig) { say(why, whyn, "out of memory"); goto done; }
    for (int i = 0; i < n; i++) {
        if (t[i].unterminated) { say(why, whyn, "unterminated string, comment or identifier"); free(sig); goto done; }
        if (t[i].kind == TK_COMMENT) {
            /* MySQL / MariaDB run the content of executable comments and honour optimizer hints */
            if (t[i].len >= 3 && !strncmp(sql + t[i].off, "/*!", 3)) { say(why, whyn, "executable comment (/*! ... */) is not allowed"); free(sig); goto done; }
            continue;
        }
        if (t[i].kind == TK_WS) continue;
        if (t[i].kind == TK_BATCH) { say(why, whyn, "more than one batch (GO) is not allowed"); free(sig); goto done; }
        if (t[i].kind == TK_BODY) { say(why, whyn, "procedural bodies ($$ ... $$) are not allowed"); free(sig); goto done; }
        sig[ns++] = i;
    }
    if (!ns) { say(why, whyn, "empty statement"); free(sig); goto done; }
    /* one statement: a ';' may only end the text */
    while (ns > 0 && t[sig[ns - 1]].kind == TK_SEMI) ns--;
    for (int k = 0; k < ns; k++) if (t[sig[k]].kind == TK_SEMI) { say(why, whyn, "more than one statement is not allowed"); free(sig); goto done; }
    if (!ns) { say(why, whyn, "empty statement"); free(sig); goto done; }
    /* first word: SELECT or WITH (parentheses allowed before it) */
    int f = 0; while (f < ns && t[sig[f]].kind == TK_LPAREN) f++;
    if (f >= ns || t[sig[f]].kind != TK_WORD || !(tok_is(sql, &t[sig[f]], "SELECT") || tok_is(sql, &t[sig[f]], "WITH"))) {
        say(why, whyn, "only SELECT (or WITH ... SELECT) queries are allowed"); free(sig); goto done;
    }
    for (int k = 0; k < ns; k++) {
        const SqlTok *x = &t[sig[k]];
        if (x->kind != TK_WORD) {
            if (x->kind == TK_OP && x->len == 2 && !strncmp(sql + x->off, ":=", 2)) { say(why, whyn, "variable assignment (:=) is not allowed"); free(sig); goto done; }
            continue;
        }
        const char *w = sql + x->off; int wl = x->len;
        bool call = k + 1 < ns && t[sig[k + 1]].kind == TK_LPAREN;
        bool dotted_prev = k > 0 && t[sig[k - 1]].kind == TK_DOT;   /* alias.column: a column name, not a keyword */
        bool dotted_next = k + 1 < ns && t[sig[k + 1]].kind == TK_DOT;
        if (wl > 0 && w[0] == '@' && dialect == DIALECT_MSSQL && wl > 1 && w[1] == '@') continue;   /* @@VERSION and friends: read-only */
        if (!dotted_prev && !call && in_list(w, wl, deny_words)) {
            char m[160]; snprintf(m, sizeof m, "keyword %.*s is not allowed in a read-only query", wl > 40 ? 40 : wl, w);
            say(why, whyn, m); free(sig); goto done;
        }
        /* REPLACE INTO (MySQL) / COMMENT ON: statements; replace(...) is a string function */
        if (!dotted_prev && word_is(w, wl, "REPLACE") && k + 1 < ns && t[sig[k + 1]].kind == TK_WORD && tok_is(sql, &t[sig[k + 1]], "INTO")) { say(why, whyn, "REPLACE INTO is not allowed"); free(sig); goto done; }
        if (!dotted_prev && word_is(w, wl, "COMMENT") && k + 1 < ns && t[sig[k + 1]].kind == TK_WORD && tok_is(sql, &t[sig[k + 1]], "ON")) { say(why, whyn, "COMMENT ON is not allowed"); free(sig); goto done; }
        /* FOR SHARE / FOR KEY SHARE: row locks */
        if (word_is(w, wl, "FOR") && k + 1 < ns && t[sig[k + 1]].kind == TK_WORD && (tok_is(sql, &t[sig[k + 1]], "SHARE") || tok_is(sql, &t[sig[k + 1]], "KEY") || tok_is(sql, &t[sig[k + 1]], "NO"))) { say(why, whyn, "row locks (FOR SHARE / FOR UPDATE) are not allowed"); free(sig); goto done; }
        if ((call || dotted_next) && (in_list(w, wl, deny_funcs) || has_prefix(w, wl, deny_prefixes))) {
            char m[160]; snprintf(m, sizeof m, "%.*s has side effects and is not allowed", wl > 40 ? 40 : wl, w);
            say(why, whyn, m); free(sig); goto done;
        }
        /* schema-qualified call: pg_catalog.pg_sleep(...), sys.xp_cmdshell, dbo.sp_x */
        if (call && dotted_prev && (in_list(w, wl, deny_funcs) || has_prefix(w, wl, deny_prefixes))) {
            char m[160]; snprintf(m, sizeof m, "%.*s has side effects and is not allowed", wl > 40 ? 40 : wl, w);
            say(why, whyn, m); free(sig); goto done;
        }
    }
    ok = true;
    say(why, whyn, "");
    free(sig);
done:
    free(t);
    return ok;
}

static char *dupf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static char *dupf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    char *s = malloc((size_t)n + 1); if (!s) return NULL;
    va_start(ap, fmt); vsnprintf(s, (size_t)n + 1, fmt, ap); va_end(ap);
    return s;
}

int sqlguard_wrap(int dialect, const char *sql, int timeout_s, char **out, int max, int *result, bool *finally_last, char *note, int nn) {
    int n = 0;
    *finally_last = true;
    if (max < 4) return 0;
    /* the query without a trailing ';' (it runs as one statement of a list) */
    size_t L = strlen(sql); while (L && (isspace((unsigned char)sql[L - 1]) || sql[L - 1] == ';')) L--;
    char *q = malloc(L + 1); memcpy(q, sql, L); q[L] = 0;
    switch (dialect) {
    case DIALECT_PG:
        out[n++] = strdup("BEGIN READ ONLY");
        if (timeout_s > 0) out[n++] = dupf("SET LOCAL statement_timeout = '%ds'", timeout_s);
        *result = n; out[n++] = q;
        out[n++] = strdup("ROLLBACK");
        if (note) snprintf(note, (size_t)nn, "read-only transaction (BEGIN READ ONLY)%s, rolled back", timeout_s > 0 ? ", server timeout" : "");
        break;
    case DIALECT_MYSQL: case DIALECT_MARIADB:
        out[n++] = strdup("START TRANSACTION READ ONLY");
        *result = n; out[n++] = q;
        out[n++] = strdup("ROLLBACK");
        if (note) snprintf(note, (size_t)nn, "read-only transaction (START TRANSACTION READ ONLY), rolled back; timeout enforced by Lume (cancel)");
        break;
    case DIALECT_ORACLE:
        out[n++] = strdup("SET TRANSACTION READ ONLY");
        *result = n; out[n++] = q;
        out[n++] = strdup("ROLLBACK");
        if (note) snprintf(note, (size_t)nn, "read-only transaction (SET TRANSACTION READ ONLY), rolled back; timeout enforced by Lume (cancel)");
        break;
    case DIALECT_SQLITE:
        out[n++] = strdup("PRAGMA query_only = ON");
        *result = n; out[n++] = q;
        out[n++] = strdup("PRAGMA query_only = OFF");
        if (note) snprintf(note, (size_t)nn, "PRAGMA query_only (writes refused by SQLite); timeout enforced by Lume (cancel)");
        break;
    case DIALECT_MSSQL:
        out[n++] = strdup("BEGIN TRANSACTION");
        *result = n; out[n++] = q;
        out[n++] = strdup("IF @@TRANCOUNT > 0 ROLLBACK");
        if (note) snprintf(note, (size_t)nn, "SQL Server has no read-only transaction: lexical policy + rollback; use a read-only login for business queries");
        break;
    default:
        *result = n; out[n++] = q; *finally_last = false;
        if (note) snprintf(note, (size_t)nn, "no read-only transaction for this engine: lexical policy only");
        break;
    }
    return n;
}

void sqlguard_free(char **out, int n) { for (int i = 0; i < n; i++) { free(out[i]); out[i] = NULL; } }
