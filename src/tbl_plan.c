/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Visual Table Designer: DDL planner per dialect (TBL-052..060, 074..081). See tbl.h.
 *
 * Order of an ALTER plan (TBL-055): drop keys that go away or change (foreign keys first), drop
 * columns, rename columns, alter columns, add columns, add keys (primary / unique / check /
 * foreign / indexes), rename keys, table options and comments, rename the table last (every
 * earlier statement names the table it still has).
 *
 * Nothing is approximated: an operation the engine cannot do, or that depends on a capability
 * Lume cannot confirm, blocks the plan with its reason (TBL-080). SQLite changes that ALTER cannot
 * make are planned as the documented rebuild (new table, copy, drop, rename, indexes, triggers,
 * foreign_key_check), refused when the table has attributes Lume does not model (TBL-081).
 */
#include "tbl.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>


static void step(TdPlan *p, int flags, int change, const char *why, const char *fmt, ...) {
    if (p->n >= TD_MAXSTEPS) { p->blocked = true; snprintf(p->block_reason, sizeof p->block_reason, "Too many statements for one plan (more than %d)", TD_MAXSTEPS); return; }
    char *b = malloc(16384);
    va_list ap; va_start(ap, fmt); vsnprintf(b, 16384, fmt, ap); va_end(ap);
    TdStep *s = &p->steps[p->n++];
    s->sql = b; s->flags = flags; s->change = change;
    snprintf(s->why, sizeof s->why, "%s", why ? why : "");
    p->flags |= flags;
}
static void warn(TdPlan *p, const char *fmt, ...) {
    if (p->nwarn >= 8) return;
    for (int i = 0; i < p->nwarn; i++) { char t[300]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap); if (!strcmp(t, p->warnings[i])) return; }
    va_list ap; va_start(ap, fmt); vsnprintf(p->warnings[p->nwarn++], sizeof p->warnings[0], fmt, ap); va_end(ap);
}
static void block(TdPlan *p, const char *fmt, ...) {
    if (p->blocked) return;
    p->blocked = true;
    va_list ap; va_start(ap, fmt); vsnprintf(p->block_reason, sizeof p->block_reason, fmt, ap); va_end(ap);
}
void td_plan_free(TdPlan *p) { for (int i = 0; i < p->n; i++) free(p->steps[i].sql); memset(p, 0, sizeof *p); }

static bool is_mysql(int d) { return d == TD_MYSQL || d == TD_MARIADB; }

/* ---- pieces ---- */

static void q(int d, const char *s, char *o, int n) { td_quote(d, s, o, n); }
static void qt(int d, const TdTable *t, char *o, int n) { td_qname(d, d == TD_SQLITE && !t->schema[0] ? "main" : t->schema, t->name, o, n); }
static void lit(int d, const char *s, char *o, int n) { td_lit(d, s, o, n); }

/* "a", "b" DESC */
static void col_list(int d, const TdTable *t, const TdKey *k, bool with_desc, char *o, int n) {
    int w = 0; o[0] = 0;
    for (int i = 0; i < k->ncols && w < n - 1; i++) {
        const TdCol *c = td_col_c(t, k->cols[i]);
        char qc[300]; q(d, c ? c->name : "?", qc, sizeof qc);
        w += snprintf(o + w, (size_t)(n - w), "%s%s%s", i ? ", " : "", qc, with_desc && k->desc[i] ? " DESC" : "");
    }
}
static void ref_list(int d, const TdKey *k, char *o, int n) {
    int w = 0; o[0] = 0;
    for (int i = 0; i < k->nref && w < n - 1; i++) { char qc[300]; q(d, k->ref_cols[i], qc, sizeof qc); w += snprintf(o + w, (size_t)(n - w), "%s%s", i ? ", " : "", qc); }
}

/* "name type [identity] [DEFAULT x] [NOT NULL] [GENERATED ...] [COLLATE] [COMMENT]" */
static void col_def(int d, const TdTable *t, const TdCol *c, bool sqlite_inline_pk, char *o, int n) {
    char qc[300]; q(d, c->name, qc, sizeof qc);
    int w = snprintf(o, (size_t)n, "%s", qc);
#define ADD(...) do { if (w < n - 1) w += snprintf(o + w, (size_t)(n - w), __VA_ARGS__); } while (0)
    if (d == TD_MSSQL && c->gen[0]) { ADD(" AS (%s)%s", c->gen, c->gen_stored ? " PERSISTED" : ""); return; }
    ADD(" %s", c->type);
    if (c->collation[0]) {
        if (d == TD_PG) { char qq[100]; q(d, c->collation, qq, sizeof qq); ADD(" COLLATE %s", qq); }
        else ADD(" COLLATE %s", c->collation);
    }
    if (c->identity) {
        if (d == TD_PG || d == TD_ORACLE) ADD(" GENERATED %s AS IDENTITY", c->identity == 1 ? "ALWAYS" : "BY DEFAULT");
        else if (d == TD_MSSQL) ADD(" IDENTITY(1,1)");
    }
    if (c->def[0] && !c->gen[0]) {
        /* MySQL needs parentheses around an expression default (8.0.13+); a literal stays as is */
        bool expr = is_mysql(d) && strchr(c->def, '(') && c->def[0] != '(' && c->def[0] != '\'';
        ADD(" DEFAULT %s%s%s", expr ? "(" : "", c->def, expr ? ")" : "");
    }
    if (c->gen[0]) {
        if (d == TD_ORACLE) ADD(" GENERATED ALWAYS AS (%s) VIRTUAL", c->gen);
        else ADD(" GENERATED ALWAYS AS (%s) %s", c->gen, c->gen_stored ? "STORED" : "VIRTUAL");
    }
    if (c->notnull) ADD(" NOT NULL");
    if (sqlite_inline_pk) ADD(" PRIMARY KEY%s", c->identity ? " AUTOINCREMENT" : "");
    if (is_mysql(d) && c->identity) ADD(" AUTO_INCREMENT");
    if (is_mysql(d) && c->comment[0]) { char l[1100]; lit(d, c->comment, l, sizeof l); ADD(" COMMENT %s", l); }
    if (c->extra[0] && is_mysql(d)) ADD(" %s", c->extra);   /* e.g. ON UPDATE CURRENT_TIMESTAMP: restated by MODIFY, never lost */
#undef ADD
}

/* "CONSTRAINT x PRIMARY KEY (...)" etc. */
static void key_def(int d, const TdTable *t, const TdKey *k, char *o, int n) {
    char nm[300] = "", cols[2000];
    if (k->name[0]) { char qq[300]; q(d, k->name, qq, sizeof qq); snprintf(nm, sizeof nm, "CONSTRAINT %s ", qq); }
    col_list(d, t, k, false, cols, sizeof cols);
    switch (k->kind) {
    case TK_PK: snprintf(o, (size_t)n, "%sPRIMARY KEY (%s)", nm, cols); break;
    case TK_UNIQUE: snprintf(o, (size_t)n, "%sUNIQUE (%s)", nm, cols); break;
    case TK_CHECK: snprintf(o, (size_t)n, "%sCHECK (%s)", nm, k->check); break;
    case TK_FK: {
        char rt[600], rc[2000];
        td_qname(d, k->ref_schema[0] ? k->ref_schema : (d == TD_SQLITE ? "" : t->schema), k->ref_table, rt, sizeof rt);
        if (d == TD_SQLITE) q(d, k->ref_table, rt, sizeof rt);   /* SQLite: the parent is in the same database */
        ref_list(d, k, rc, sizeof rc);
        int w = snprintf(o, (size_t)n, "%sFOREIGN KEY (%s) REFERENCES %s (%s)", nm, cols, rt, rc);
        if (k->on_delete[0] && strcmp(k->on_delete, "NO ACTION") && w < n) w += snprintf(o + w, (size_t)(n - w), " ON DELETE %s", k->on_delete);
        if (k->on_update[0] && strcmp(k->on_update, "NO ACTION") && w < n) w += snprintf(o + w, (size_t)(n - w), " ON UPDATE %s", k->on_update);
        if (k->deferrable && w < n) snprintf(o + w, (size_t)(n - w), " DEFERRABLE INITIALLY %s", k->deferred ? "DEFERRED" : "IMMEDIATE");
        break;
    }
    default: o[0] = 0;
    }
}
static void index_name(const TdTable *t, const TdKey *k, char *o, int n) {
    if (k->name[0]) snprintf(o, (size_t)n, "%s", k->name); else td_key_name(t, k, NULL, o, n);
}
static void create_index(int d, const TdTable *t, const TdKey *k, char *o, int n) {
    char nm[200], qn[300], tq[600], cols[2000];
    index_name(t, k, nm, sizeof nm);
    qt(d, t, tq, sizeof tq);
    col_list(d, t, k, true, cols, sizeof cols);
    if (d == TD_PG || d == TD_MYSQL || d == TD_MARIADB || d == TD_MSSQL) q(d, nm, qn, sizeof qn);
    else td_qname(d, d == TD_SQLITE && !t->schema[0] ? "main" : t->schema, nm, qn, sizeof qn);   /* Oracle / SQLite: schema on the index name */
    if (d == TD_SQLITE) q(d, t->name, tq, sizeof tq);
    int w = snprintf(o, (size_t)n, "CREATE %sINDEX %s ON %s", k->unique ? "UNIQUE " : "", qn, tq);
    if (k->method[0] && d == TD_PG) w += snprintf(o + w, (size_t)(n - w), " USING %s", k->method);
    w += snprintf(o + w, (size_t)(n - w), " (%s)", cols);
    if (k->ninclude) {
        w += snprintf(o + w, (size_t)(n - w), " INCLUDE (");
        for (int i = 0; i < k->ninclude; i++) { const TdCol *c = td_col_c(t, k->include[i]); char qc[300]; q(d, c ? c->name : "?", qc, sizeof qc); w += snprintf(o + w, (size_t)(n - w), "%s%s", i ? ", " : "", qc); }
        w += snprintf(o + w, (size_t)(n - w), ")");
    }
    if (k->where[0]) snprintf(o + w, (size_t)(n - w), " WHERE %s", k->where);
}
static void drop_index(int d, const TdTable *t, const TdKey *k, char *o, int n) {
    char qn[300], tq[600];
    qt(d, t, tq, sizeof tq);
    if (d == TD_MYSQL || d == TD_MARIADB || d == TD_MSSQL) { q(d, k->name, qn, sizeof qn); snprintf(o, (size_t)n, "DROP INDEX %s ON %s", qn, tq); }
    else { td_qname(d, d == TD_SQLITE && !t->schema[0] ? "main" : t->schema, k->name, qn, sizeof qn); snprintf(o, (size_t)n, "DROP INDEX %s", qn); }
}
/* SQL Server extended property for comments (MS_Description) */
static void ms_comment(TdPlan *p, const TdTable *t, const char *col, const char *old, const char *now) {
    char sch[300], tb[300], cl[300], v[1100];
    lit(TD_MSSQL, t->schema[0] ? t->schema : "dbo", sch, sizeof sch); lit(TD_MSSQL, t->name, tb, sizeof tb);
    if (col) lit(TD_MSSQL, col, cl, sizeof cl);
    lit(TD_MSSQL, now, v, sizeof v);
    const char *proc = !old[0] ? "sp_addextendedproperty" : !now[0] ? "sp_dropextendedproperty" : "sp_updateextendedproperty";
    char lvl2[400] = ""; if (col) snprintf(lvl2, sizeof lvl2, ", @level2type = N'COLUMN', @level2name = %s", cl);
    if (!now[0]) step(p, 0, 0, "comment", "EXEC sys.%s @name = N'MS_Description', @level0type = N'SCHEMA', @level0name = %s, @level1type = N'TABLE', @level1name = %s%s", proc, sch, tb, lvl2);
    else step(p, 0, 0, "comment", "EXEC sys.%s @name = N'MS_Description', @value = %s, @level0type = N'SCHEMA', @level0name = %s, @level1type = N'TABLE', @level1name = %s%s", proc, v, sch, tb, lvl2);
}
static void comment_stmt(int d, TdPlan *p, const TdTable *t, const TdCol *c, const char *old, const char *now) {
    char tq[600], v[1100];
    qt(d, t, tq, sizeof tq);
    if (d == TD_MSSQL) { ms_comment(p, t, c ? c->name : NULL, old, now); return; }
    if (d == TD_PG || d == TD_ORACLE) {
        if (now[0]) lit(d, now, v, sizeof v); else snprintf(v, sizeof v, d == TD_PG ? "NULL" : "''");
        if (c) { char qc[300]; q(d, c->name, qc, sizeof qc); step(p, 0, 0, "comment", "COMMENT ON COLUMN %s.%s IS %s", tq, qc, v); }
        else step(p, 0, 0, "comment", "COMMENT ON TABLE %s IS %s", tq, v);
        return;
    }
    if (is_mysql(d) && !c) { lit(d, now, v, sizeof v); step(p, TS_IMPLICIT_COMMIT, 0, "comment", "ALTER TABLE %s COMMENT = %s", tq, v); }
}

static int base_flags(int d) { return d == TD_ORACLE || is_mysql(d) ? TS_IMPLICIT_COMMIT : 0; }

/* ---- CREATE TABLE (TBL-001 / 011..046) ---- */

static void plan_create(int d, const TdTable *t, TdPlan *p) {
    char tq[600]; qt(d, t, tq, sizeof tq);
    size_t cap = 65536; char *b = malloc(cap); int w = 0;
    const char *kind = "TABLE";
    if (t->temporary) kind = d == TD_ORACLE ? "GLOBAL TEMPORARY TABLE" : d == TD_MSSQL ? NULL : "TEMPORARY TABLE";
    else if (t->unlogged && d == TD_PG) kind = "UNLOGGED TABLE";
    if (!kind) { block(p, "SQL Server temporary tables are named #name in tempdb: create it from a console"); free(b); return; }
    w += snprintf(b + w, cap - (size_t)w, "CREATE %s %s (", kind, tq);
    /* SQLite: an INTEGER identity is "INTEGER PRIMARY KEY AUTOINCREMENT" on the column itself */
    const TdKey *pk = NULL; for (int i = 0; i < t->nkeys; i++) if (t->keys[i].kind == TK_PK) pk = &t->keys[i];
    int inline_pk = -1;
    if (d == TD_SQLITE && pk && pk->ncols == 1) { const TdCol *c = td_col_c(t, pk->cols[0]); if (c && c->identity) inline_pk = c->id; }
    for (int i = 0; i < t->ncols; i++) {
        char def[2000]; col_def(d, t, &t->cols[i], t->cols[i].id == inline_pk, def, sizeof def);
        w += snprintf(b + w, cap - (size_t)w, "%s\n    %s", i ? "," : "", def);
        if (d == TD_SQLITE && t->cols[i].identity && t->cols[i].id != inline_pk) block(p, "SQLite: an auto-increment column must be the single INTEGER primary key");
    }
    for (int i = 0; i < t->nkeys; i++) {
        const TdKey *k = &t->keys[i];
        if ((k->kind != TK_PK && k->kind != TK_UNIQUE && k->kind != TK_FK && k->kind != TK_CHECK) || k->raw[0] || (k->kind == TK_PK && inline_pk >= 0)) continue;
        char kd[3000]; key_def(d, t, k, kd, sizeof kd);
        w += snprintf(b + w, cap - (size_t)w, ",\n    %s", kd);
    }
    w += snprintf(b + w, cap - (size_t)w, "\n)");
    if (t->temporary && d == TD_ORACLE) w += snprintf(b + w, cap - (size_t)w, " ON COMMIT PRESERVE ROWS");
    if (is_mysql(d)) {
        if (t->engine[0]) w += snprintf(b + w, cap - (size_t)w, " ENGINE = %s", t->engine);
        if (t->charset[0]) w += snprintf(b + w, cap - (size_t)w, " DEFAULT CHARSET = %s", t->charset);
        if (t->collation[0]) w += snprintf(b + w, cap - (size_t)w, " COLLATE = %s", t->collation);
        if (t->comment[0]) { char l[1100]; lit(d, t->comment, l, sizeof l); w += snprintf(b + w, cap - (size_t)w, " COMMENT = %s", l); }
    }
    step(p, base_flags(d), 0, "create the table", "%s", b);
    free(b);
    for (int i = 0; i < t->nkeys; i++) if (t->keys[i].kind == TK_INDEX && !t->keys[i].raw[0]) { char s[4000]; create_index(d, t, &t->keys[i], s, sizeof s); step(p, base_flags(d), t->keys[i].id, "create an index", "%s", s); }
    if (!is_mysql(d)) {
        if (t->comment[0]) comment_stmt(d, p, t, NULL, "", t->comment);
        for (int i = 0; i < t->ncols; i++) if (t->cols[i].comment[0]) comment_stmt(d, p, t, &t->cols[i], "", t->cols[i].comment);
    }
    if (d == TD_SQLITE && t->comment[0]) warn(p, "SQLite has no table comments: the comment is not saved");
}

/* ---- SQLite rebuild (TBL-074): the documented 12-step procedure ---- */

static bool col_same(const TdCol *a, const TdCol *b) { TdCol x = *b; x.id = a->id; snprintf(x.extra, sizeof x.extra, "%s", a->extra); return td_col_same(a, &x); }
static bool key_same(const TdKey *a, const TdKey *b, bool with_name) { return td_key_same(a, b, with_name); }
static const TdKey *key_in(const TdTable *t, int id) { for (int i = 0; i < t->nkeys; i++) if (t->keys[i].id == id) return &t->keys[i]; return NULL; }

static void plan_sqlite_rebuild(const TdTable *o, const TdTable *t, const TdPlanOpts *op, TdPlan *p) {
    for (int i = 0; i < o->ncols; i++) if (o->cols[i].extra[0]) { block(p, "Rebuilding %s would drop attributes Lume does not model (%s on %s): change it in a console", o->name, o->cols[i].extra, o->cols[i].name); return; }
    for (int i = 0; i < o->nkeys; i++) if (o->keys[i].raw[0] && key_in(t, o->keys[i].id)) { block(p, "Rebuilding %s would have to rewrite %s, which Lume does not model: change it in a console", o->name, o->keys[i].name); return; }
    if (o->extra[0]) { block(p, "Rebuilding %s would drop table options Lume does not model (%s)", o->name, o->extra); return; }
    p->rebuild = true;
    p->fk_toggle = op && op->sqlite_fk_on;
    TdTable n; td_copy(&n, t);
    char tmpname[160]; snprintf(tmpname, sizeof tmpname, "_lume_new_%s", t->name);
    snprintf(n.name, sizeof n.name, "%s", tmpname);
    for (int i = 0; i < n.nkeys; i++) if (n.keys[i].kind == TK_INDEX) n.keys[i].kind = -1;   /* indexes come after the rename */
    TdPlan cp; memset(&cp, 0, sizeof cp);
    plan_create(TD_SQLITE, &n, &cp);
    td_free(&n);
    if (cp.blocked) { block(p, "%s", cp.block_reason); td_plan_free(&cp); return; }
    const int RB = TS_REBUILD | TS_REWRITE | TS_BLOCKING;
    step(p, RB, 0, "rebuild: the new definition under a temporary name", "%s", cp.steps[0].sql);
    td_plan_free(&cp);
    /* copy the columns that survive (by id), renamed columns from their old name */
    char dst[8000] = "", src[8000] = ""; int wd = 0, ws = 0;
    for (int i = 0; i < t->ncols; i++) {
        const TdCol *c = &t->cols[i], *oc = td_col_c(o, c->id);
        if (!oc || c->gen[0]) continue;
        char a[300], b[300]; td_quote(TD_SQLITE, c->name, a, sizeof a); td_quote(TD_SQLITE, oc->name, b, sizeof b);
        wd += snprintf(dst + wd, sizeof dst - (size_t)wd, "%s%s", wd ? ", " : "", a);
        ws += snprintf(src + ws, sizeof src - (size_t)ws, "%s%s", ws ? ", " : "", b);
    }
    char nq[300], oq[300], sc[200]; td_quote(TD_SQLITE, tmpname, nq, sizeof nq); td_quote(TD_SQLITE, o->name, oq, sizeof oq);
    td_quote(TD_SQLITE, o->schema[0] ? o->schema : "main", sc, sizeof sc);
    if (wd) step(p, RB, 0, "rebuild: copy the rows (only the columns kept)", "INSERT INTO %s.%s (%s) SELECT %s FROM %s.%s", sc, nq, dst, src, sc, oq);
    int dropped = 0; for (int i = 0; i < o->ncols; i++) dropped += !td_col_c(t, o->cols[i].id);
    step(p, RB | (dropped ? TS_DESTRUCTIVE : 0), 0, dropped ? "rebuild: drop the old table (its dropped columns are lost)" : "rebuild: drop the old table", "DROP TABLE %s.%s", sc, oq);
    char fq[300]; td_quote(TD_SQLITE, t->name, fq, sizeof fq);
    step(p, RB, 0, "rebuild: give the new table its name", "ALTER TABLE %s.%s RENAME TO %s", sc, nq, fq);
    TdTable fin; td_copy(&fin, t);
    for (int i = 0; i < fin.nkeys; i++) if (fin.keys[i].kind == TK_INDEX && !fin.keys[i].raw[0]) { char s[4000]; create_index(TD_SQLITE, &fin, &fin.keys[i], s, sizeof s); step(p, RB, fin.keys[i].id, "rebuild: recreate the index", "%s", s); }
    for (int i = 0; i < o->nkeys; i++) if (o->keys[i].raw[0] && o->keys[i].kind == TK_INDEX) { const TdKey *k = key_in(t, o->keys[i].id); if (k) step(p, RB, k->id, "rebuild: recreate the index as it was", "%s", k->raw); }
    td_free(&fin);
    /* triggers that the old table carried: their SQL from the snapshot, unchanged */
    if (o->triggers[0]) {
        const char *s = o->triggers;
        while (*s) {
            const char *e = strstr(s, "\n;;\n");
            int len = e ? (int)(e - s) : (int)strlen(s);
            if (len > 0) { char *one = malloc((size_t)len + 1); memcpy(one, s, (size_t)len); one[len] = 0; step(p, RB, 0, "rebuild: recreate a trigger of the table", "%s", one); free(one); }
            s += len + (e ? 4 : 0);
        }
    }
    step(p, TS_DATA_CHECK, 0, "rebuild: every foreign key must still hold (no row may be returned)", "PRAGMA %s.foreign_key_check(%s)", sc, fq);
    warn(p, "SQLite cannot make this change with ALTER TABLE: the table is rebuilt (copied into a new table) inside one transaction");
    if (dropped) warn(p, "Data of %d dropped column(s) is lost when the old table is dropped", dropped);
}

/* ---- ALTER (TBL-002 / 011..041) ---- */

/* same family: no USING needed on PostgreSQL */
static int family(const char *t) {
    char b[64]; int i = 0; for (; t[i] && t[i] != '(' && i < 63; i++) b[i] = (char)tolower((unsigned char)t[i]);
    b[i] = 0; while (i > 0 && b[i - 1] == ' ') b[--i] = 0;
    if (strstr(b, "char") || strstr(b, "text") || !strcmp(b, "name") || !strcmp(b, "citext")) return 1;
    if (strstr(b, "int") || !strcmp(b, "numeric") || !strcmp(b, "decimal") || !strcmp(b, "real") || strstr(b, "double") || strstr(b, "float") || !strcmp(b, "number") || !strcmp(b, "money")) return 2;
    if (strstr(b, "time") || !strcmp(b, "date")) return 3;
    if (!strcmp(b, "boolean") || !strcmp(b, "bool") || !strcmp(b, "bit")) return 4;
    return 10 + (int)(b[0]);
}
static const char *using_for(const TdPlanOpts *op, int id) {
    for (int i = 0; op && i < op->nusing; i++) if (op->using_col[i] == id) return op->using_expr[i];
    return NULL;
}

/* does SQLite need a rebuild for this change set? */
static const char *sqlite_needs_rebuild(const TdTable *o, const TdTable *t, TdCaps caps) {
    for (int i = 0; i < o->ncols; i++) {
        const TdCol *a = &o->cols[i], *b = td_col_c(t, a->id);
        if (!b) {
            if (caps.drop_column != TCAP_YES) return "dropping a column (ALTER TABLE DROP COLUMN needs SQLite 3.35)";
            if (td_col_in_kind(o, a->id, TK_PK) || td_col_in_kind(o, a->id, TK_UNIQUE) || td_col_in_kind(o, a->id, TK_INDEX) || td_col_in_kind(o, a->id, TK_FK)) return "dropping a column used by a key or index";
            continue;
        }
        TdCol x = *b; snprintf(x.name, sizeof x.name, "%s", a->name);
        if (!col_same(a, &x)) return "changing a column's type, nullability, default, collation or expression";
        if (strcmp(a->name, b->name) && caps.rename_column != TCAP_YES) return "renaming a column (needs SQLite 3.25)";
    }
    for (int i = 0; i < t->ncols; i++) {
        const TdCol *c = &t->cols[i];
        if (td_col_c(o, c->id)) continue;
        if (td_col_in_kind(t, c->id, TK_PK) || td_col_in_kind(t, c->id, TK_UNIQUE)) return "adding a column that is part of a primary / unique key";
        if (c->notnull && !c->def[0]) return "adding a NOT NULL column without a default";
        if (c->def[0] && (strchr(c->def, '(') || !strncasecmp(c->def, "CURRENT_", 8))) return "adding a column whose default is not a constant";
        if (c->gen[0] && c->gen_stored) return "adding a STORED generated column";
        if (c->identity) return "adding an auto-increment column";
    }
    /* order of existing columns */
    int oi = 0;
    for (int i = 0; i < t->ncols; i++) { if (!td_col_c(o, t->cols[i].id)) continue; while (oi < o->ncols && !td_col_c(t, o->cols[oi].id)) oi++; if (oi < o->ncols && o->cols[oi].id != t->cols[i].id) return "reordering existing columns"; oi++; }
    for (int i = 0; i < o->nkeys; i++) {
        const TdKey *a = &o->keys[i], *b = key_in(t, a->id);
        if (a->kind == TK_INDEX || a->backs_constraint) continue;
        if (!b || !key_same(a, b, true)) return "changing a primary key, unique, foreign key or check (SQLite has no ALTER for constraints)";
    }
    for (int i = 0; i < t->nkeys; i++) if (t->keys[i].kind != TK_INDEX && !key_in(o, t->keys[i].id)) return "adding a primary key, unique, foreign key or check";
    return NULL;
}

static void plan_alter(int d, const TdTable *o, const TdTable *t, const TdPlanOpts *op, TdPlan *p) {
    TdCaps caps = td_caps(d, op ? op->version : 0);
    char tq[600]; qt(d, o, tq, sizeof tq);
    int BF = base_flags(d);
    int ALTER_LOCK = d == TD_PG || d == TD_MSSQL || is_mysql(d) || d == TD_ORACLE ? TS_BLOCKING : 0;
    if (o->special[0]) { block(p, "This table is read-only in the designer: %s", o->special); return; }
    if (!o->can_alter) { block(p, "%s", o->alter_why[0] ? o->alter_why : "The session may not alter this table"); return; }
    if (d == TD_SQLITE) {
        const char *why = sqlite_needs_rebuild(o, t, caps);
        if (why) { plan_sqlite_rebuild(o, t, op, p); if (!p->blocked) warn(p, "Rebuild needed: %s", why); goto table_level; }
    }
    /* 1. keys that go away or change: foreign keys first, then the others, then indexes */
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < o->nkeys; i++) {
            const TdKey *a = &o->keys[i], *b = key_in(t, a->id);
            if (a->backs_constraint) continue;
            int want = a->kind == TK_FK ? 0 : a->kind == TK_INDEX ? 2 : 1;
            if (want != pass) continue;
            bool gone = !b, changed = b && !key_same(a, b, false);
            bool renamed = b && !changed && strcmp(a->name, b->name);
            /* a rename the engine cannot do natively becomes drop + add (said so: TBL-036) */
            bool can_rename = a->kind == TK_INDEX ? caps.rename_index == TCAP_YES : caps.rename_constraint == TCAP_YES || (is_mysql(d) && a->kind == TK_UNIQUE && caps.rename_index == TCAP_YES);
            if (renamed && !can_rename) changed = true;
            if (!gone && !changed) continue;
            if (b && b->raw[0] && changed) { block(p, "%s is not modelled by Lume (%s): it can be dropped, not changed", a->name, a->raw); return; }
            char qn[300]; q(d, a->name, qn, sizeof qn);
            const char *why = gone ? "drop (removed in the draft)" : renamed ? "drop and recreate: this engine cannot rename it" : "drop and recreate (definition changed)";
            if (a->kind == TK_INDEX) { char s[1000]; drop_index(d, o, a, s, sizeof s); step(p, BF | ALTER_LOCK, a->id, why, "%s", s); }
            else if (is_mysql(d)) {
                if (a->kind == TK_PK) step(p, BF | ALTER_LOCK, a->id, why, "ALTER TABLE %s DROP PRIMARY KEY", tq);
                else if (a->kind == TK_UNIQUE) step(p, BF | ALTER_LOCK, a->id, why, "ALTER TABLE %s DROP INDEX %s", tq, qn);
                else if (a->kind == TK_FK) step(p, BF | ALTER_LOCK, a->id, why, "ALTER TABLE %s DROP FOREIGN KEY %s", tq, qn);
                else step(p, BF | ALTER_LOCK, a->id, why, d == TD_MARIADB ? "ALTER TABLE %s DROP CONSTRAINT %s" : "ALTER TABLE %s DROP CHECK %s", tq, qn);
            } else step(p, BF | ALTER_LOCK, a->id, why, "ALTER TABLE %s DROP CONSTRAINT %s", tq, qn);
        }
    /* 2. columns dropped */
    for (int i = 0; i < o->ncols; i++) {
        const TdCol *a = &o->cols[i];
        if (td_col_c(t, a->id)) continue;
        if (caps.drop_column == TCAP_NO) { block(p, "%s cannot drop columns", td_dialect_name(d)); return; }
        char qc[300]; q(d, a->name, qc, sizeof qc);
        if (d == TD_MSSQL && a->def_name[0]) { char dn[300]; q(d, a->def_name, dn, sizeof dn); step(p, BF | ALTER_LOCK, a->id, "drop the column's DEFAULT constraint first", "ALTER TABLE %s DROP CONSTRAINT %s", tq, dn); }
        step(p, BF | ALTER_LOCK | TS_DESTRUCTIVE | (d == TD_PG ? 0 : TS_REWRITE | TS_UNCERTAIN), a->id, "drop the column: its data is lost", "ALTER TABLE %s DROP COLUMN %s", tq, qc);
    }
    /* 3. renames */
    for (int i = 0; i < t->ncols; i++) {
        const TdCol *b = &t->cols[i], *a = td_col_c(o, b->id);
        if (!a || !strcmp(a->name, b->name)) continue;
        if (caps.rename_column != TCAP_YES) { block(p, "Renaming a column is %s on this %s version: it would need DROP + ADD, which loses data", caps.rename_column == TCAP_NO ? "not supported" : "not confirmed", td_dialect_name(d)); return; }
        char qa[300], qb[300]; q(d, a->name, qa, sizeof qa); q(d, b->name, qb, sizeof qb);
        if (d == TD_MSSQL) { char l[700], nn[300], full[600]; snprintf(full, sizeof full, "%s.%s.%s", o->schema[0] ? o->schema : "dbo", o->name, a->name); lit(d, full, l, sizeof l); lit(d, b->name, nn, sizeof nn); step(p, BF | ALTER_LOCK, b->id, "rename (keeps the data)", "EXEC sp_rename %s, %s, N'COLUMN'", l, nn); }
        else step(p, BF | ALTER_LOCK, b->id, "rename (keeps the data)", "ALTER TABLE %s RENAME COLUMN %s TO %s", tq, qa, qb);
    }
    /* 4. column changes */
    for (int i = 0; i < t->ncols; i++) {
        const TdCol *b = &t->cols[i], *a = td_col_c(o, b->id);
        if (!a) continue;
        bool ty = strcasecmp(a->type, b->type) != 0, nn = a->notnull != b->notnull, df = strcmp(a->def, b->def) != 0, ge = strcmp(a->gen, b->gen) || a->gen_stored != b->gen_stored,
             id = a->identity != b->identity, co = strcmp(a->collation, b->collation) != 0, cm = strcmp(a->comment, b->comment) != 0;
        if (!(ty || nn || df || ge || id || co || cm)) continue;
        char qc[300]; q(d, b->name, qc, sizeof qc);
        char risk[200] = ""; if (ty) td_type_risk(a->type, b->type, risk, sizeof risk);
        int tyf = ty ? TS_REWRITE | TS_BLOCKING | (risk[0] ? TS_DESTRUCTIVE : 0) : 0;
        if (ge) { if (is_mysql(d)) goto mysql_modify; block(p, "Changing the expression of generated column %s is not supported here: drop the column and add it again", b->name); return; }
        if (id && d == TD_MSSQL) { block(p, "SQL Server cannot add or remove IDENTITY on an existing column (%s): it needs a new column and a data copy", b->name); return; }
        if (id && d == TD_ORACLE) { block(p, "Oracle cannot turn an existing column (%s) into an identity or back with this designer", b->name); return; }
        if (is_mysql(d)) {
        mysql_modify:;
            /* MySQL restates the whole column: every attribute is repeated, the unmodelled ones too */
            char def[3000]; col_def(d, t, b, false, def, sizeof def);
            step(p, BF | ALTER_LOCK | tyf | (nn && b->notnull ? TS_DATA_CHECK : 0) | (ty || co ? TS_UNCERTAIN : 0), b->id, ty ? (risk[0] ? risk : "change the type (MySQL may copy the table)") : "restate the column", "ALTER TABLE %s MODIFY COLUMN %s", tq, def);
            continue;
        }
        if (d == TD_PG) {
            if (ty || co) {
                const char *u = using_for(op, b->id);
                char coll[200] = ""; if (b->collation[0]) { char qq[100]; q(d, b->collation, qq, sizeof qq); snprintf(coll, sizeof coll, " COLLATE %s", qq); }
                if (ty && family(a->type) != family(b->type) && !u) { block(p, "%s: %s -> %s has no implicit conversion. Give a USING expression (for example %s::%s) in Review", b->name, a->type, b->type, qc, b->type); return; }
                /* a default is cast with the column: PostgreSQL refuses one that does not convert, so it goes first and comes back after */
                if (ty && a->def[0] && family(a->type) != family(b->type)) { step(p, BF | TS_BLOCKING, b->id, "drop the default first: the old one would not convert", "ALTER TABLE %s ALTER COLUMN %s DROP DEFAULT", tq, qc); df = b->def[0] != 0; }
                if (u && u[0]) step(p, BF | TS_BLOCKING | TS_REWRITE | (risk[0] ? TS_DESTRUCTIVE : 0), b->id, risk[0] ? risk : "change the type (rewrites the table)", "ALTER TABLE %s ALTER COLUMN %s TYPE %s%s USING %s", tq, qc, b->type, coll, u);
                else step(p, BF | TS_BLOCKING | TS_REWRITE | TS_UNCERTAIN | (risk[0] ? TS_DESTRUCTIVE : 0), b->id, risk[0] ? risk : "change the type (may rewrite the table and its indexes)", "ALTER TABLE %s ALTER COLUMN %s TYPE %s%s", tq, qc, b->type, coll);
            }
            if (df) { if (b->def[0]) step(p, BF | TS_BLOCKING, b->id, "new default (existing rows keep their values)", "ALTER TABLE %s ALTER COLUMN %s SET DEFAULT %s", tq, qc, b->def); else step(p, BF | TS_BLOCKING, b->id, "remove the default", "ALTER TABLE %s ALTER COLUMN %s DROP DEFAULT", tq, qc); }
            if (nn) { if (b->notnull) step(p, BF | TS_BLOCKING | TS_DATA_CHECK, b->id, "NOT NULL: every row is checked; fails if one is NULL", "ALTER TABLE %s ALTER COLUMN %s SET NOT NULL", tq, qc); else step(p, BF | TS_BLOCKING, b->id, "allow NULL", "ALTER TABLE %s ALTER COLUMN %s DROP NOT NULL", tq, qc); }
            if (id) { if (b->identity) step(p, BF | TS_BLOCKING, b->id, "identity (not a sequence default)", "ALTER TABLE %s ALTER COLUMN %s ADD GENERATED %s AS IDENTITY", tq, qc, b->identity == 1 ? "ALWAYS" : "BY DEFAULT"); else step(p, BF | TS_BLOCKING, b->id, "stop generating values", "ALTER TABLE %s ALTER COLUMN %s DROP IDENTITY", tq, qc); }
            if (cm) comment_stmt(d, p, o, b, a->comment, b->comment);
            continue;
        }
        if (d == TD_ORACLE) {
            char parts[2000] = ""; int w = 0;
            if (ty) w += snprintf(parts + w, sizeof parts - (size_t)w, " %s", b->type);
            if (co) w += snprintf(parts + w, sizeof parts - (size_t)w, " COLLATE %s", b->collation[0] ? b->collation : "USING_NLS_COMP");
            if (df) w += snprintf(parts + w, sizeof parts - (size_t)w, " DEFAULT %s", b->def[0] ? b->def : "NULL");
            if (nn) w += snprintf(parts + w, sizeof parts - (size_t)w, b->notnull ? " NOT NULL" : " NULL");
            if (w) step(p, BF | TS_BLOCKING | tyf | (nn && b->notnull ? TS_DATA_CHECK : 0), b->id, ty ? (risk[0] ? risk : "change the type") : "modify the column", "ALTER TABLE %s MODIFY (%s%s)", tq, qc, parts);
            if (cm) comment_stmt(d, p, o, b, a->comment, b->comment);
            continue;
        }
        if (d == TD_MSSQL) {
            bool ddef = df || ty || co;   /* the DEFAULT constraint is in the way of ALTER COLUMN */
            if (ddef && a->def_name[0]) { char dn[300]; q(d, a->def_name, dn, sizeof dn); step(p, BF | TS_BLOCKING, b->id, "drop the DEFAULT constraint (recreated below)", "ALTER TABLE %s DROP CONSTRAINT %s", tq, dn); }
            if (ty || nn || co) {
                char coll[100] = ""; if (b->collation[0]) snprintf(coll, sizeof coll, " COLLATE %s", b->collation);
                step(p, BF | TS_BLOCKING | tyf | (nn && b->notnull ? TS_DATA_CHECK : 0) | TS_UNCERTAIN, b->id, ty ? (risk[0] ? risk : "change the type (Sch-M lock; indexes on the column make the server refuse)") : "change nullability (Sch-M lock)",
                     "ALTER TABLE %s ALTER COLUMN %s %s%s %s", tq, qc, b->type, coll, b->notnull ? "NOT NULL" : "NULL");
            }
            if ((ddef && a->def_name[0] && b->def[0]) || (df && b->def[0])) {
                char dn[300], nmb[200]; snprintf(nmb, sizeof nmb, "%s", a->def_name[0] ? a->def_name : ""); if (!nmb[0]) snprintf(nmb, sizeof nmb, "DF_%s_%s", o->name, b->name);
                q(d, nmb, dn, sizeof dn);
                step(p, BF | TS_BLOCKING, b->id, "the default", "ALTER TABLE %s ADD CONSTRAINT %s DEFAULT %s FOR %s", tq, dn, b->def, qc);
            }
            if (cm) comment_stmt(d, p, o, b, a->comment, b->comment);
            continue;
        }
        if (d == TD_SQLITE && cm) warn(p, "SQLite has no column comments: the comment of %s is not saved", b->name);
    }
    /* 5. new columns, in the order chosen */
    for (int i = 0; i < t->ncols; i++) {
        const TdCol *c = &t->cols[i];
        if (td_col_c(o, c->id)) continue;
        char def[3000]; col_def(d, t, c, false, def, sizeof def);
        int f = BF | ALTER_LOCK | (c->notnull && !c->def[0] && !c->identity && !c->gen[0] ? TS_DATA_CHECK : 0) | (c->gen_stored || (d != TD_PG && c->def[0]) ? TS_UNCERTAIN : 0);
        const char *why = !(c->notnull && !c->def[0] && !c->identity && !c->gen[0]) ? "add the column"
                        : is_mysql(d) ? "NOT NULL without a default: existing rows get the type's implicit default ('' / 0) silently"
                        : "NOT NULL without a default: fails if the table has rows";
        if (d == TD_ORACLE) step(p, f, c->id, why, "ALTER TABLE %s ADD (%s)", tq, def);
        else if (d == TD_MSSQL) step(p, f, c->id, why, "ALTER TABLE %s ADD %s", tq, def);
        else if (is_mysql(d) && i > 0 && td_col_c(o, t->cols[i - 1].id) && i - 1 < t->ncols - 1) {
            char after[300]; q(d, t->cols[i - 1].name, after, sizeof after);
            bool last = true; for (int j = i + 1; j < t->ncols; j++) if (td_col_c(o, t->cols[j].id)) last = false;
            if (last) step(p, f, c->id, why, "ALTER TABLE %s ADD COLUMN %s", tq, def);
            else step(p, f, c->id, why, "ALTER TABLE %s ADD COLUMN %s AFTER %s", tq, def, after);
        } else if (is_mysql(d) && i == 0) step(p, f, c->id, why, "ALTER TABLE %s ADD COLUMN %s FIRST", tq, def);
        else step(p, f, c->id, why, "ALTER TABLE %s ADD COLUMN %s", tq, def);
        if (!is_mysql(d) && c->comment[0]) comment_stmt(d, p, o, c, "", c->comment);
        if (d != TD_MYSQL && d != TD_MARIADB) {
            int pos = td_col_index(t, c->id); bool tail = true; for (int j = pos + 1; j < t->ncols; j++) if (td_col_c(o, t->cols[j].id)) tail = false;
            if (!tail) warn(p, "%s adds new columns at the end: %s goes after the existing columns", td_dialect_name(d), c->name);
        }
    }
    /* existing columns moved */
    { int oi = 0; bool moved = false;
      for (int i = 0; i < t->ncols; i++) { if (!td_col_c(o, t->cols[i].id)) continue; while (oi < o->ncols && !td_col_c(t, o->cols[oi].id)) oi++; if (oi < o->ncols && o->cols[oi].id != t->cols[i].id) moved = true; oi++; }
      if (moved) { block(p, "%s cannot reorder existing columns without rebuilding the table: move only new columns", td_dialect_name(d)); return; } }
    /* 6. keys added or recreated */
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < t->nkeys; i++) {
            const TdKey *b = &t->keys[i], *a = key_in(o, b->id);
            if (b->backs_constraint) continue;
            int want = b->kind == TK_INDEX ? 2 : b->kind == TK_FK ? 1 : 0;
            if (want != pass) continue;
            bool can_rename = b->kind == TK_INDEX ? caps.rename_index == TCAP_YES : caps.rename_constraint == TCAP_YES || (is_mysql(d) && b->kind == TK_UNIQUE && caps.rename_index == TCAP_YES);
            if (a && key_same(a, b, false) && (!strcmp(a->name, b->name) || can_rename)) continue;
            if (b->raw[0]) continue;
            int f = BF | (b->kind == TK_INDEX ? (d == TD_PG ? TS_BLOCKING : TS_BLOCKING | TS_UNCERTAIN) : ALTER_LOCK) | (b->kind == TK_INDEX ? 0 : TS_DATA_CHECK);
            if (b->kind == TK_INDEX) { char s[4000]; create_index(d, o, b, s, sizeof s); step(p, f | (b->unique ? TS_DATA_CHECK : 0), b->id, d == TD_PG ? "create the index (blocks writes while it builds)" : "create the index", "%s", s); }
            else {
                char kd[3000]; key_def(d, t, b, kd, sizeof kd);
                const char *why = b->kind == TK_FK ? "every existing row is checked against the referenced table" : b->kind == TK_CHECK ? "every existing row is checked" : "fails if existing rows have duplicates or NULLs";
                if (d == TD_ORACLE) step(p, f, b->id, why, "ALTER TABLE %s ADD %s", tq, kd);
                else step(p, f, b->id, why, "ALTER TABLE %s ADD %s", tq, kd);
            }
        }
    /* 7. renames of keys the engine can rename */
    for (int i = 0; i < t->nkeys; i++) {
        const TdKey *b = &t->keys[i], *a = key_in(o, b->id);
        if (!a || !key_same(a, b, false) || !strcmp(a->name, b->name) || b->backs_constraint) continue;
        bool can_rename = b->kind == TK_INDEX ? caps.rename_index == TCAP_YES : caps.rename_constraint == TCAP_YES || (is_mysql(d) && b->kind == TK_UNIQUE && caps.rename_index == TCAP_YES);
        if (!can_rename) continue;   /* recreated above */
        char qa[300], qb[300]; q(d, a->name, qa, sizeof qa); q(d, b->name, qb, sizeof qb);
        if (d == TD_MSSQL) {
            char l[700], nn[300], full[600];
            if (b->kind == TK_INDEX) snprintf(full, sizeof full, "%s.%s.%s", o->schema[0] ? o->schema : "dbo", o->name, a->name); else snprintf(full, sizeof full, "%s.%s", o->schema[0] ? o->schema : "dbo", a->name);
            lit(d, full, l, sizeof l); lit(d, b->name, nn, sizeof nn);
            step(p, BF, b->id, "rename (no rebuild)", "EXEC sp_rename %s, %s, N'%s'", l, nn, b->kind == TK_INDEX ? "INDEX" : "OBJECT");
        } else if (is_mysql(d)) step(p, BF | ALTER_LOCK, b->id, "rename (no rebuild)", "ALTER TABLE %s RENAME INDEX %s TO %s", tq, qa, qb);
        else if (b->kind == TK_INDEX) { char qi[600]; td_qname(d, o->schema, a->name, qi, sizeof qi); step(p, BF, b->id, "rename (no rebuild)", "ALTER INDEX %s RENAME TO %s", qi, qb); }
        else step(p, BF | ALTER_LOCK, b->id, "rename (no rebuild)", "ALTER TABLE %s RENAME CONSTRAINT %s TO %s", tq, qa, qb);
    }
table_level:
    if (p->blocked) return;
    /* 8. table options and comment */
    if (strcmp(o->comment, t->comment)) { if (caps.comments == TCAP_NO) warn(p, "%s has no table comments: the comment is not saved", td_dialect_name(d)); else comment_stmt(d, p, o, NULL, o->comment, t->comment); }
    if (is_mysql(d)) {
        if (strcmp(o->engine, t->engine) && t->engine[0]) step(p, BF | TS_BLOCKING | TS_REWRITE, 0, "another storage engine copies the whole table", "ALTER TABLE %s ENGINE = %s", tq, t->engine);
        if ((strcmp(o->charset, t->charset) && t->charset[0]) || (strcmp(o->collation, t->collation) && t->collation[0])) {
            step(p, BF, 0, "default for new columns only (existing columns keep theirs)", "ALTER TABLE %s DEFAULT CHARACTER SET = %s%s%s", tq, t->charset[0] ? t->charset : o->charset, t->collation[0] ? " COLLATE = " : "", t->collation);
            warn(p, "Changing the table's default charset / collation affects new columns only");
        }
    }
    if ((o->unlogged != t->unlogged) && d == TD_PG) step(p, BF | TS_BLOCKING | TS_REWRITE, 0, t->unlogged ? "UNLOGGED: data is lost after a crash and is not replicated" : "LOGGED: rewrites the table into the WAL", "ALTER TABLE %s SET %s", tq, t->unlogged ? "UNLOGGED" : "LOGGED");
    if (o->temporary != t->temporary) { block(p, "A table cannot become temporary (or stop being one) by ALTER"); return; }
    /* 9. the table's name, last */
    if (strcmp(o->name, t->name)) {
        char nn[300]; q(d, t->name, nn, sizeof nn);
        if (d == TD_MSSQL) { char l[700], nl[300], full[600]; snprintf(full, sizeof full, "%s.%s", o->schema[0] ? o->schema : "dbo", o->name); lit(d, full, l, sizeof l); lit(d, t->name, nl, sizeof nl); step(p, BF | TS_BLOCKING, 0, "rename the table (views and code that name it break)", "EXEC sp_rename %s, %s", l, nl); }
        else if (is_mysql(d)) { char nt[600]; td_qname(d, o->schema, t->name, nt, sizeof nt); step(p, BF | TS_BLOCKING, 0, "rename the table (views and code that name it break)", "RENAME TABLE %s TO %s", tq, nt); }
        else step(p, BF | TS_BLOCKING, 0, "rename the table (views and code that name it break)", "ALTER TABLE %s RENAME TO %s", tq, nn);
    }
}

/* ---- entry ---- */

void td_plan(int d, const TdTable *o, const TdTable *t, const TdPlanOpts *op, TdPlan *p) {
    memset(p, 0, sizeof *p);
    TdIssue is[64]; int ni = td_validate(d, op ? op->version : 0, o, t, is, 64);
    for (int i = 0; i < ni && i < 64; i++) if (is[i].level == TV_ERROR) { block(p, "%s", is[i].text); break; }
    if (p->blocked) return;
    TdCaps caps = td_caps(d, op ? op->version : 0);
    if (!o || !o->exists) plan_create(d, t, p);
    else plan_alter(d, o, t, op, p);
    if (p->blocked) { for (int i = 0; i < p->n; i++) free(p->steps[i].sql); p->n = 0; return; }
    /* transaction and the truth about rollback (TBL-060 / 076 / 099) */
    p->transactional = caps.transactional_ddl == TCAP_YES && p->n > 0;
    if (d == TD_PG && op && op->lock_timeout[0] && p->n) {
        /* SET LOCAL inside the transaction, first */
        memmove(p->steps + 1, p->steps, sizeof p->steps[0] * (size_t)(p->n < TD_MAXSTEPS ? p->n : TD_MAXSTEPS - 1));
        if (p->n < TD_MAXSTEPS) p->n++;
        char *b = malloc(80); char l[40]; td_lit(d, op->lock_timeout, l, sizeof l); snprintf(b, 80, "SET LOCAL lock_timeout = %s", l);
        p->steps[0].sql = b; p->steps[0].flags = 0; p->steps[0].change = 0; snprintf(p->steps[0].why, sizeof p->steps[0].why, "give up instead of waiting longer than %s for a lock", op->lock_timeout);
    }
    p->rollback = p->transactional ? TR_TRANSACTIONAL : (p->flags & TS_DESTRUCTIVE) ? TR_IRREVERSIBLE : TR_COMPENSATE;
    if (p->flags & TS_IMPLICIT_COMMIT) {
        warn(p, d == TD_ORACLE ? "Oracle commits before and after every DDL statement: if step N fails, steps 1..N-1 stay applied (there is no rollback)"
                               : "%s commits implicitly around every ALTER / CREATE: if a step fails, the earlier ones stay applied (there is no rollback)", td_dialect_name(d));
        if (is_mysql(d)) warn(p, "ALTER TABLE takes a metadata lock; whether it runs INSTANT, INPLACE or COPY depends on the server version and the operation (not guaranteed)");
    }
    if (d == TD_PG && (p->flags & TS_BLOCKING)) warn(p, "Most ALTER TABLE forms take an ACCESS EXCLUSIVE lock: reads and writes on the table wait until the transaction ends");
    if (d == TD_MSSQL && (p->flags & TS_BLOCKING)) warn(p, "ALTER TABLE takes a schema-modification (Sch-M) lock: every query on the table waits; duration cannot be guaranteed");
    if (p->flags & TS_REWRITE) warn(p, "At least one step may rewrite or copy the table: its duration grows with the table's size");
    if (p->flags & TS_DATA_CHECK) warn(p, "At least one step checks the existing rows and fails if one does not fit (NOT NULL, UNIQUE, CHECK or FOREIGN KEY)");
    if (p->flags & TS_UNCERTAIN) warn(p, "Some effects depend on the server version and the data: they are marked uncertain");
    p->risk = (p->flags & (TS_DESTRUCTIVE | TS_REBUILD)) || ((p->flags & TS_IMPLICIT_COMMIT) && p->n > 1) ? RISK_HIGH : (p->flags & (TS_BLOCKING | TS_REWRITE | TS_DATA_CHECK)) ? RISK_MEDIUM : RISK_LOW;
    /* a compensating script for non-transactional, non-destructive plans: a proposal, never run alone */
    if (p->rollback == TR_COMPENSATE && o && o->exists && !(op && op->no_compensate)) {
        TdPlan rev; TdPlanOpts ro; memset(&ro, 0, sizeof ro); if (op) ro.version = op->version;
        ro.no_compensate = true;
        TdTable back; td_copy(&back, o);
        td_plan(d, t, &back, &ro, &rev);
        if (!rev.blocked && rev.n) { char *s = td_plan_script(d, &rev); snprintf(p->compensate, sizeof p->compensate, "%s", s); free(s); }
        else p->rollback = TR_IRREVERSIBLE;
        td_plan_free(&rev); td_free(&back);
    }
    if (p->rollback == TR_COMPENSATE && (!o || !o->exists)) { char tq[600]; qt(d, t, tq, sizeof tq); snprintf(p->compensate, sizeof p->compensate, "DROP TABLE %s;\n", tq); }
}

char *td_plan_script(int d, const TdPlan *p) {
    size_t cap = 1024; for (int i = 0; i < p->n; i++) cap += strlen(p->steps[i].sql) + 8;
    char *s = malloc(cap); size_t w = 0; s[0] = 0;
    const char *begin = d == TD_MSSQL ? "BEGIN TRANSACTION" : "BEGIN";
    if (d == TD_SQLITE && p->fk_toggle) w += (size_t)snprintf(s + w, cap - w, "PRAGMA foreign_keys = OFF;\n");
    if (p->transactional) w += (size_t)snprintf(s + w, cap - w, "%s;\n", begin);
    for (int i = 0; i < p->n; i++) w += (size_t)snprintf(s + w, cap - w, "%s;\n", p->steps[i].sql);
    if (p->transactional) w += (size_t)snprintf(s + w, cap - w, "COMMIT;\n");
    if (d == TD_SQLITE && p->fk_toggle) w += (size_t)snprintf(s + w, cap - w, "PRAGMA foreign_keys = ON;\n");
    return s;
}
uint64_t td_plan_hash(const TdPlan *p, uint64_t rev) {
    uint64_t h = 1469598103934665603ULL ^ rev;
    for (int i = 0; i < p->n; i++) { for (const char *s = p->steps[i].sql; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } h ^= 0x1f; h *= 1099511628211ULL; }
    return h;
}
