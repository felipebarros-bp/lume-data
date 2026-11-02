/* SPDX-License-Identifier: AGPL-3.0-only */
/* Headless checks of the editing model against SQLite (US-048 .. US-050): two connections
 * on one file simulate another user. Nothing here touches the app's own data. */
#include "db.h"
#include "edit.h"
#include "grid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

static int failures = 0;
static void check(bool ok, const char *what, const char *detail) {
    printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) failures++;
}
static bool run(Db *db, const char *sql) {
    ResultSet rs; db->drv->exec(db, sql, -1, 0, &rs);
    bool ok = rs.ok; if (!ok) printf("      exec failed: %s\n      %s\n", rs.error, sql);
    rs_free(&rs); return ok;
}
static char *scalar(Db *db, const char *sql) {
    ResultSet rs; db->drv->exec(db, sql, -1, 0, &rs);
    char *v = rs.ok && rs.nrows ? (rs_cell(&rs, 0, 0) ? strdup(rs_cell(&rs, 0, 0)) : NULL) : strdup("<error>");
    rs_free(&rs); return v;
}
static bool scalar_is(Db *db, const char *sql, const char *want) {
    char *v = scalar(db, sql); bool ok = want ? (v && strcmp(v, want) == 0) : v == NULL; free(v); return ok;
}

typedef struct { ResultSet rs; Grid g; EditSet e; DbTable t; } View;
static void view_open(View *v, Db *db, const char *table, const char *order) {
    char q[64], sql[512];
    db->drv->quote_ident(table, q, sizeof q);
    db->drv->select_sql(q, NULL, order, 100, sql, sizeof sql);
    db->drv->exec(db, sql, -1, 0, &v->rs);
    grid_init(&v->g, &v->rs);
    edit_init(&v->e, &v->rs, db->drv, q, v->g.numeric);
    memset(&v->t, 0, sizeof v->t); v->t.name = (char *)table;
    char err[200];
    db->drv->list_columns(db, &v->t, err, sizeof err);
    int pk[8], npk = 0;
    for (int c = 0; c < v->t.ncols; c++) {
        int src = -1;
        for (int k = 0; k < v->rs.ncols; k++) if (strcmp(v->rs.cols[k], v->t.cols[c].name) == 0) src = k;
        if (src < 0) continue;
        if (v->t.cols[c].pk && npk < 8) pk[npk++] = src;
        if (v->t.cols[c].generated) edit_set_readonly(&v->e, src, "generated column");
        if (v->t.cols[c].identity_always) edit_set_readonly(&v->e, src, "identity column (generated always)");
    }
    edit_set_key(&v->e, pk, npk);
}
static void view_close(View *v) {
    edit_free(&v->e); grid_free(&v->g); rs_free(&v->rs);
    for (int c = 0; c < v->t.ncols; c++) { free(v->t.cols[c].name); free(v->t.cols[c].type); }
    free(v->t.cols);
}
static int col(View *v, const char *name) { for (int c = 0; c < v->rs.ncols; c++) if (strcmp(v->rs.cols[c], name) == 0) return c; return -1; }

static int hook_calls; static Db *hook_db2; static const char *hook_sql;
static void hook_before(void *ud, int i, const char *sql) { (void)ud; (void)sql; hook_calls++; if (i == hook_calls - 1 && hook_sql) run(hook_db2, hook_sql); }

int main(void) {
    const char *path = "/tmp/lume_edit_check.db";
    remove(path);
    { sqlite3 *h = NULL; sqlite3_open(path, &h); sqlite3_close(h); }   /* the driver opens existing files only */
    char err[256];
    Db *db1 = db_open(&db_sqlite_driver, path, err, sizeof err);
    Db *db2 = db_open(&db_sqlite_driver, path, err, sizeof err);
    if (!db1 || !db2) { printf("cannot open %s: %s\n", path, err); return 2; }
    run(db1, "PRAGMA busy_timeout = 200");
    run(db2, "PRAGMA busy_timeout = 200");
    run(db1, "CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT UNIQUE, qty INTEGER, price REAL, note TEXT, big INTEGER, "
             "doc TEXT, password TEXT, twice INTEGER GENERATED ALWAYS AS (qty * 2) VIRTUAL, version INTEGER DEFAULT 1)");
    run(db1, "INSERT INTO items (id, name, qty, price, note, big, doc, password) VALUES (1, 'Ana', 2, 1.5, NULL, 9007199254740993, '{\"a\": 1}', 's3cret'), (2, 'Bia', 3, 2.25, 'n2', 5, NULL, NULL), (3, 'Cid', 4, 0.1, 'n3', 7, NULL, NULL)");
    run(db1, "CREATE TABLE pair (a INTEGER, b TEXT, v TEXT, PRIMARY KEY (a, b))");
    run(db1, "INSERT INTO pair VALUES (1, 'x', 'one'), (1, 'y', 'two')");

    View v; ApplyResult ar; char d[600];
    /* strategy: pk + snapshot, floating point and generated left out, version column noticed */
    view_open(&v, db1, "items", "\"id\"");
    check(v.e.npk == 1 && v.e.pkcols[0] == col(&v, "id"), "primary key found in the result", NULL);
    edit_describe_strategy(&v.e, d, sizeof d);
    check(strstr(d, "snapshot of 8") && strstr(d, "price (floating point") && strstr(d, "version column version"), "strategy: key + snapshot of comparable columns; REAL left out and named; version column included", d);
    check(edit_col_readonly(&v.e, col(&v, "twice")) != NULL && edit_col_readonly(&v.e, col(&v, "name")) == NULL, "generated column is read-only with a reason, ordinary columns editable", edit_col_readonly(&v.e, col(&v, "twice")));
    char why[200];
    check(!edit_validate(&v.e, col(&v, "twice"), "9", why, sizeof why) && strstr(why, "generated"), "validation refuses a generated column", why);
    check(!edit_validate(&v.e, col(&v, "qty"), "abc", why, sizeof why), "validation refuses text in a numeric column", why);
    check(edit_validate(&v.e, col(&v, "big"), "9223372036854775807", why, sizeof why) && !edit_validate(&v.e, col(&v, "big"), "123456789012345678901234567890", why, sizeof why) && strstr(why, "exactly"), "int64 accepted; a 30-digit integer is refused on SQLite (would be rounded to a double)", why);
    v.rs.kinds[col(&v, "doc")] = CK_JSON;
    check(!edit_validate(&v.e, col(&v, "doc"), "{\"a\": }", why, sizeof why) && strstr(why, "JSON"), "invalid JSON is refused with the offset", why);
    check(edit_validate(&v.e, col(&v, "doc"), "{\"a\": [1, 2]}", why, sizeof why), "valid JSON passes", NULL);

    /* 1. conflict on a column that was NOT edited (full snapshot) */
    edit_put(&v.e, 0, col(&v, "name"), "Ana Maria", false);
    run(db2, "UPDATE items SET note = 'changed elsewhere' WHERE id = 1");
    edit_apply(&v.e, db1, NULL, &ar);
    check(ar.status == APPLY_CONFLICT && ar.stmt == 0 && ar.affected == 0, "another session changed an unedited column: conflict, batch rolled back", ar.message);
    check(strstr(ar.conflict, "note: you saw NULL, now changed elsewhere") != NULL, "conflict diagnosis names the column with old and new value", ar.conflict);
    check(v.e.nedits == 1 && edit_pending_count(&v.e) == 1, "pending change kept after the conflict", NULL);
    check(scalar_is(db1, "SELECT name FROM items WHERE id = 1", "Ana"), "nothing was written", NULL);
    /* masked review of a sensitive column */
    edit_put(&v.e, 1, col(&v, "password"), "hunter2", false);
    edit_build(&v.e);
    bool masked = true;
    for (int i = 0; i < v.e.nstmts; i++) if (strstr(v.e.stmts[i].shown, "hunter2") || strstr(v.e.stmts[i].note, "hunter2")) masked = false;
    bool runs_real = false;
    for (int i = 0; i < v.e.nstmts; i++) if (strstr(v.e.stmts[i].sql, "'hunter2'")) runs_real = true;
    check(masked && runs_real, "review shows sensitive literals masked while the statement that runs carries the value", NULL);
    view_close(&v);

    /* 2. row deleted by someone else */
    view_open(&v, db1, "items", "\"id\"");
    edit_put(&v.e, 2, col(&v, "qty"), "9", false);
    run(db2, "DELETE FROM items WHERE id = 3");
    edit_apply(&v.e, db1, NULL, &ar);
    check(ar.status == APPLY_CONFLICT && strstr(ar.conflict, "no longer exists") != NULL, "row deleted by someone else is reported as such", ar.conflict);
    view_close(&v);
    run(db2, "INSERT INTO items (id, name, qty, price, note, big) VALUES (3, 'Cid', 4, 0.1, 'n3', 7)");

    /* 3. NULL -> value on the edited column and NULL in the snapshot (IS NULL) */
    view_open(&v, db1, "items", "\"id\"");
    run(db2, "UPDATE items SET note = NULL WHERE id = 1");
    view_close(&v);
    view_open(&v, db1, "items", "\"id\"");
    edit_put(&v.e, 0, col(&v, "note"), "filled", false);
    edit_build(&v.e);
    check(strstr(v.e.stmts[0].sql, "\"note\" IS NULL") != NULL && strstr(v.e.stmts[0].sql, "\"doc\" = '{\"a\": 1}'") != NULL && strstr(v.e.stmts[0].sql, "\"qty\" = '2'") != NULL, "WHERE uses IS NULL for NULL snapshot values and quotes JSON text", v.e.stmts[0].sql);
    edit_apply(&v.e, db1, NULL, &ar);
    check(ar.status == APPLY_OK && scalar_is(db1, "SELECT note FROM items WHERE id = 1", "filled"), "NULL -> value applied", ar.message);
    view_close(&v);

    /* 4. composite primary key */
    view_open(&v, db1, "pair", "\"a\", \"b\"");
    check(v.e.npk == 2, "composite key: two key columns", NULL);
    edit_put(&v.e, 1, col(&v, "v"), "TWO", false);
    edit_build(&v.e);
    check(strstr(v.e.stmts[0].sql, "WHERE \"a\" = '1' AND \"b\" = 'y' AND \"v\" = 'two'") != NULL, "composite key: both columns in the WHERE", v.e.stmts[0].sql);
    edit_apply(&v.e, db1, NULL, &ar);
    check(ar.status == APPLY_OK && scalar_is(db1, "SELECT v FROM pair WHERE a = 1 AND b = 'y'", "TWO"), "composite key update applied", NULL);
    view_close(&v);

    /* 5. atomicity: the second statement violates UNIQUE; the first must not persist */
    view_open(&v, db1, "items", "\"id\"");
    edit_put(&v.e, 0, col(&v, "qty"), "100", false);
    edit_put(&v.e, 1, col(&v, "name"), "Cid", false);     /* duplicate of row 3 */
    edit_apply(&v.e, db1, NULL, &ar);
    check(ar.status == APPLY_STMT_FAILED && ar.stmt == 1 && strstr(ar.message, "nothing was written"), "constraint violation on the second statement: whole batch rolled back", ar.message);
    check(scalar_is(db1, "SELECT qty FROM items WHERE id = 1", "2") && v.e.nedits == 2, "first statement not persisted; pending changes kept", NULL);
    view_close(&v);

    /* 6. delete + insert + big integer round trip; generated column stays DEFAULT on duplicate */
    view_open(&v, db1, "items", "\"id\"");
    edit_row_toggle_delete(&v.e, 1);
    int nr = edit_row_add(&v.e, 0);
    check(nr == 3 && edit_find(&v.e, nr, col(&v, "twice")) == NULL && edit_find(&v.e, nr, col(&v, "id")) == NULL && edit_find(&v.e, nr, col(&v, "big")) != NULL, "duplicate row: key and generated column take DEFAULT, data copied", NULL);
    edit_put(&v.e, nr, col(&v, "name"), "Dup", false);
    edit_put(&v.e, nr, col(&v, "big"), "9223372036854775807", false);
    edit_build(&v.e);
    int kinds[3] = { 0 };
    for (int i = 0; i < v.e.nstmts; i++) kinds[v.e.stmts[i].kind]++;
    check(v.e.nstmts == 2 && kinds[1] == 1 && kinds[2] == 1 && strstr(v.e.stmts[0].note, "cannot be undone"), "DELETE flagged irreversible; INSERT generated", v.e.stmts[0].note);
    check(strstr(v.e.stmts[1].sql, "9223372036854775807") && !strstr(v.e.stmts[1].sql, "'9223372036854775807'"), "an int64 is written bare", v.e.stmts[1].sql);
    edit_apply(&v.e, db1, NULL, &ar);
    check(ar.status == APPLY_OK && scalar_is(db1, "SELECT big FROM items WHERE name = 'Dup'", "9223372036854775807") && scalar_is(db1, "SELECT COUNT(*) FROM items WHERE id = 2", "0"), "delete and insert committed; int64 exact", ar.message);
    view_close(&v);
    view_open(&v, db1, "items", "\"id\"");
    int r = -1; for (int i = 0; i < v.rs.nrows; i++) if (strcmp(rs_cell(&v.rs, i, col(&v, "name")), "Dup") == 0) r = i;
    edit_put(&v.e, r, col(&v, "price"), "0.1", false);
    edit_build(&v.e);
    check(!strstr(v.e.stmts[0].sql, "\"price\" = 1.5") && strstr(v.e.stmts[0].sql, "\"qty\" = '2'"), "REAL column stays out of the WHERE while integers are compared (quoted: the column type converts)", v.e.stmts[0].sql);
    edit_apply(&v.e, db1, NULL, &ar);
    check(ar.status == APPLY_OK && scalar_is(db1, "SELECT price FROM items WHERE name = 'Dup'", "0.1"), "editing a REAL column still works (it is just not part of the conflict check)", ar.message);
    view_close(&v);

    /* 7. COMMIT fails (another connection holds a read lock): reported, not applied, pending kept */
    view_open(&v, db1, "items", "\"id\"");
    edit_put(&v.e, 0, col(&v, "qty"), "77", false);
    ResultSet lock; db2->drv->exec(db2, "SELECT id FROM items", -1, 1, &lock);   /* open cursor = shared lock */
    edit_apply(&v.e, db1, NULL, &ar);
    rs_free(&lock);
    check(ar.status == APPLY_COMMIT_FAILED && strstr(ar.message, "nothing was written"), "COMMIT blocked by a reader: reported as commit failed, nothing written", ar.message);
    check(scalar_is(db1, "SELECT qty FROM items WHERE id = 1", "2") && v.e.nedits == 1, "value unchanged, pending change kept", NULL);
    edit_apply(&v.e, db1, NULL, &ar);
    check(ar.status == APPLY_OK && scalar_is(db1, "SELECT qty FROM items WHERE id = 1", "77"), "retry after the lock is gone applies", ar.message);
    view_close(&v);

    /* 8. hook runs before each statement and before COMMIT (used by the PostgreSQL kill test) */
    view_open(&v, db1, "items", "\"id\"");
    edit_put(&v.e, 0, col(&v, "qty"), "78", false);
    hook_calls = 0; hook_db2 = db2; hook_sql = NULL;
    EditHooks h = { hook_before, NULL };
    edit_apply(&v.e, db1, &h, &ar);
    check(ar.status == APPLY_OK && hook_calls == 2, "hook called for the statement and for COMMIT", NULL);
    view_close(&v);

    /* 8b. SQL INSERT export round trip (US-053): the copied statements rebuild the rows */
    view_open(&v, db1, "items", "\"id\"");
    grid_select_all(&v.g);
    char *ins = grid_copy(&v.g, FMT_SQL, true, "items_copy");
    run(db1, "CREATE TABLE items_copy AS SELECT * FROM items WHERE 0");
    bool all_ok = true;
    for (char *stmt = ins, *nl; stmt && *stmt; stmt = nl ? nl + 1 : NULL) { nl = strchr(stmt, '\n'); if (nl) *nl = 0; if (*stmt && !run(db1, stmt)) all_ok = false; }
    free(ins);
    check(all_ok && scalar_is(db1, "SELECT COUNT(*) FROM items_copy", "3") && scalar_is(db1, "SELECT COUNT(*) FROM (SELECT id, name, qty, price, note, big, doc, password, version FROM items EXCEPT SELECT id, name, qty, price, note, big, doc, password, version FROM items_copy)", "0"), "SQL INSERT export re-executed yields identical rows (NULL, '', JSON text, int64)", NULL);
    view_close(&v);

    /* 8c. '' and NULL produce different SQL; binary columns are refused (US-050) */
    view_open(&v, db1, "items", "\"id\"");
    edit_put(&v.e, 0, col(&v, "note"), "", false);
    edit_put(&v.e, 1, col(&v, "note"), NULL, true);
    edit_build(&v.e);
    check(v.e.nstmts == 2 && strstr(v.e.stmts[0].sql, "\"note\" = ''") && strstr(v.e.stmts[1].sql, "\"note\" = NULL"), "empty string and NULL are different values in the generated SQL", v.e.stmts[0].sql);
    v.rs.kinds[col(&v, "doc")] = CK_BINARY;
    check(!edit_validate(&v.e, col(&v, "doc"), "\\x00", why, sizeof why) && strstr(why, "binary"), "a binary column refuses edits with a reason", why);
    view_close(&v);

    /* 8d. values that break naive SQL, applied and read back (US-050 / US-053) */
    run(db1, "CREATE TABLE tricky (id INTEGER PRIMARY KEY, t TEXT, b BLOB)");
    run(db1, "INSERT INTO tricky VALUES (1, 'plain', X'00FF10'), (2, NULL, NULL), (3, '', NULL)");
    view_open(&v, db1, "tricky", "\"id\"");
    /* unmodified: typing the stored value back removes the pending change, no statement */
    edit_put_text(&v.e, 0, col(&v, "t"), "changed");
    edit_put_text(&v.e, 0, col(&v, "t"), "plain");
    edit_build(&v.e);
    check(v.e.nedits == 0 && v.e.nstmts == 0, "typing the stored value back leaves the cell unmodified (no statement)", NULL);
    /* '' over NULL and NULL over '' are real edits; '' typed into an '' cell is not */
    edit_put_text(&v.e, 1, col(&v, "t"), "");
    edit_put_text(&v.e, 2, col(&v, "t"), "");
    check(v.e.nedits == 1 && edit_find(&v.e, 1, col(&v, "t")) != NULL, "'' typed over NULL is an edit; '' over '' is not", NULL);
    edit_put(&v.e, 2, col(&v, "t"), NULL, true);
    edit_put_text(&v.e, 0, col(&v, "t"), "O'Brien; DROP TABLE tricky; --\nline2 \"q\" \\ end");
    edit_apply(&v.e, db1, NULL, &ar);
    check(ar.status == APPLY_OK, "quotes, semicolon, comment marker, newline, backslash applied", ar.message);
    check(scalar_is(db1, "SELECT t FROM tricky WHERE id = 1", "O'Brien; DROP TABLE tricky; --\nline2 \"q\" \\ end") && scalar_is(db1, "SELECT COUNT(*) FROM tricky", "3"), "read back byte for byte; nothing executed from the value", NULL);
    check(scalar_is(db1, "SELECT typeof(t) || ':' || length(t) FROM tricky WHERE id = 2", "text:0") && scalar_is(db1, "SELECT typeof(t) FROM tricky WHERE id = 3", "null"), "'' and NULL read back as different values (text:0 vs null)", NULL);
    view_close(&v);
    /* long values: written whole, and a row whose snapshot is long still matches its WHERE */
    {
        run(db1, "CREATE TABLE longv (id INTEGER PRIMARY KEY, t TEXT)");
        run(db1, "INSERT INTO longv VALUES (1, 'short')");
        static char big[5001], big2[5001];
        for (int i = 0; i < 5000; i++) { big[i] = i % 97 == 0 ? '\'' : (char)('a' + i % 26); big2[i] = (char)('A' + i % 26); }
        big[5000] = big2[5000] = 0;
        view_open(&v, db1, "longv", "\"id\"");
        edit_put_text(&v.e, 0, col(&v, "t"), big);
        edit_apply(&v.e, db1, NULL, &ar);
        check(ar.status == APPLY_OK && scalar_is(db1, "SELECT length(t) FROM longv WHERE id = 1", "5000"), "a 5,000-byte value with quotes is written whole (no truncation)", ar.message);
        view_close(&v);
        view_open(&v, db1, "longv", "\"id\"");
        edit_put_text(&v.e, 0, col(&v, "t"), big2);
        edit_apply(&v.e, db1, NULL, &ar);
        check(ar.status == APPLY_OK && scalar_is(db1, "SELECT substr(t, 1, 3) || length(t) FROM longv WHERE id = 1", "ABC5000"), "editing a row whose stored value is 5,000 bytes matches its WHERE (no false conflict)", ar.message);
        view_close(&v);
    }
    /* a real BLOB column: kind from the declared type, hex text, edit refused with the reason */
    view_open(&v, db1, "tricky", "\"id\"");
    int bc = col(&v, "b");
    check(v.rs.kinds[bc] == CK_BINARY && rs_cell(&v.rs, 0, bc) && strcmp(rs_cell(&v.rs, 0, bc), "\\x00ff10") == 0, "BLOB column: binary kind, bytes shown as \\x hex", rs_cell(&v.rs, 0, bc));
    const char *bro = edit_col_readonly(&v.e, bc);
    check(bro && strstr(bro, "binary") && !edit_validate(&v.e, bc, "\\x00", why, sizeof why), "BLOB column is read-only with the reason", bro);
    /* SQL INSERT export of the same rows rebuilds them exactly, BLOB included */
    grid_select_all(&v.g);
    char *ins2 = grid_copy(&v.g, FMT_SQL, true, "tricky_copy");
    run(db1, "CREATE TABLE tricky_copy (id INTEGER PRIMARY KEY, t TEXT, b BLOB)");
    bool ins_ok = true;
    for (char *stmt = ins2, *nl; stmt && *stmt; stmt = nl ? nl + 1 : NULL) {
        nl = strstr(stmt, ");\n"); if (nl) { nl[2] = 0; nl += 2; }
        if (*stmt && !run(db1, stmt)) ins_ok = false;
    }
    check(ins_ok && scalar_is(db1, "SELECT COUNT(*) FROM tricky_copy", "3") && scalar_is(db1, "SELECT COUNT(*) FROM (SELECT id, t, b FROM tricky EXCEPT SELECT id, t, b FROM tricky_copy)", "0"), "SQL INSERT export with quotes / newline / NULL / '' / BLOB re-executed yields identical rows", ins2);
    check(scalar_is(db1, "SELECT typeof(b) FROM tricky_copy WHERE id = 1", "blob"), "BLOB exported as a blob literal, not as hex text", NULL);
    free(ins2);
    view_close(&v);
    /* column names that are not plain identifiers are quoted in the INSERT */
    run(db1, "CREATE TABLE odd (id INTEGER PRIMARY KEY, \"order\" TEXT, \"valor total\" REAL, \"x\"\"y\" TEXT)");
    run(db1, "INSERT INTO odd VALUES (1, 'a', 2.5, 'q')");
    view_open(&v, db1, "odd", "\"id\"");
    grid_select_all(&v.g);
    char *ins3 = grid_copy(&v.g, FMT_SQL, true, "odd_copy");
    run(db1, "CREATE TABLE odd_copy AS SELECT * FROM odd WHERE 0");
    char *nl3 = strchr(ins3, '\n'); if (nl3) *nl3 = 0;
    check(run(db1, ins3) && scalar_is(db1, "SELECT \"order\" || '|' || \"valor total\" || '|' || \"x\"\"y\" FROM odd_copy", "a|2.5|q") && strstr(ins3, "id, \"order\", \"valor total\", \"x\"\"y\""), "reserved word, space and embedded quote in column names are quoted", ins3);
    free(ins3);
    view_close(&v);

    /* 9. no primary key: editing refused by the caller; the strategy says so */
    run(db1, "CREATE TABLE nokey (x INTEGER)");
    view_open(&v, db1, "nokey", NULL);
    edit_describe_strategy(&v.e, d, sizeof d);
    check(v.e.npk == 0 && strstr(d, "no primary key"), "table without a key: no identification, editing off", d);
    view_close(&v);

    db_close(db2); db_close(db1);
    remove(path);
    printf("%s (%d failures)\n", failures ? "EDIT (SQLite): FAIL" : "EDIT (SQLite): OK", failures);
    return failures ? 1 : 0;
}
