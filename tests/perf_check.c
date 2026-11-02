/* SPDX-License-Identifier: AGPL-3.0-only */
#include <stdarg.h>
/* Timing of the grid model on 100k rows: sort, filter, search, serialise. */
#include "grid.h"
#include "json.h"
#include "sysinfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
static int failures = 0;
/* the bar from docs/benchmarks.md: any single action on 100k rows under 250 ms */
static void bar(double ms, const char *what) { printf("%s  %s: %.1f ms (bar 250 ms)\n", ms <= 250 ? "ok  " : "FAIL", what, ms); if (ms > 250) failures++; }
/* One timed step against its bar (docs/benchmarks.md: any single action on 100k rows under
 * 250 ms). `fmt` is the line printed, its first conversion receives the elapsed ms. */
static void timed(double ms, double limit, const char *fmt, ...) {
    char line[240]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    bool ok = ms <= limit;
    printf("%s  %s (bar %.0f ms)\n", ok ? "ok  " : "FAIL", line, limit);
    if (!ok) failures++;
}
#define TIMED(limit, fmt, ...) do { double ms_ = now_ms() - t0; timed(ms_, limit, fmt, ms_, ##__VA_ARGS__); } while (0)

int main(void) {
    ResultSet rs;
    rs_init(&rs);
    rs_add_column(&rs, "id", "INTEGER", CK_NUMBER);
    rs_add_column(&rs, "amount", "REAL", CK_NUMBER);
    rs_add_column(&rs, "label", "TEXT", CK_TEXT);
    rs_add_column(&rs, "maybe", "TEXT", CK_TEXT);
    double t0 = now_ms();
    for (int i = 0; i < 100000; i++) {
        int r = rs_add_row(&rs);
        char b[64];
        snprintf(b, sizeof b, "%d", i); rs_set(&rs, r, 0, b, -1);
        snprintf(b, sizeof b, "%.2f", (100000 - i) * 2.5); rs_set(&rs, r, 1, b, -1);
        snprintf(b, sizeof b, "row %d", (i * 7919) % 100000); rs_set(&rs, r, 2, b, -1);
        rs_set(&rs, r, 3, i % 3 ? "v" : NULL, -1);
    }
    TIMED(250, "load 100k x 4: %.1f ms, %.1f MB", rs_bytes(&rs) / 1048576.0);
    { double mb = rs_bytes(&rs) / 1048576.0; printf("%s  model of 100k x 4: %.1f MB (bar 10 MB)\n", mb <= 10 ? "ok  " : "FAIL", mb); if (mb > 10) failures++; }
    Grid g; t0 = now_ms(); grid_init(&g, &rs); TIMED(250, "grid_init: %.1f ms");
    t0 = now_ms(); grid_sort_click(&g, 1, false); bar(now_ms() - t0, "sort numeric 100k");
    t0 = now_ms(); grid_sort_click(&g, 2, false); TIMED(250, "sort text 100k: %.1f ms");
    t0 = now_ms(); grid_sort_click(&g, 2, true); grid_sort_click(&g, 0, true); TIMED(250, "two-key sort: %.1f ms");
    t0 = now_ms(); grid_search(&g, "row 9999"); bar(now_ms() - t0, "search 400k cells");
    t0 = now_ms(); grid_filter_add(&g, 2, OP_CONTAINS, "5", NULL); TIMED(250, "filter contains: %.1f ms (%d rows)", g.nvis);
    grid_filter_clear(&g);
    grid_select_all(&g);
    t0 = now_ms(); char *s = grid_copy(&g, FMT_TSV, true, NULL); TIMED(250, "TSV of 100k rows: %.1f ms, %.1f MB", strlen(s) / 1048576.0); free(s);
    t0 = now_ms(); char err[128]; ExportOpts o = { FMT_JSON, true, ',', false, false, NULL_EMPTY, false, false, NULL }; grid_export(&g, "/tmp/lume_perf.json", &o, 1, err, sizeof err); TIMED(250, "JSON export 100k: %.1f ms");
    remove("/tmp/lume_perf.json");
    grid_select_none(&g);
    t0 = now_ms(); grid_col_hide(&g, 1); grid_col_move(&g, 2, 0); grid_freeze(&g, 1); TIMED(250, "hide + move + freeze: %.2f ms");
    char lay[256]; grid_layout_save(&g, lay, sizeof lay);
    t0 = now_ms(); grid_layout_load(&g, lay); TIMED(250, "layout load: %.2f ms");
    grid_layout_reset(&g);
    g.fmt.group = true; g.fmt.decimals = 2; g.fmt.date_style = DATE_DMY;
    t0 = now_ms();
    char tmp[64]; size_t changed = 0;
    for (int i = 0; i < g.nvis; i++) for (int c = 0; c < g.ncols; c++) { const char *v = grid_display(&g, i, c, tmp, sizeof tmp); if (v == tmp) changed++; }
    TIMED(250, "format 400k cells: %.1f ms (%zu changed)", changed);
    grid_select_none(&g);
    t0 = now_ms(); long xr = grid_export_xlsx(&g, "/tmp/lume_perf.xlsx", 1, true, false, err, sizeof err); bar(now_ms() - t0, "XLSX export 100k"); (void)xr; remove("/tmp/lume_perf.xlsx");
    ResultSet cmp; CompareStats cst;
    t0 = now_ms(); grid_compare(&g, &g, 0, 0, &cmp, &cst); bar(now_ms() - t0, "compare 100k vs 100k"); rs_free(&cmp);
    /* JSON: a 1 MB document */
    size_t cap = 1200000; char *doc = malloc(cap); size_t p = 0;
    p += snprintf(doc + p, cap - p, "[");
    for (int i = 0; p < cap - 100; i++) p += snprintf(doc + p, cap - p, "%s{\"id\": %d, \"name\": \"item %d\", \"tags\": [\"a\", \"b\"], \"ok\": true, \"v\": null}", i ? "," : "", i, i);
    p += snprintf(doc + p, cap - p, "]");
    Json j;
    t0 = now_ms(); bool ok = json_parse(&j, doc, p); { double ms_ = now_ms() - t0; timed(ms_, 250, "JSON parse %.1f MB: %.1f ms (%s, %d nodes)", p / 1048576.0, ms_, ok ? "ok" : j.err, j.n); }
    json_free(&j); free(doc);
    grid_free(&g); rs_free(&rs);
    printf("rss after 100k x 4: %.1f MB\n", sys_rss_bytes() / 1048576.0);

    /* US-052: 200 columns, long strings, a big JSON document */
    ResultSet wide; rs_init(&wide);
    for (int c = 0; c < 200; c++) { char n[16]; snprintf(n, sizeof n, "c%d", c); rs_add_column(&wide, n, c % 3 ? "TEXT" : "INTEGER", c % 3 ? CK_TEXT : CK_NUMBER); }
    t0 = now_ms();
    for (int i = 0; i < 20000; i++) {
        int r = rs_add_row(&wide);
        for (int c = 0; c < 200; c++) { char b[32]; snprintf(b, sizeof b, c % 3 ? "t%d_%d" : "%d", i * 7 % 1000, c); rs_set(&wide, r, c, b, -1); }
    }
    TIMED(250, "load 20k x 200 (4M cells): %.1f ms, %.1f MB", rs_bytes(&wide) / 1048576.0);
    Grid gw; t0 = now_ms(); grid_init(&gw, &wide); TIMED(250, "grid_init 200 cols: %.1f ms");
    t0 = now_ms(); grid_sort_click(&gw, 3, false); TIMED(250, "sort 20k rows on col 3: %.1f ms");
    t0 = now_ms(); grid_search(&gw, "t999_"); TIMED(250, "search 4M cells: %.1f ms (%d hits)", gw.nhits);
    t0 = now_ms(); for (int c = 0; c < 100; c++) grid_col_hide(&gw, 0); grid_freeze(&gw, 2); TIMED(250, "hide 100 columns + freeze: %.2f ms");
    t0 = now_ms(); grid_select_all(&gw); char *tw = grid_copy(&gw, FMT_TSV, true, NULL); TIMED(250, "TSV of 20k x 100: %.1f ms, %.1f MB", strlen(tw) / 1048576.0); free(tw);
    grid_free(&gw); rs_free(&wide);
    ResultSet longs; rs_init(&longs); rs_add_column(&longs, "blob", "TEXT", CK_TEXT); rs_add_column(&longs, "n", "INTEGER", CK_NUMBER);
    char *big = malloc(65536); for (int i = 0; i < 65535; i++) big[i] = 'a' + i % 26; big[65535] = 0;
    t0 = now_ms();
    for (int i = 0; i < 1000; i++) { int r = rs_add_row(&longs); rs_set(&longs, r, 0, big, 65535); char b[16]; snprintf(b, sizeof b, "%d", i); rs_set(&longs, r, 1, b, -1); }
    TIMED(250, "load 1000 rows x 64 KB strings: %.1f ms, %.1f MB", rs_bytes(&longs) / 1048576.0);
    Grid gl; grid_init(&gl, &longs);
    t0 = now_ms(); grid_search(&gl, "xyz"); TIMED(250, "search 64 MB of text: %.1f ms (%d hits)", gl.nhits);
    t0 = now_ms(); grid_sort_click(&gl, 0, false); TIMED(250, "sort 1000 x 64 KB strings: %.1f ms");
    char tmp2[64]; t0 = now_ms(); for (int i = 0; i < gl.nvis; i++) grid_display(&gl, i, 0, tmp2, sizeof tmp2); TIMED(250, "display pass over long cells: %.2f ms");
    grid_free(&gl); rs_free(&longs); free(big);
    cap = 6000000; doc = malloc(cap); p = 0;
    p += snprintf(doc + p, cap - p, "{\"items\": [");
    for (int i = 0; p < cap - 200; i++) p += snprintf(doc + p, cap - p, "%s{\"id\": %d, \"name\": \"item %d\", \"tags\": [\"a\", \"b\", \"c\"], \"nested\": {\"x\": %d.5, \"ok\": false}}", i ? "," : "", i, i, i);
    p += snprintf(doc + p, cap - p, "]}");
    t0 = now_ms(); ok = json_parse(&j, doc, p); { double ms_ = now_ms() - t0; timed(ms_, 250, "JSON parse %.1f MB: %.1f ms (%s, %d nodes)", p / 1048576.0, ms_, ok ? "ok" : j.err, j.n); }
    json_free(&j); free(doc);
    double rss = sys_rss_bytes() / 1048576.0;
    printf("%s  rss at end: %.1f MB (bar 150 MB)\n", rss <= 150 ? "ok  " : "FAIL", rss); if (rss > 150) failures++;
    printf("%s (%d failures)\n", failures ? "PERF: FAIL" : "PERF: OK", failures);
    return failures ? 1 : 0;
}
