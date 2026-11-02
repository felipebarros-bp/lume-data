/* SPDX-License-Identifier: AGPL-3.0-only */
/* Autocompletion, headless: JOIN ... ON conditions from foreign keys (composite too, either
 * direction, with the statement's aliases) and from same-named key columns; generic columns and
 * each table's own "id" never suggested. */
#include "complete.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the real one lives in db.c (with every driver): the same, here */
void db_fks_free(DbFk *f, int n) { for (int i = 0; i < n; i++) { free(f[i].name); free(f[i].from_col); free(f[i].to_table); free(f[i].to_col); } free(f); }

static int fails, oks;
#define CHECK(c, ...) do { if (c) { oks++; printf("ok    "); } else { fails++; printf("FAIL  "); } printf(__VA_ARGS__); printf("\n"); } while (0)

static DbColumn C(const char *n, const char *t, bool pk) { DbColumn c; memset(&c, 0, sizeof c); c.name = strdup(n); c.type = strdup(t); c.pk = pk; return c; }
static DbTable T(const char *name, DbColumn *cols, int n) { DbTable t; memset(&t, 0, sizeof t); t.name = strdup(name); t.cols = malloc(sizeof *cols * (size_t)n); memcpy(t.cols, cols, sizeof *cols * (size_t)n); t.ncols = n; t.cols_loaded = true; return t; }
static int loads;
static bool fks(void *ctx, const char *table, DbFk **out, int *n) {
    (void)ctx; loads++;
    static const struct { const char *tb, *name, *from, *to, *tocol; } all[] = {
        { "orders", "fk_orders_customer", "customer_id", "customers", "id" },
        { "shipments", "fk_ship_item", "order_id", "order_items", "order_id" },
        { "shipments", "fk_ship_item", "item_line", "order_items", "line" },
        { "order_items", "fk_item_order", "order_id", "orders", "id" },
    };
    int k = 0; DbFk *f = calloc(4, sizeof *f);
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) if (!strcmp(all[i].tb, table)) { f[k].name = strdup(all[i].name); f[k].from_col = strdup(all[i].from); f[k].to_table = strdup(all[i].to); f[k].to_col = strdup(all[i].tocol); k++; }
    *out = f; *n = k;
    return true;
}
static int query(Completer *c, const char *sql, Cand *out) { int pl; return complete_query(c, sql, (int)strlen(sql), (int)strlen(sql), false, out, COMPLETE_MAX, &pl); }
static bool has(Cand *o, int n, const char *text) { for (int i = 0; i < n; i++) if (!strcmp(o[i].text, text)) return true; return false; }

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
    complete_destroy(c);
    printf("COMPLETE CHECK %s: %d ok, %d failed\n", fails ? "FAILED" : "OK", oks, fails);
    return fails ? 1 : 0;
}
