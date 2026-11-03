/* SPDX-License-Identifier: AGPL-3.0-only */
/* ER diagram model, headless (docs/diagrams.md): the catalog read with schidx's SQLite statements from
 * the deterministic fixture, then the model's rules, layout, find, grid, file and exports, with the
 * MVP budgets measured. Usage: dgm_check <fixture.db> */
#include "dgm.h"
#include "schidx.h"
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

static int fails, oks;
#define CHECK(c, ...) do { if (c) { oks++; } else { fails++; printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define OK(...) do { printf("ok    "); printf(__VA_ARGS__); printf("\n"); } while (0)
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static long rss_kb(void) { struct rusage u; getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    return u.ru_maxrss / 1024;
#else
    return u.ru_maxrss;
#endif
}
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double p95(double *v, int n) { qsort(v, (size_t)n, sizeof *v, cmpd); return v[(int)(n * 0.95)]; }

static int statements;
static bool load(SchIdx *x, const char *path) {
    sqlite3 *db; if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) return false;
    schidx_init(x); snprintf(x->dialect, sizeof x->dialect, "sqlite");
    char sql[8192]; int kind;
    for (int k = 0; schidx_sql("sqlite", k, NULL, 0, &kind, sql, sizeof sql); k++) {
        sqlite3_stmt *st; statements++;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) { printf("FAIL  catalog statement %d: %s\n", k, sqlite3_errmsg(db)); fails++; continue; }
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *f[8]; for (int c = 0; c < 8; c++) f[c] = c < sqlite3_column_count(st) ? (const char *)sqlite3_column_text(st, c) : NULL;
            if (kind == SXQ_COLS && f[1] && f[3]) { int ti = schidx_table(x, f[0] ? f[0] : "", f[1], f[2] && atoi(f[2])); schidx_add_col(x, ti, f[3], f[4] ? f[4] : "?", f[5] && atoi(f[5]), f[6] && atoi(f[6]), f[7]); }
            else if (kind == SXQ_FKS && f[1] && f[4]) schidx_add_fk2(x, schidx_find(x, f[0], f[1]), f[5], f[2] ? f[2] : "", f[3], f[4], f[6]);
            else if (kind == SXQ_UNIQUES && f[1] && f[2]) schidx_add_unique(x, schidx_find(x, f[0], f[1]), f[2]);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    schidx_finish(x);
    return true;
}
static int nodes_with(const Dgm *d, const char *key) { return dgm_find_node(d, key) >= 0; }
static int edge_between(const Dgm *d, const char *a, const char *b) {
    int ia = dgm_find_node(d, a), ib = dgm_find_node(d, b);
    for (int e = 0; e < d->ne; e++) if (d->e[e].from == ia && d->e[e].to == ib) return e;
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: dgm_check <fixture.db>\n"); return 2; }
    SchIdx x;
    long r0 = rss_kb(); double t0 = now_ms();
    if (!load(&x, argv[1])) { printf("FAIL  open %s\n", argv[1]); return 1; }
    double tload = now_ms() - t0; long r1 = rss_kb();
    int nfk = 0, ncol = 0; for (int i = 0; i < x.n; i++) { nfk += x.t[i].nfks; ncol += x.t[i].ncols; }
    CHECK(x.n >= 14000 && ncol >= 120000 && nfk >= 30000, "fixture read: %d tables, %d columns, %d FKs", x.n, ncol, nfk);
    OK("DGM-071: the whole catalog in %d bulk statements (%d tables, %d columns, %d FKs) in %.0f ms", statements, x.n, ncol, nfk, tload);
    CHECK(statements == 4, "DGM-071: 4 catalog statements whatever the table count (got %d)", statements);
    /* the index's own memory: every string and array it holds, with the allocator's overhead (16 bytes per block) */
    size_t bytes = sizeof(SxTable) * (size_t)x.cap + sizeof(int) * (size_t)x.nslot;
#define SZ(p) ((p) ? strlen(p) + 1 + 16 : 0)
    for (int i = 0; i < x.n; i++) {
        const SxTable *t = &x.t[i];
        bytes += SZ(t->schema) + SZ(t->name) + SZ(t->comment) + SZ(t->uniques) + sizeof(SxCol) * (size_t)t->capcols + sizeof(SxFk) * (size_t)t->capfks;
        for (int c = 0; c < t->ncols; c++) bytes += SZ(t->cols[c].name) + SZ(t->cols[c].type) + SZ(t->cols[c].comment);
        for (int k = 0; k < t->nfks; k++) bytes += SZ(t->fks[k].cols) + SZ(t->fks[k].ref_table) + SZ(t->fks[k].name) + SZ(t->fks[k].ref_cols);
    }
#undef SZ
    double mem_mb = bytes / 1048576.0;
    CHECK(mem_mb < 25, "DGM-072: scope index memory %.1f MB (bar 25 MB)", mem_mb);
    OK("DGM-072: scope index %.1f MB for %d tables / %d columns (process peak grew %ld MB while reading, SQLite included)", mem_mb, x.n, ncol, (r1 - r0) / 1024);

    int ord = schidx_find(&x, NULL, "orders"), sh = schidx_find(&x, NULL, "shipments"), cu = schidx_find(&x, NULL, "customers");
    CHECK(ord >= 0 && sh >= 0 && cu >= 0, "core tables found");
    if (sh >= 0) {
        const SxFk *f = &x.t[sh].fks[0];
        CHECK(x.t[sh].nfks == 1 && !strcmp(f->cols, "order_id,item_line") && f->ref_cols && !strcmp(f->ref_cols, "order_id,line"), "DGM-010: composite FK with ordered pairs (got %s -> %s)", f->cols, f->ref_cols ? f->ref_cols : "?");
    }
    CHECK(cu >= 0 && x.t[cu].uniques && strstr(x.t[cu].uniques, "email"), "unique keys read (customers.email)");

    /* ---- open focused (DGM-001) */
    Dgm d; dgm_init(&d); snprintf(d.name, sizeof d.name, "test");
    dgm_bind(&d, &x);
    t0 = now_ms();
    dgm_open_focus(&d, ord, 40);
    dgm_measure_all(&d, NULL, NULL);
    dgm_layout(&d, false, NULL);
    double topen = now_ms() - t0;
    CHECK(d.nn == 3 && nodes_with(&d, "orders") && nodes_with(&d, "customers") && nodes_with(&d, "order_items"), "DGM-001: orders opens with its 1-hop neighbours only (%d cards)", d.nn);
    CHECK(topen < 300, "DGM-001: focused open + layout in %.1f ms (bar 300)", topen);
    int e = edge_between(&d, "orders", "customers");
    CHECK(e >= 0 && d.e[e].origin == DE_FK && d.e[e].card_from == DC_MANY && d.e[e].card_to == DC_ONE && !d.e[e].optional, "DGM-043: orders -> customers is many-to-one, mandatory (NOT NULL)");
    int oi = edge_between(&d, "order_items", "orders");
    CHECK(oi >= 0 && d.e[oi].card_from == DC_MANY, "DGM-043: order_items -> orders many (FK is part of a composite PK, not the whole)");
    CHECK(d.n[dgm_find_node(&d, "customers")].more == 1 && d.n[dgm_find_node(&d, "order_items")].more == 2, "related tables not on the diagram are counted (customers +1: customer_profiles; order_items +2: products, shipments)");

    /* one-to-one, optional, self reference, no invented relation, composite */
    int cp = schidx_find(&x, NULL, "customer_profiles"), em = schidx_find(&x, NULL, "employees"), ar = schidx_find(&x, NULL, "archive_orders");
    dgm_add_table(&d, cp, 0, 0); dgm_add_table(&d, em, 0, 0); dgm_add_table(&d, ar, 0, 0); dgm_add_table(&d, sh, 0, 0);
    dgm_edges(&d);
    e = edge_between(&d, "customer_profiles", "customers");
    CHECK(e >= 0 && d.e[e].card_from == DC_ONE, "DGM-043: customer_profiles -> customers is one-to-one (FK = PK)");
    e = edge_between(&d, "employees", "employees");
    CHECK(e >= 0 && d.e[e].optional, "self reference drawn as one edge, optional (nullable manager_id)");
    CHECK(edge_between(&d, "archive_orders", "customers") < 0, "DGM-012: archive_orders.customer_id has no constraint: no edge");
    e = edge_between(&d, "shipments", "order_items");
    CHECK(e >= 0 && !strcmp(d.e[e].cols, "order_id,item_line"), "DGM-010: one edge for the composite FK");
    float p[8]; int np = e >= 0 ? dgm_route(&d, e, p) : 0;
    CHECK(np >= 2, "edges are routed (%d points)", np);

    /* external stub: a FK whose target is outside the catalog */
    {
        SchIdx y; schidx_init(&y); int a = schidx_table(&y, "s1", "child", false); schidx_add_col(&y, a, "id", "int", true, true, NULL); schidx_add_col(&y, a, "p_id", "int", false, false, NULL);
        schidx_add_fk2(&y, a, "fk_child_p", "p_id", "other", "parent", "id"); schidx_finish(&y);
        Dgm s; dgm_init(&s); snprintf(s.schemas, sizeof s.schemas, "s1"); dgm_bind(&s, &y); dgm_add_table(&s, a, 0, 0); dgm_edges(&s);
        int st = dgm_find_node(&s, "other.parent");
        CHECK(st >= 0 && s.n[st].kind == DN_STUB && s.ne == 1, "DGM-011 / 014: a target outside the scope is an external stub, not a missing relation");
        char *js = dgm_to_json(&s); CHECK(js && !strstr(js, "other.parent"), "stubs are derived: never saved"); free(js);
        dgm_free(&s); schidx_free(&y);
    }

    /* ---- expansion and budgets (DGM-075 / 070) */
    int hub0 = schidx_find(&x, NULL, "sales_t00000");
    dgm_add_table(&d, hub0, 600, 0);
    int would = dgm_expand(&d, hub0, 3, 0, true);
    CHECK(would > 150, "DGM-075: 3 hops from the hub would add %d tables (counted before adding anything)", would);
    int added = dgm_expand(&d, hub0, 3, 150, false);
    CHECK(added == 150 || added == would, "DGM-070: the budget caps one operation at 150 (added %d)", added);

    /* ---- layout determinism and time (DGM-052 / 058) */
    dgm_measure_all(&d, NULL, NULL);
    t0 = now_ms(); dgm_layout(&d, false, NULL); double tlay = now_ms() - t0;
    char *j1 = dgm_to_json(&d);
    dgm_layout(&d, false, NULL);
    char *j2 = dgm_to_json(&d);
    CHECK(j1 && j2 && !strcmp(j1, j2), "DGM-052 / 058: the same input lays out to a byte-identical file");
    CHECK(tlay < 30 * (d.nn / 200.0 + 1), "DGM-052: layout of %d cards in %.1f ms", d.nn, tlay);
    OK("DGM-052: layered layout of %d cards in %.1f ms", d.nn, tlay);
    /* no two cards overlap after layout */
    int overlaps = 0;
    for (int a = 0; a < d.nn; a++) for (int b = a + 1; b < d.nn; b++) { DgNode *u = &d.n[a], *v = &d.n[b]; if (u->x < v->x + v->w && v->x < u->x + u->w && u->y < v->y + v->h && v->y < u->y + u->h) overlaps++; }
    CHECK(overlaps == 0, "layout: no overlapping cards (%d overlaps)", overlaps);
    /* cancel leaves positions untouched */
    { volatile int cancel = 1; float x0 = d.n[1].x; d.n[1].x += 999; bool done = dgm_layout(&d, false, &cancel); CHECK(!done && d.n[1].x == x0 + 999, "DGM-094: a cancelled layout changes nothing"); d.n[1].x = x0; }
    /* pinned cards stay */
    { int k = 2; d.n[k].pinned = true; d.n[k].x = 12345; dgm_layout(&d, false, NULL); CHECK(d.n[k].x == 12345, "DGM-054: pinned cards keep their position"); d.n[k].pinned = false; }

    /* 1,000-card layout on the big catalog */
    {
        Dgm big; dgm_init(&big); dgm_bind(&big, &x);
        int hub = schidx_find(&x, NULL, "sales_t00000");
        dgm_add_table(&big, hub, 0, 0); dgm_expand(&big, hub, 4, 999, false);
        dgm_measure_all(&big, NULL, NULL);
        t0 = now_ms(); dgm_layout(&big, false, NULL); double tb = now_ms() - t0;
        CHECK(big.nn >= 900 && tb < 500, "DGM-052: %d cards laid out in %.0f ms (bar 500, worker)", big.nn, tb);
        OK("DGM-052: %d cards laid out in %.0f ms", big.nn, tb);
        /* culling: a viewport sees only nearby cards (DGM-073) */
        dgm_grid_build(&big);
        int vis[2048]; float vx = big.n[0].x, vy = big.n[0].y;
        int nv = dgm_grid_query(&big, vx - 400, vy - 300, vx + 800, vy + 500, vis, 2048);
        CHECK(nv > 0 && nv < big.nn / 4, "DGM-073: a 1200x800 viewport returns %d of %d cards", nv, big.nn);
        int lin = 0; for (int i = 0; i < big.nn; i++) { DgNode *n = &big.n[i]; if (n->x < vx + 800 && n->x + n->w > vx - 400 && n->y < vy + 500 && n->y + n->h > vy - 300) lin++; }
        CHECK(lin == nv, "DGM-073: the grid returns exactly the cards a full scan finds (%d / %d)", nv, lin);
        double tq[200]; for (int k = 0; k < 200; k++) { double q0 = now_ms(); dgm_grid_query(&big, vx - 400 + k, vy - 300, vx + 800 + k, vy + 500, vis, 2048); tq[k] = now_ms() - q0; }
        CHECK(p95(tq, 200) < 1, "DGM-073: viewport query p95 %.3f ms", p95(tq, 200));
        dgm_free(&big);
    }

    /* ---- find (DGM-030..035) */
    DgHit *hits = malloc(sizeof *hits * 5000);
    int nh = dgm_find(&d, "order", DF_TABLES, hits, 5000);
    CHECK(nh == 3 && hits[0].node >= 0, "DGM-032: hits on the diagram come first (%d hits)", nh);
    bool sorted = true; int first_off = -1;
    for (int i = 0; i < (nh < 5000 ? nh : 5000); i++) { if (hits[i].node < 0 && first_off < 0) first_off = i; if (first_off >= 0 && hits[i].node >= 0) sorted = false; }
    CHECK(sorted, "DGM-032: then the catalog hits not on the diagram");
    nh = dgm_find(&d, "audit_event_history", DF_TABLES, hits, 5000);
    CHECK(nh == 1 && hits[0].node < 0 && hits[0].ti >= 0, "DGM-034: a table not on the diagram is found in the catalog (not on diagram)");
    nh = dgm_find(&d, "item_line", DF_COLUMNS, hits, 5000);
    CHECK(nh == 1 && strstr(hits[0].text, "shipments.item_line"), "DGM-031: column hits read schema.table.column (%s)", nh ? hits[0].text : "-");
    nh = dgm_find(&d, "*_t0001?", DF_TABLES | DF_GLOB, hits, 5000);
    CHECK(nh == 10, "DGM-036: glob *_t0001? finds 10 tables (got %d)", nh);
    nh = dgm_find(&d, "ORDERS", DF_TABLES | DF_EXACT, hits, 5000);
    CHECK(nh == 1, "DGM-036: exact, case-insensitive by default (%d)", nh);
    nh = dgm_find(&d, "ORDERS", DF_TABLES | DF_EXACT | DF_CASE, hits, 5000);
    CHECK(nh == 0, "DGM-036: case-sensitive on request");
    {
        const char *typed[] = { "s", "sa", "sal", "sale", "sales", "sales_", "sales_t", "sales_t0", "sales_t00", "sales_t001", "b", "bi", "bil", "c", "cl", "cla", "o", "or", "ord", "orde" };
        double tf[60]; int nt = 0;
        for (int r = 0; r < 3; r++) for (int k = 0; k < 20; k++) { double q0 = now_ms(); dgm_find(&d, typed[k], DF_TABLES, hits, 500); tf[nt++] = now_ms() - q0; }
        double pf = p95(tf, nt);
        CHECK(pf < 8, "DGM-035: find per keystroke on %d tables p95 %.2f ms (bar 8)", x.n, pf);
        OK("DGM-035: find per keystroke p95 %.2f ms on %d tables", pf, x.n);
    }
    free(hits);

    /* ---- undo / redo (DGM-024 / 054) */
    dgm_snapshot(&d); float ux = d.n[0].x; d.n[0].x += 100;
    CHECK(dgm_undo(&d) && d.n[0].x == ux, "undo restores the position");
    CHECK(dgm_redo(&d) && d.n[0].x == ux + 100, "redo moves it back");
    CHECK(!dgm_redo(&d), "nothing more to redo");

    /* ---- file round trip and rejection (DGM-058) */
    char *js = dgm_to_json(&d);
    Dgm r; dgm_init(&r); char why[200];
    CHECK(dgm_from_json(&r, js, strlen(js), why, sizeof why), "the file loads back (%s)", why);
    dgm_bind(&r, &x);
    char *js2 = dgm_to_json(&r);
    CHECK(!strcmp(js, js2), "DGM-058: save -> load -> save is byte-identical");
    CHECK(strchr(js, '\n') && strstr(js, "\"formatVersion\": 1"), "DGM-058: one member per line, readable");
    CHECK(!strstr(js, "password") && !strstr(js, "secret"), "RN-08: no credentials in the file");
    free(js2); dgm_free(&r);
    dgm_init(&r);
    CHECK(!dgm_from_json(&r, "{\"formatVersion\": 2}", 20, why, sizeof why) && strstr(why, "newer Lume"), "a newer format is refused with a reason (%s)", why);
    CHECK(!dgm_from_json(&r, "{\"formatVersion\": 1, \"evil\": 1}", 31, why, sizeof why) && strstr(why, "evil"), "an unknown field is refused, named (%s)", why);
    const char *bad = "{\"formatVersion\": 1, \"nodes\": [{\"key\": \"a\", \"kind\": \"view\"}]}";
    CHECK(!dgm_from_json(&r, bad, strlen(bad), why, sizeof why) && strstr(why, "kind"), "an unknown card kind is refused (%s)", why);
    const char *far = "{\"formatVersion\": 1, \"nodes\": [{\"key\": \"a\", \"x\": 1e9}]}";
    CHECK(!dgm_from_json(&r, far, strlen(far), why, sizeof why), "positions out of range are refused");
    CHECK(!dgm_from_json(&r, "not json", 8, why, sizeof why), "garbage is refused");
    dgm_free(&r);
    free(js);

    /* ---- comparison (DGM-016) */
    dgm_resign(&d);
    int c0 = dgm_compare(&d);
    int ni = dgm_find_node(&d, "orders"); snprintf(d.n[ni].sig, sizeof d.n[ni].sig, "0000000000000000");
    int c1 = dgm_compare(&d);
    CHECK(c0 == 0 && c1 == 1 && d.n[ni].status == DS_CHANGED, "DGM-016: a changed table is flagged (%d -> %d)", c0, c1);
    dgm_resign(&d);

    /* ---- user relations, entities, notes, exports, escaping */
    int rv = dgm_add_rel(&d, DE_VIRTUAL, "archive_orders", "customers", "customer_id", "id", "legacy link");
    dgm_edges(&d);
    e = edge_between(&d, "archive_orders", "customers");
    CHECK(rv >= 0 && e >= 0 && d.e[e].origin == DE_VIRTUAL, "DGM-013: a virtual relation is drawn as virtual");
    d.show_virtual = false; dgm_edges(&d);
    CHECK(edge_between(&d, "archive_orders", "customers") < 0, "virtual relations can be hidden independently");
    d.show_virtual = true; dgm_edges(&d);
    int en = dgm_add_entity(&d, "<script>alert(1)</script>&Co", 0, 0);
    snprintf(d.n[en].attrs, sizeof d.n[en].attrs, "PK id integer\nname text");
    dgm_add_note(&d, false, 0, -200, 200, 80, "Note <b>", "a & b");
    dgm_measure_all(&d, NULL, NULL);
    DgSvgOpt so = { .legend = true, .scale = 1, .title = true, .title_text = "T & <x>", .footer = "Lume Data" };
    char *svg = dgm_svg(&d, false, &so);
    CHECK(svg && !strstr(svg, "<script") && strstr(svg, "&lt;script&gt;") && strstr(svg, "&amp;Co"), "DGM-062 / 100: SVG escapes names (no live <script>)");
    CHECK(strstr(svg, "foreign key (database)") && strstr(svg, "stroke-dasharray=\"6 4\""), "DGM-067: legend tells real from virtual");
    free(svg);
    char *html = dgm_html(&d, &so);
    int scripts = 0; for (const char *q = html; (q = strstr(q, "<script")); q++) scripts++;
    CHECK(html && scripts == 1 && !strstr(html, "src=") && !strstr(html, "href=\"http") && !strstr(html, "@import"), "DGM-069: one inline script (ours), no external requests (%d)", scripts);
    free(html);
    int lost = 0; char *mm = dgm_mermaid(&d, &lost);
    CHECK(mm && !strncmp(mm, "erDiagram", 9) && strstr(mm, "customers ||--o{ orders") && strstr(mm, "..") && lost > 0, "DGM-064: Mermaid erDiagram with cardinalities; virtual as non-identifying; losses counted (%d)", lost);
    free(mm);
    char *gm = dgm_graphml(&d); CHECK(gm && strstr(gm, "<graphml") && !strstr(gm, "<script"), "DGM-064: GraphML, escaped"); free(gm);
    char *dio = dgm_drawio(&d); CHECK(dio && strstr(dio, "<mxGraphModel>") && strstr(dio, "ERmany"), "DGM-064: draw.io with ER arrows"); free(dio);

    /* components and align */
    int *comp = malloc(sizeof(int) * (size_t)d.nn); int nc = dgm_components(&d, comp);
    CHECK(nc >= 2, "DGM-055: %d connected components", nc); free(comp);
    for (int i = 0; i < d.nn; i++) d.n[i].sel = i < 4;
    dgm_align(&d, 0);
    CHECK(d.n[0].x == d.n[1].x && d.n[1].x == d.n[3].x, "DGM-056: align left");
    for (int i = 0; i < d.nn; i++) d.n[i].sel = false;

    dgm_free(&d);
    schidx_free(&x);
    printf("DGM CHECK %s: %d ok, %d failed\n", fails ? "FAILED" : "OK", oks, fails);
    return fails ? 1 : 0;
}
