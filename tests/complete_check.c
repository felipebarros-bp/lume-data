/* SPDX-License-Identifier: AGPL-3.0-only */
/* Autocompletion, headless: JOIN ... ON conditions from foreign keys (composite too, either
 * direction, with the statement's aliases) and from same-named key columns; generic columns and
 * each table's own "id" never suggested. */
#include "complete.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the real one lives in db.c (with every driver): the same, here */
void db_fks_free(DbFk *f, int n) { for (int i = 0; i < n; i++) { free(f[i].name); free(f[i].from_col); free(f[i].to_table); free(f[i].to_col); } free(f); }

static int fails, oks;
#define CHECK(c, ...) do { if (c) { oks++; printf("ok    "); } else { fails++; printf("FAIL  "); } printf(__VA_ARGS__); printf("\n"); } while (0)

static DbColumn C(const char *n, const char *t, bool pk) { DbColumn c; memset(&c, 0, sizeof c); c.name = strdup(n); c.type = strdup(t); c.pk = pk; return c; }
static DbTable T(const char *name, DbColumn *cols, int n) { DbTable t; memset(&t, 0, sizeof t); t.name = strdup(name); t.cols = malloc(sizeof *cols * (size_t)n); memcpy(t.cols, cols, sizeof *cols * (size_t)n); t.ncols = n; t.cols_loaded = true; return t; }
static int loads;
/* late_fks: keys of these catalog tables "arrive" later (0 = asked), like the catalog session */
static char late_fks[200] = ""; static DbFk *late_out; static int late_n;
static int fks(void *ctx, const char *table, DbFk **out, int *n) {
    (void)ctx; loads++;
    if (late_fks[0] && !strcmp(table, late_fks)) { if (!late_out) { *out = NULL; *n = 0; return 0; } *out = late_out; *n = late_n; late_out = NULL; return 1; }
    static const struct { const char *tb, *name, *from, *to, *tocol; } all[] = {
        { "orders", "fk_orders_customer", "customer_id", "customers", "id" },
        { "shipments", "fk_ship_item", "order_id", "order_items", "order_id" },
        { "shipments", "fk_ship_item", "item_line", "order_items", "line" },
        { "order_items", "fk_item_order", "order_id", "orders", "id" },
    };
    int k = 0; DbFk *f = calloc(4, sizeof *f);
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) if (!strcmp(all[i].tb, table)) { f[k].name = strdup(all[i].name); f[k].from_col = strdup(all[i].from); f[k].to_table = strdup(all[i].to); f[k].to_col = strdup(all[i].tocol); k++; }
    *out = f; *n = k;
    return 1;
}
static int query(Completer *c, const char *sql, Cand *out) { int pl; return complete_query(c, sql, (int)strlen(sql), (int)strlen(sql), false, out, COMPLETE_MAX, &pl); }
static bool has(Cand *o, int n, const char *text) { for (int i = 0; i < n; i++) if (!strcmp(o[i].text, text)) return true; return false; }

/* a fake catalog session: tables handed out by schema.name; columns "arrive" when the test says so */
typedef struct { char key[200]; DbTable t; int state; } FakeT;
static FakeT fake[16]; static int nfake, fake_asks;
static DbTable *fake_table(void *ctx, const char *schema, const char *name, int *state) {
    (void)ctx; char key[200]; snprintf(key, sizeof key, "%s.%s", schema, name);
    for (int i = 0; i < nfake; i++) if (!strcmp(fake[i].key, key)) { *state = fake[i].state; return &fake[i].t; }
    FakeT *f = &fake[nfake++]; memset(f, 0, sizeof *f); snprintf(f->key, sizeof f->key, "%s", key);
    f->t.name = strdup(key); f->state = 1 /* OCS_LOADING */; fake_asks++;
    *state = f->state; return &f->t;
}
static void fake_arrive(const char *key, DbColumn *cols, int n) {
    for (int i = 0; i < nfake; i++) if (!strcmp(fake[i].key, key)) {
        fake[i].t.cols = malloc(sizeof *cols * (size_t)n); memcpy(fake[i].t.cols, cols, sizeof *cols * (size_t)n);
        fake[i].t.ncols = n; fake[i].t.cols_loaded = true; fake[i].state = 2 /* OCS_LOADED */;
    }
}
static int query_x(Completer *c, const char *sql, Cand *out, bool explicit) { int pl; return complete_query(c, sql, (int)strlen(sql), (int)strlen(sql), explicit, out, COMPLETE_MAX, &pl); }
static const char *shown(const Cand *c) { return c->label[0] ? c->label : c->text; }

int main(void) {
    DbColumn cu[] = { C("id", "INTEGER", true), C("name", "TEXT", false), C("created_at", "TEXT", false) };
    DbColumn orr[] = { C("id", "INTEGER", true), C("customer_id", "INTEGER", false), C("created_at", "TEXT", false) };
    DbColumn oi[] = { C("order_id", "INTEGER", true), C("line", "INTEGER", true), C("qty", "INTEGER", false) };
    DbColumn sh[] = { C("id", "INTEGER", true), C("order_id", "INTEGER", false), C("item_line", "INTEGER", false) };
    DbColumn pr[] = { C("customer_id", "INTEGER", true), C("bio", "TEXT", false), C("created_at", "TEXT", false) };
    DbTable tb[] = { T("customers", cu, 3), T("orders", orr, 3), T("order_items", oi, 3), T("shipments", sh, 3), T("profiles", pr, 3) };
    Completer *c = complete_create();
    complete_set_schema(c, tb, 5, NULL, NULL, NULL);
    complete_set_fk_loader(c, fks);
    Cand o[COMPLETE_MAX]; int n;

    n = query(c, "SELECT * FROM orders o JOIN customers c ON ", o);
    CHECK(n >= 1 && !strcmp(o[0].text, "c.id = o.customer_id") && strstr(o[0].detail, "fk_orders_customer"), "JOIN customers c ON: the FK first, with the aliases (%s)", n ? o[0].text : "-");
    n = query(c, "select * from shipments s join order_items oi on ", o);
    CHECK(n >= 1 && !strcmp(o[0].text, "oi.order_id = s.order_id AND oi.line = s.item_line") && strstr(o[0].detail, "composite"), "composite FK: every pair, in key order (%s)", n ? o[0].text : "-");
    n = query(c, "SELECT * FROM customers JOIN orders ON ", o);
    CHECK(n >= 1 && !strcmp(o[0].text, "orders.customer_id = customers.id"), "no aliases: table names; the FK read in the other direction (%s)", n ? o[0].text : "-");
    n = query(c, "SELECT * FROM customers c JOIN profiles p ON ", o);
    CHECK(has(o, n, "p.customer_id = c.customer_id") == false && !has(o, n, "p.created_at = c.created_at"), "a generic column (created_at) is never a JOIN condition");
    n = query(c, "SELECT * FROM orders o JOIN profiles p ON ", o);
    CHECK(has(o, n, "p.customer_id = o.customer_id"), "same column name, a primary key on one side: suggested by name");
    for (int i = 0; i < n; i++) if (!strcmp(o[i].text, "p.customer_id = o.customer_id")) CHECK(strstr(o[i].detail, "same column name") != NULL, "  ... and labelled as such (%s)", o[i].detail);
    /* the implicit join: FROM a, b WHERE */
    n = query(c, "select o.*, c.* from orders o, customers c where ", o);
    CHECK(n >= 1 && !strcmp(o[0].text, "o.customer_id = c.id") && strstr(o[0].detail, "fk_orders_customer"), "FROM a, b WHERE: the join condition first, by foreign key (%s)", n ? o[0].text : "-");
    n = query(c, "select * from orders o, customers c where o.customer_id = c.id and ", o);
    CHECK(!has(o, n, "o.customer_id = c.id") && has(o, n, "o.id"), "  ... not proposed again once written (the columns follow)");
    n = query(c, "select * from orders o, customers c where o.customer_id=c.id and ", o);
    CHECK(!has(o, n, "o.customer_id = c.id"), "  ... spaces aside");
    n = query(c, "select * from shipments s, order_items oi, orders o where ", o);
    CHECK(has(o, n, "s.order_id = oi.order_id AND s.item_line = oi.line") && has(o, n, "oi.order_id = o.id"), "three tables: each one joined to the ones before it, in FROM order (composite key whole)");
    n = query(c, "select * from orders o, customers c where o.customer_id = ", o);
    CHECK(n >= 1 && !strcmp(o[0].text, "c.id") && strstr(o[0].detail, "foreign key"), "WHERE a.id = : the column the foreign key pairs with, first (%s)", n ? o[0].text : "-");
    n = query(c, "select * from customers c, orders o where c.id = ", o);
    CHECK(n >= 1 && !strcmp(o[0].text, "o.customer_id"), "  ... in the other direction too (%s)", n ? o[0].text : "-");
    n = query(c, "select * from orders o, profiles p where o.customer_id = ", o);
    CHECK(n >= 1 && !strcmp(o[0].text, "p.customer_id") && strstr(o[0].detail, "same column name"), "  ... same-name key column when there is no foreign key (%s)", n ? o[0].text : "-");
    n = query(c, "select o. from orders o, customers c", o);
    { int pl; const char *q = "select o. from orders o, customers c"; n = complete_query(c, q, (int)strlen(q), 9, false, o, COMPLETE_MAX, &pl); }
    CHECK(n >= 2 && !strcmp(o[0].text, "*") && strstr(o[0].detail, "every column of orders") && has(o, n, "customer_id"), "select a. with several tables: * first, then the columns (%s)", n ? o[0].text : "-");
    n = query(c, "select * from orders o, customers c where o.", o);
    CHECK(n >= 1 && strcmp(o[0].text, "*") != 0, "  ... not in a WHERE");
    n = query(c, "select * from orders o join customers c using (", o);
    CHECK(has(o, n, "id") && has(o, n, "created_at") && !has(o, n, "customer_id") && !has(o, n, "name"), "JOIN b USING (: the columns both sides have");
    n = query(c, "merge into orders o using customers c on c.", o);
    CHECK(has(o, n, "id") && has(o, n, "name"), "MERGE ... USING u c: c is an alias of customers");
    n = query(c, "update orders set ", o);
    CHECK(has(o, n, "customer_id") && !has(o, n, "SET"), "UPDATE t SET: SET is not taken for an alias");
    n = query(c, "select * from orders o, customers c where o.", o);
    for (int i = 0; i < n; i++) if (!strcmp(o[i].text, "customer_id")) CHECK(strstr(o[i].detail, "FK") == NULL, "one qualifier: the detail is the type only (%s)", o[i].detail);
    /* query levels: CTEs, derived tables, correlated subqueries */
    n = query(c, "with t as (select id, name as nm, created_at from customers) select t.", o);
    CHECK(n == 3 && has(o, n, "id") && has(o, n, "nm") && has(o, n, "created_at"), "a CTE: its select list is its columns (%d)", n);
    for (int i = 0; i < n; i++) if (!strcmp(o[i].text, "id")) CHECK(strstr(o[i].detail, "INTEGER") && strstr(o[i].detail, "PK"), "  ... a plain column keeps its type and key (%s)", o[i].detail);
    for (int i = 0; i < n; i++) if (!strcmp(o[i].text, "nm")) CHECK(strstr(o[i].detail, "TEXT") != NULL, "  ... `col AS name` keeps the column's type (%s)", o[i].detail);
    n = query(c, "select * from (select o.id, o.customer_id cid, count(*) n from orders o) d where d.", o);
    CHECK(n == 3 && has(o, n, "id") && has(o, n, "cid") && has(o, n, "n"), "a derived table: `q.col`, `expr name` and an aggregate named (%d)", n);
    for (int i = 0; i < n; i++) if (!strcmp(o[i].text, "n")) CHECK(strstr(o[i].detail, "expression") != NULL, "  ... an expression says so (%s)", o[i].detail);
    n = query(c, "select * from (select * from orders) as d where d.", o);
    CHECK(n == 3 && has(o, n, "customer_id"), "a derived table with *: the FROM's columns (%d)", n);
    n = query(c, "select * from orders o where o.id in (select oi.order_id from order_items oi where oi.", o);
    CHECK(n == 3 && has(o, n, "qty") && !has(o, n, "customer_id"), "inside a subquery, its own alias: its own columns only (%d)", n);
    n = query(c, "select * from orders o where o.id in (select oi.order_id from order_items oi where ", o);
    CHECK(has(o, n, "oi.qty") && has(o, n, "o.customer_id"), "a correlated subquery sees the outer alias too (qualified)");
    n = query(c, "select * from orders o where exists (select 1 from order_items oi where oi.order_id = ", o);
    CHECK(n >= 1 && !strcmp(o[0].text, "o.id") && strstr(o[0].detail, "foreign key"), "  ... and pairs with the outer table by foreign key (%s)", n ? o[0].text : "-");
    n = query(c, "select * from orders o where o.customer_id in (select id from customers) and o.", o);
    CHECK(has(o, n, "customer_id") && !has(o, n, "name"), "after a subquery closes, the outer level again");
    n = query(c, "with recursive t as (select id from customers) select * from t join orders o on ", o);
    CHECK(n >= 1 && has(o, n, "o.id = t.id") == false && n > 0, "a CTE joined: no foreign keys, columns offered (%d)", n);
    { int pl; const char *q = "select cu from orders o, customers c"; n = complete_query(c, q, (int)strlen(q), 9, false, o, COMPLETE_MAX, &pl); }
    for (int i = 0; i < n; i++) if (!strcmp(o[i].text, "o.customer_id")) CHECK(strstr(o[i].detail, "FK \xE2\x86\x92 customers.id") != NULL, "several tables: a foreign-key column says what it points at (%s)", o[i].detail);
    n = query(c, "SELECT * FROM customers c JOIN orders o ON ", o);
    CHECK(!has(o, n, "o.id = c.id"), "each table's own id is not a relation");
    CHECK(n >= 1 && !strcmp(o[0].text, "o.customer_id = c.id"), "the FK still comes first (%s)", n ? o[0].text : "-");
    int before = loads;
    query(c, "SELECT * FROM orders o JOIN customers c ON ", o);
    CHECK(loads == before, "foreign keys are read once per table (cached)");
    n = query(c, "SELECT * FROM orders o JOIN customers c ON c.", o);
    CHECK(n >= 1 && has(o, n, "id"), "after the alias and a dot: that table's columns, as before");
    n = query(c, "SELECT * FROM orders o WHERE ", o);
    CHECK(!has(o, n, "c.id = o.customer_id"), "no JOIN conditions outside ON");

    /* ---- every visible object (objcat): other schemas, synonyms, routines ---- */
    {
        DbColumn mc[] = { C("id", "NUMBER", true) };
        DbTable mine[] = { T("MY_NOTES", mc, 1) };   /* the schema cache: the user's own tables */
        Completer *k = complete_create();
        complete_set_schema(k, mine, 1, NULL, NULL, NULL);
        ObjCat cat; memset(&cat, 0, sizeof cat);
        snprintf(cat.home, sizeof cat.home, "ME");
        objcat_add(&cat, "ME", "MY_NOTES", OC_TABLE, NULL);
        objcat_add(&cat, "ME", "MY_PROC", OC_PROCEDURE, NULL);
        objcat_add(&cat, "APPCORE", "PATIENT_VISIT", OC_TABLE, NULL);
        objcat_add(&cat, "APPCORE", "PATIENT_VISIT_V", OC_VIEW, NULL);
        objcat_add(&cat, "APPCORE", "PKG_BILLING", OC_PACKAGE, NULL);
        objcat_add(&cat, "APPCORE", "SALES_MV", OC_MVIEW, NULL);
        objcat_add(&cat, "APPCORE", "SALES_MV", OC_TABLE, NULL);   /* Oracle lists a materialized view's container table too */
        objcat_add(&cat, "PUBLIC", "VISITS", OC_SYNONYM, "APPCORE.PATIENT_VISIT");
        objcat_add(&cat, "APPAUDIT", "AUDIT_LOG", OC_TABLE, NULL);
        for (int i = 0; i < 30000; i++) { char nm[32]; snprintf(nm, sizeof nm, "T%05d", i); objcat_add(&cat, "BIGSCHEMA", nm, OC_TABLE, NULL); }
        objcat_finish(&cat);
        CHECK(cat.ns == 5 && objcat_schema(&cat, "appcore", 7) && objcat_schema(&cat, "appcore", 7)->n == 4, "the catalog groups objects by schema; a materialized view listed twice is kept once (%d schemas)", cat.ns);
        complete_set_catalog(k, &cat, NULL, NULL, NULL);

        n = query(k, "select * from APP", o);
        CHECK(has(o, n, "APPCORE") && has(o, n, "APPAUDIT"), "schema names are proposed (APP -> APPCORE, APPAUDIT)");
        for (int i = 0; i < n; i++) if (!strcmp(o[i].text, "APPCORE")) CHECK(o[i].kind == CAND_SCHEMA && strstr(o[i].detail, "schema") && strstr(o[i].detail, "4 objects"), "  ... as schemas, with their object count (%s)", o[i].detail);
        { ObjCat many; memset(&many, 0, sizeof many);   /* more than fit: tables before synonyms when nothing else differs */
          for (int i = 0; i < 60; i++) { char nm[16]; snprintf(nm, sizeof nm, "SY%03d", i); objcat_add(&many, "BIG", nm, OC_SYNONYM, "BIG.T000"); snprintf(nm, sizeof nm, "T%03d", i); objcat_add(&many, "BIG", nm, OC_TABLE, NULL); }
          objcat_finish(&many); complete_set_catalog(k, &many, NULL, NULL, NULL);
          int nn = query(k, "select * from BIG.", o); int tabs = 0; for (int i = 0; i < nn; i++) tabs += o[i].kind == CAND_TABLE;
          CHECK(nn == COMPLETE_MAX && tabs >= 48 - 0 && o[0].kind == CAND_TABLE, "a schema with more objects than fit: its tables come first (%d of %d)", tabs, nn);
          complete_set_catalog(k, &cat, NULL, NULL, NULL); objcat_free(&many); }
        n = query(k, "select * from APPCORE.", o);
        CHECK(n == 3 && has(o, n, "PATIENT_VISIT") && has(o, n, "SALES_MV") && !has(o, n, "PKG_BILLING"), "SCHEMA. after FROM lists that schema's tables / views, without the schema (%d)", n);
        n = query(k, "select * from appcore.pat", o);
        CHECK(n == 2 && has(o, n, "PATIENT_VISIT") && has(o, n, "PATIENT_VISIT_V"), "SCHEMA.prefix filters them (case-insensitive schema)");
        n = query(k, "select * from PATIENT_V", o);
        CHECK(has(o, n, "APPCORE.PATIENT_VISIT"), "another schema's table found by its own name, proposed as SCHEMA.NAME");
        n = query(k, "select * from MY_", o);
        CHECK(has(o, n, "MY_NOTES") && !has(o, n, "MY_PROC") && !has(o, n, "ME.MY_NOTES"), "the current schema's objects unqualified; the schema cache's own table not proposed twice");
        { Cand o2[COMPLETE_MAX]; int n2 = query(k, "begin MY_", o2); CHECK(has(o2, n2, "MY_PROC"), "a procedure is proposed where it can be called"); }
        int dup = 0; for (int i = 0; i < n; i++) dup += !strcmp(o[i].text, "MY_NOTES");
        CHECK(dup == 1, "MY_NOTES once (%d)", dup);
        n = query(k, "select * from VIS", o);
        CHECK(has(o, n, "VISITS"), "a PUBLIC synonym unqualified");
        for (int i = 0; i < n; i++) if (!strcmp(o[i].text, "VISITS")) CHECK(o[i].kind == CAND_OBJECT && strstr(o[i].detail, "synonym") && strstr(o[i].detail, "APPCORE.PATIENT_VISIT"), "  ... shown as a synonym with its target (%s)", o[i].detail);
        n = query(k, "select * from APPCORE.PATIENT", o);
        CHECK(has(o, n, "PATIENT_VISIT"), "the qualified name typed whole still matches");
        n = query(k, "select * from T2999", o);
        CHECK(n >= 10 && has(o, n, "BIGSCHEMA.T29990") && has(o, n, "BIGSCHEMA.T29999"), "a large schema: the best matches are kept, not the first ones listed (%d)", n);
        n = query(k, "select * from PUBLIC.", o);
        CHECK(n == 1 && has(o, n, "VISITS"), "PUBLIC. lists the public synonyms");
        n = query(k, "select * from NOSUCH.", o);
        CHECK(n == 0, "an unknown qualifier proposes nothing");
        complete_set_catalog(k, &cat, "APPCORE", NULL, NULL);
        n = query(k, "select * from PATIENT_V", o);
        CHECK(has(o, n, "PATIENT_VISIT") && !has(o, n, "APPCORE.PATIENT_VISIT"), "after a schema change in the session, that schema is the unqualified one");

        /* the disk cache: names and kinds round-trip */
        CHECK(objcat_save(&cat, "build/objcat_check.tsv"), "the catalog is written to its cache");
        ObjCat back; memset(&back, 0, sizeof back);
        CHECK(objcat_load(&back, "build/objcat_check.tsv") && back.n == cat.n && back.ns == cat.ns && !strcmp(back.home, "ME"), "read back: same objects and schemas (%d / %d)", back.n, cat.n);
        const OcSchema *pub = objcat_schema(&back, "PUBLIC", 6);
        CHECK(pub && back.o[pub->first].kind == OC_SYNONYM && back.o[pub->first].target && !strcmp(back.o[pub->first].target, "APPCORE.PATIENT_VISIT"), "a synonym keeps its target");
        FILE *f = fopen("build/objcat_check.tsv", "w"); if (f) { fputs("not a catalog\n", f); fclose(f); }
        ObjCat bad; memset(&bad, 0, sizeof bad);
        CHECK(!objcat_load(&bad, "build/objcat_check.tsv") && bad.n == 0, "a file that is not a catalog cache is ignored");
        CHECK(objcat_sql("oracle", 2, false) && strstr(objcat_sql("oracle", 2, false), "all_synonyms") && objcat_sql("oracle", 1, true) && !objcat_sql("sqlite", 1, false) && !objcat_sql("oracle", 3, false),
              "queries: Oracle reads synonyms and has a fallback for old servers; SQLite has none");
        objcat_free(&back); objcat_free(&cat);
        complete_destroy(k);
    }
    {   /* columns of other schemas' tables and synonyms, from the catalog session (in the background) */
        Completer *k = complete_create();
        complete_set_schema(k, tb, 5, NULL, NULL, NULL);
        ObjCat cat; memset(&cat, 0, sizeof cat); snprintf(cat.home, sizeof cat.home, "ME");
        objcat_add(&cat, "APPCORE", "PATIENT_VISIT", OC_TABLE, NULL);
        objcat_add(&cat, "APPCORE", "DOCTOR", OC_TABLE, NULL);
        objcat_add(&cat, "APPCORE", "PKG_BILLING", OC_PACKAGE, NULL);
        objcat_add(&cat, "PUBLIC", "VISITS", OC_SYNONYM, "APPCORE.PATIENT_VISIT");
        objcat_finish(&cat);
        complete_set_catalog(k, &cat, NULL, fake_table, NULL);

        n = query_x(k, "select * from APPCORE.PATIENT_VISIT a where a.", o, false);
        CHECK(n == 1 && o[0].info && strstr(o[0].label, "Loading columns of APPCORE.PATIENT_VISIT") && complete_pending(k), "alias of another schema's table: a Loading row while its columns come (%s)", n ? shown(&o[0]) : "-");
        CHECK(fake_asks == 1, "the columns were asked for once (%d)", fake_asks);
        DbColumn pv[] = { C("VISIT_ID", "NUMBER", true), C("PATIENT_ID", "NUMBER", false), C("VISIT_DATE", "DATE", false), C("CREATED_BY", "VARCHAR2", false) };
        pv[3].identity_always = false; DbColumn pv_gen = C("TOTAL_V", "NUMBER", false); pv_gen.generated = true;
        DbColumn pvall[] = { pv[0], pv[1], pv[2], pv[3], pv_gen };
        fake_arrive("APPCORE.PATIENT_VISIT", pvall, 5);
        n = query_x(k, "select * from APPCORE.PATIENT_VISIT a where a.", o, false);
        CHECK(n == 5 && !o[0].info && has(o, n, "VISIT_ID") && has(o, n, "VISIT_DATE") && !complete_pending(k), "when they arrive: that table's columns (%d)", n);
        { int stale = 0; for (int i = 0; i < n; i++) stale += o[i].info || o[i].label[0]; CHECK(stale == 0, "  ... none keeps the Loading row's label (the array is reused)"); }
        query_x(k, "select * from APPCORE.PATIENT_VISIT a where a.", o, false);
        CHECK(fake_asks == 1, "  ... and they are not asked for again");
        n = query_x(k, "select * from APPCORE.PATIENT_VISIT where PATIENT_VISIT.", o, false);
        CHECK(has(o, n, "VISIT_ID"), "no alias: the table's own name as qualifier");
        n = query_x(k, "select * from APPCORE.PATIENT_VISIT where APPCORE.PATIENT_VISIT.", o, false);
        CHECK(has(o, n, "VISIT_ID"), "no alias: SCHEMA.TABLE. as qualifier");
        n = query_x(k, "select * from APPCORE.PATIENT_VISIT where ", o, false);
        CHECK(has(o, n, "VISIT_ID") && has(o, n, "PATIENT_ID"), "no alias: WHERE opens with the columns of the table in FROM");
        n = query_x(k, "select  from APPCORE.PATIENT_VISIT", o, false);
        { int pl; n = complete_query(k, "select  from APPCORE.PATIENT_VISIT", 34, 7, true, o, COMPLETE_MAX, &pl); }
        CHECK(has(o, n, "VISIT_ID") && has(o, n, "VISIT_DATE"), "Ctrl+Space between SELECT and FROM: the columns of the table (one table: plain names)");
        n = query_x(k, "select * from VISITS v where v.", o, false);
        CHECK(n == 1 && o[0].info, "a PUBLIC synonym: its columns are asked for too (%s)", n ? shown(&o[0]) : "-");
        DbColumn dc[] = { C("DOCTOR_ID", "NUMBER", true), C("NAME", "VARCHAR2", false) };
        n = query_x(k, "select * from APPCORE.DOCTOR d where d.", o, false);
        fake_arrive("APPCORE.DOCTOR", dc, 2);
        { int pl; const char *q = "select  from APPCORE.PATIENT_VISIT a join APPCORE.DOCTOR d on d.DOCTOR_ID = a.PATIENT_ID"; n = complete_query(k, q, (int)strlen(q), 7, true, o, COMPLETE_MAX, &pl); }
        CHECK(has(o, n, "a.VISIT_ID") && has(o, n, "d.NAME") && !has(o, n, "NAME"), "several tables: every column qualified with its alias (a.VISIT_ID, d.NAME)");
        { int pl; const char *q = "select na from APPCORE.PATIENT_VISIT a join APPCORE.DOCTOR d on 1 = 1"; n = complete_query(k, q, (int)strlen(q), 9, false, o, COMPLETE_MAX, &pl); }
        CHECK(n >= 1 && !strcmp(o[0].text, "d.NAME"), "  ... and found by the column's own name (na -> d.NAME: %s)", n ? o[0].text : "-");
        n = query_x(k, "insert into APPCORE.PATIENT_VISIT ", o, false);
        CHECK(n >= 1 && !strcmp(o[0].text, "(VISIT_ID, PATIENT_ID, VISIT_DATE, CREATED_BY) VALUES (") && strstr(o[0].detail, "1 generated"), "INSERT INTO t: first proposal, every writable column and VALUES ( (%s | %s)", n ? o[0].text : "-", n ? o[0].detail : "-");
        n = query_x(k, "insert into APPCORE.PATIENT_VISIT (", o, false);
        CHECK(n >= 2 && !strcmp(o[0].text, "VISIT_ID, PATIENT_ID, VISIT_DATE, CREATED_BY)") && has(o, n, "VISIT_ID"), "INSERT INTO t (: every column first, then each one (%s)", n ? o[0].text : "-");
        n = query_x(k, "insert into orders ", o, false);
        CHECK(n >= 1 && !strcmp(o[0].text, "(id, customer_id, created_at) VALUES ("), "INSERT on the schema cache's own table too (%s)", n ? o[0].text : "-");
        n = query_x(k, "select * from APPCORE.", o, false);
        CHECK(has(o, n, "DOCTOR") && !has(o, n, "PKG_BILLING"), "after FROM, SCHEMA. lists what can be queried: tables, views, synonyms (no packages)");
        n = query_x(k, "begin APPCORE.", o, false);
        CHECK(has(o, n, "PKG_BILLING"), "elsewhere SCHEMA. lists every kind (a package to call)");
        n = query_x(k, "select * from PKG", o, false);
        CHECK(!has(o, n, "APPCORE.PKG_BILLING"), "after FROM, no packages or procedures");
        /* foreign keys of a catalog table, through the catalog session (late) */
        complete_set_fk_loader(k, fks);
        snprintf(late_fks, sizeof late_fks, "APPCORE.PATIENT_VISIT"); late_out = NULL;
        n = query_x(k, "select * from APPCORE.PATIENT_VISIT v join APPCORE.DOCTOR d on ", o, false);
        CHECK(n >= 1 && o[0].info && strstr(o[0].label, "Loading foreign keys of APPCORE.PATIENT_VISIT") && complete_pending(k), "JOIN ... ON between two catalog tables: a Loading row while the keys come (%s)", n ? shown(&o[0]) : "-");
        { DbFk *f = calloc(1, sizeof *f); f[0].name = strdup("FK_VISIT_DOCTOR"); f[0].from_col = strdup("PATIENT_ID"); f[0].to_table = strdup("APPCORE.DOCTOR"); f[0].to_col = strdup("DOCTOR_ID"); late_out = f; late_n = 1; }
        n = query_x(k, "select * from APPCORE.PATIENT_VISIT v join APPCORE.DOCTOR d on ", o, false);
        CHECK(n >= 1 && !o[0].info && !strcmp(o[0].text, "d.DOCTOR_ID = v.PATIENT_ID") && strstr(o[0].detail, "FK_VISIT_DOCTOR") && !complete_pending(k), "  ... then the condition from the key (%s)", n ? shown(&o[0]) : "-");
        n = query_x(k, "select * from APPCORE.PATIENT_VISIT v, APPCORE.DOCTOR d where ", o, false);
        CHECK(n >= 1 && !strcmp(o[0].text, "v.PATIENT_ID = d.DOCTOR_ID"), "  ... and in the implicit join, in FROM order (%s)", n ? o[0].text : "-");
        /* a synonym: the app's loader resolves it to its target (objcat_fks); the fake answers for the synonym's name */
        snprintf(late_fks, sizeof late_fks, "PUBLIC.VISITS");   /* the fake names its tables schema.name */
        { DbFk *f = calloc(1, sizeof *f); f[0].name = strdup("FK_VISIT_DOCTOR"); f[0].from_col = strdup("PATIENT_ID"); f[0].to_table = strdup("APPCORE.DOCTOR"); f[0].to_col = strdup("DOCTOR_ID"); late_out = f; late_n = 1; }
        n = query_x(k, "select * from VISITS v join APPCORE.DOCTOR d on ", o, false);
        CHECK(has(o, n, "d.DOCTOR_ID = v.PATIENT_ID"), "a synonym on one side: its target's keys (through the loader)");
        late_fks[0] = 0;
        complete_destroy(k); objcat_free(&cat);
    }
    {   /* speed: the popup is recomputed on every key; a 200,000-object catalog must not lag it */
        ObjCat big; memset(&big, 0, sizeof big); snprintf(big.home, sizeof big.home, "ME");
        for (int i = 0; i < 200000; i++) { char sc[32], nm[48]; snprintf(sc, sizeof sc, "SCHEMA_%03d", i % 300); snprintf(nm, sizeof nm, "TB_%c%c_ITEM_%06d", 'A' + i % 26, 'A' + (i / 26) % 26, i); objcat_add(&big, sc, nm, i % 7 ? OC_TABLE : OC_SYNONYM, NULL); }
        objcat_finish(&big);
        Completer *k = complete_create(); complete_set_schema(k, tb, 5, NULL, NULL, NULL); complete_set_catalog(k, &big, NULL, NULL, NULL);
        const char *qs[] = { "select * from T", "select * from IT", "select * from TB_AB", "select * from SCHEMA_120." };
        double worst = 0;
        for (int q = 0; q < 4; q++) {
            double best = 1e9;
            for (int r = 0; r < 3; r++) { struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); query(k, qs[q], o); clock_gettime(CLOCK_MONOTONIC, &t1); double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6; if (ms < best) best = ms; }
            if (best > worst) worst = best;
        }
        CHECK(worst < 60, "200,000 objects: the slowest proposal list takes %.1f ms (under 60 ms with the sanitizers; a few ms in the app)", worst);
        complete_destroy(k); objcat_free(&big);
    }
    complete_destroy(c);
    printf("COMPLETE CHECK %s: %d ok, %d failed\n", fails ? "FAILED" : "OK", oks, fails);
    return fails ? 1 : 0;
}
