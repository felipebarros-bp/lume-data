/* SPDX-License-Identifier: AGPL-3.0-only */
/* Headless checks for the grid model: sorting, filters, search, selection and serialisation
 * with awkward values (NULL, empty, Unicode, multiline, big numbers, quotes). */
#include "grid.h"
#include "json.h"
#include "geo.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>

static int failures = 0;
static void check(bool ok, const char *what) { printf("%s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) failures++; }

/* fake driver: a cursor that yields `fake_left` more rows of increasing numbers */
static int fake_left;
static int fake_next = 3;
static bool fake_fetch(Db *db, ResultSet *rs, int n) {
    (void)db;
    int got = 0;
    while (got < n && fake_left > 0) { int r = rs_add_row(rs); char b[16]; snprintf(b, sizeof b, "%d", fake_next++); rs_set(rs, r, 0, b, -1); fake_left--; got++; }
    if (fake_left == 0) { rs->cursor = NULL; rs->complete = true; return false; }
    return true;
}
static void fake_release(Db *db, ResultSet *rs) { (void)db; rs->cursor = NULL; }
static bool fake_fetch_slow(Db *db, ResultSet *rs, int n) { usleep(20000); return fake_fetch(db, rs, n); }   /* 20 ms per block, like a slow network */
static const DbDriver slow_drv = { "slow", NULL, NULL, NULL, NULL, NULL, fake_fetch_slow, fake_release, NULL, NULL, NULL, DB_CAP_FETCH_MORE, NULL, NULL, NULL, NULL, NULL };
static Db slow_db = { &slow_drv, NULL, NULL, NULL, 0 };
static const DbDriver fake_drv = { "fake", NULL, NULL, NULL, NULL, NULL, fake_fetch, fake_release, NULL, NULL, NULL, DB_CAP_FETCH_MORE, NULL, NULL, NULL, NULL, NULL };
static Db fake_db = { &fake_drv, NULL, NULL, NULL, 0 };

static ExportProgress last_pr; static long cancel_at;
static bool fake_fetch_slow(Db *db, ResultSet *rs, int n);
static int prog_cancel(void *ud, const ExportProgress *p) { (void)ud; last_pr = *p; return p->rows >= cancel_at ? EXPORT_CANCELLED : EXPORT_GO; }

/* minimal RFC 4180 reader for the round-trip check: returns cells of one record */
static int csv_record(FILE *f, char cells[][128], int max) {
    int n = 0, c, len = 0; bool quoted = false, any = false;
    cells[0][0] = 0;
    while ((c = fgetc(f)) != EOF) {
        any = true;
        if (quoted) {
            if (c == '"') { int d = fgetc(f); if (d == '"') { if (len < 127) cells[n][len++] = '"'; continue; } quoted = false; if (d == EOF) break; c = d; }
            else { if (len < 127) cells[n][len++] = (char)c; continue; }
        }
        if (c == '"' && len == 0) { quoted = true; continue; }
        if (c == ',') { cells[n][len] = 0; if (++n >= max) break; len = 0; cells[n][0] = 0; continue; }
        if (c == '\n') break;
        if (len < 127) cells[n][len++] = (char)c;
    }
    if (!any) return 0;
    cells[n][len] = 0;
    return n + 1;
}
static char *slurp(const char *path, long *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1); size_t got = fread(s, 1, n, f); s[got] = 0; fclose(f); if (len) *len = (long)got; return s;
}

int main(void) {
    ResultSet rs;
    rs_init(&rs);
    rs_add_column(&rs, "id", "INTEGER", CK_NUMBER);
    rs_add_column(&rs, "name", "TEXT", CK_TEXT);
    rs_add_column(&rs, "amount", "", CK_OTHER);     /* unknown type: heuristic */
    rs_add_column(&rs, "flag", "bool", CK_BOOL);
    const char *names[] = { "Ana", "", NULL, "Zé \"quote\", comma", "line1\nline2", "=SUM(A1)", "ana" };
    const char *amounts[] = { "10", "9007199254740993", NULL, "-2.5", "1e3", "7", "10" };
    const char *flags[] = { "t", "f", NULL, "t", "f", "t", "f" };
    for (int i = 0; i < 7; i++) {
        int r = rs_add_row(&rs);
        char id[8]; snprintf(id, sizeof id, "%d", 7 - i);
        rs_set(&rs, r, 0, id, -1);
        rs_set(&rs, r, 1, names[i], -1);
        rs_set(&rs, r, 2, amounts[i], -1);
        rs_set(&rs, r, 3, flags[i], -1);
    }
    check(rs.nrows == 7 && rs_cell(&rs, 2, 1) == NULL && strcmp(rs_cell(&rs, 1, 1), "") == 0, "model keeps NULL and '' apart");
    check(strcmp(rs_cell(&rs, 1, 2), "9007199254740993") == 0, "big integers are kept as text, no precision loss");

    Grid g;
    grid_init(&g, &rs);
    check(g.numeric[0] && !g.numeric[1] && g.numeric[2] && !g.numeric[3], "numeric detection: metadata first, heuristic for unknown");
    check(g.nvis == 7 && g.order[0] == 0, "initial order is the driver order");

    grid_sort_click(&g, 0, false);
    check(strcmp(grid_cell(&g, 0, 0), "1") == 0 && strcmp(grid_cell(&g, 6, 0), "7") == 0, "sort asc by number");
    grid_sort_click(&g, 0, false);
    check(strcmp(grid_cell(&g, 0, 0), "7") == 0, "second click: desc");
    grid_sort_click(&g, 0, false);
    check(g.nsort == 0 && g.order[0] == 0, "third click: original order restored");
    grid_sort_click(&g, 2, false);
    check(grid_cell(&g, 6, 2) == NULL, "NULL sorts last ascending");
    grid_sort_click(&g, 1, true);
    check(g.nsort == 2 && grid_sort_priority(&g, 1, NULL) == 2, "shift-click adds a secondary key");
    grid_sort_clear(&g);

    grid_filter_add(&g, 1, OP_CONTAINS, "ana", NULL);
    check(g.nvis == 2, "contains is case-insensitive and skips NULL");
    grid_filter_clear(&g);
    grid_filter_add(&g, 2, OP_GT, "8", NULL);
    check(g.nvis == 4, "numeric > compares as numbers (1e3 > 8, -2.5 not)");
    grid_filter_add(&g, 3, OP_EQUALS, "t", NULL);
    check(g.nvis == 1, "filters combine with AND");
    char d[128];
    grid_filter_describe(&g, 1, d, sizeof d);
    check(strcmp(d, "flag = t") == 0, "filter description");
    grid_filter_clear(&g);
    grid_filter_add(&g, 1, OP_IS_NULL, NULL, NULL);
    check(g.nvis == 1 && grid_cell(&g, 0, 1) == NULL, "is NULL");
    grid_filter_clear(&g);
    check(g.nvis == 7, "clear filters restores all rows");

    grid_search(&g, "ANA");
    int r, c;
    check(g.nhits == 2 && grid_search_step(&g, 1, &r, &c) && r == 0 && c == 1, "search finds case-insensitively");
    grid_search_step(&g, 1, &r, &c);
    check(r == 6, "search next wraps through hits");
    grid_search(&g, "");

    grid_select_cell(&g, 0, 0, false);
    grid_select_cell(&g, 1, 2, true);
    int r0, c0, r1, c1;
    grid_sel_rect(&g, &r0, &c0, &r1, &c1);
    check(r0 == 0 && r1 == 1 && c0 == 0 && c1 == 2, "shift extends to a rectangle");
    char *tsv = grid_copy(&g, FMT_TSV, true, NULL);
    check(strcmp(tsv, "id\tname\tamount\n7\tAna\t10\n6\t\t9007199254740993\n") == 0, "TSV copy with headers");
    free(tsv);
    grid_select_row(&g, 2, false);
    tsv = grid_copy(&g, FMT_TSV, false, NULL);
    check(strcmp(tsv, "5\tNULL\tNULL\tNULL\n") == 0, "row selection; NULL stays explicit in TSV");
    free(tsv);
    grid_select_cell(&g, 3, 1, false);
    grid_select_cell(&g, 5, 1, true);
    char *csv = grid_copy(&g, FMT_CSV, false, NULL);
    check(strcmp(csv, "\"Zé \"\"quote\"\", comma\"\n\"line1\nline2\"\n=SUM(A1)\n") == 0, "CSV quoting of quotes, commas, newlines");
    free(csv);
    grid_select_all(&g);
    char *json = grid_copy(&g, FMT_JSON, true, NULL);
    check(strstr(json, "{\"id\": 7, \"name\": \"Ana\", \"amount\": 10, \"flag\": true}") != NULL, "JSON: numbers and booleans bare");
    check(strstr(json, "\"name\": null") != NULL && strstr(json, "\"name\": \"\"") != NULL, "JSON: SQL NULL vs empty string");
    check(strstr(json, "\"amount\": 9007199254740993") != NULL, "JSON: big integer verbatim");
    check(strstr(json, "\"line1\\nline2\"") != NULL, "JSON: newline escaped");
    free(json);
    grid_select_cell(&g, 0, 0, false);
    grid_select_cell(&g, 1, 3, true);
    char *sql = grid_copy(&g, FMT_SQL, false, "people");
    check(strstr(sql, "INSERT INTO people (id, name, amount, flag) VALUES (7, 'Ana', 10, TRUE);") != NULL, "SQL INSERT");
    free(sql);
    char *md = grid_copy(&g, FMT_MARKDOWN, true, NULL);
    check(strstr(md, "| id | name | amount | flag |") != NULL && strstr(md, "| ---: |") != NULL, "Markdown header and numeric alignment");
    free(md);

    grid_select_cell(&g, 0, 2, false);
    grid_select_cell(&g, 6, 2, true);
    char sum[256];
    grid_sel_summary(&g, sum, sizeof sum);
    check(strstr(sum, "7\xC3\x97" "1 selection") && strstr(sum, "6 non-null") && strstr(sum, "sum 9.007199255e+15") != NULL && strstr(sum, "min -2.5") != NULL, "selection summary aggregates one numeric column");
    grid_select_cell(&g, 0, 0, false);
    grid_select_cell(&g, 1, 2, true);
    grid_sel_summary(&g, sum, sizeof sum);
    check(strstr(sum, "2\xC3\x97" "3 selection") && strstr(sum, "select one column") != NULL, "no mixed-column aggregates");

    char err[128];
    ExportOpts oc = { FMT_CSV, true, ',', false, false, NULL_EMPTY, false, true, NULL };
    long n = grid_export(&g, "/tmp/lume_grid_check.csv", &oc, 1, err, sizeof err);
    FILE *f = fopen("/tmp/lume_grid_check.csv", "r");
    char line[256]; int lines = 0; bool guarded = false;
    while (f && fgets(line, sizeof line, f)) { lines++; if (strstr(line, "\"'=SUM(A1)\"")) guarded = true; }
    if (f) fclose(f);
    check(n == 7 && lines == 9 && guarded, "CSV export: 7 rows + header, formula guard applied");
    ExportOpts oj = { FMT_JSON, true, ',', false, false, NULL_EMPTY, false, false, NULL };
    n = grid_export(&g, "/tmp/lume_grid_check.json", &oj, 1, err, sizeof err);
    f = fopen("/tmp/lume_grid_check.json", "r");
    lines = 0; int braces = 0;
    while (f && fgets(line, sizeof line, f)) { lines++; if (line[0] == '[' || line[0] == ']') braces++; }
    if (f) fclose(f);
    check(n == 7 && braces == 2, "JSON export is one array");
    remove("/tmp/lume_grid_check.csv"); remove("/tmp/lume_grid_check.json");

    /* export options (US-024): delimiter, NULL policy, quoting, line ending, displayed values */
    grid_select_none(&g);
    ExportOpts o2 = { FMT_CSV, false, ';', true, true, NULL_WORD, true, false, NULL };
    g.fmt.group = true;
    char *s2 = grid_copy_opts(&g, &o2);
    check(strstr(s2, "\"6\";\"\";\"9,007,199,254,740,993\";\"false\"\r\n") != NULL, "CSV options: ; delimiter, quote all, CRLF, grouped display value");
    check(strstr(s2, "\"5\";NULL;NULL;NULL\r\n") != NULL, "CSV options: NULL written as the word");
    free(s2);
    g.fmt.group = false;
    ExportOpts o3 = { FMT_TSV, false, ',', false, false, NULL_BACKSLASH_N, false, true, NULL };
    s2 = grid_copy_opts(&g, &o3);
    check(strstr(s2, "5\t\\N\t\\N\t\\N\n") != NULL && strstr(s2, "'=SUM(A1)") != NULL, "TSV options: \\N for NULL and formula guard");
    free(s2);

    /* display formats (US-018): raw text is never touched, only the shown form */
    char tmp[64];
    g.fmt.group = true;
    check(strcmp(grid_format_value(&g, 2, "9007199254740993", tmp, sizeof tmp), "9,007,199,254,740,993") == 0, "thousands separator keeps 16-digit integers exact");
    check(strcmp(grid_format_value(&g, 2, "-2.5", tmp, sizeof tmp), "-2.5") == 0, "grouping leaves short numbers alone");
    g.fmt.decimals = 2;
    check(strcmp(grid_format_value(&g, 2, "1e3", tmp, sizeof tmp), "1e3") == 0, "exponent notation is left as stored");
    check(strcmp(grid_format_value(&g, 2, "-2.5", tmp, sizeof tmp), "-2.50") == 0, "fixed decimals");
    g.fmt.group = false; g.fmt.decimals = -1;
    g.fmt.date_style = DATE_DMY;
    check(strcmp(grid_format_value(&g, 1, "2024-05-01", tmp, sizeof tmp), "01/05/2024") == 0, "ISO date shown as DD/MM/YYYY");
    check(strcmp(grid_format_value(&g, 1, "2024-05-01 13:45:10.123+02:00", tmp, sizeof tmp), "01/05/2024 13:45:10.123+02:00") == 0, "timestamp keeps time, fraction and offset");
    check(grid_format_value(&g, 1, "Ana", tmp, sizeof tmp) == rs_cell(&rs, 0, 1) || strcmp(grid_format_value(&g, 1, "Ana", tmp, sizeof tmp), "Ana") == 0, "non-dates untouched by the date style");
    g.fmt.date_style = DATE_AS_STORED; g.fmt.tz = TZ_UTC;
    check(strcmp(grid_format_value(&g, 1, "2024-05-01 23:45:10+02:00", tmp, sizeof tmp), "2024-05-01 21:45:10+00:00") == 0, "offset timestamp converted to UTC");
    check(strcmp(grid_format_value(&g, 1, "2024-05-01 23:45:10", tmp, sizeof tmp), "2024-05-01 23:45:10") == 0, "timestamp without offset is never converted");
    g.fmt.tz = TZ_AS_STORED;
    char dsc[96];
    check(grid_describe_datetime("2024-05-01T23:45:10.5Z", dsc, sizeof dsc) && strstr(dsc, "offset +00:00") && strstr(dsc, "milliseconds"), "datetime description: offset and precision");
    check(!grid_describe_datetime("12345", dsc, sizeof dsc), "plain numbers are not dates");

    /* column layout (US-005): hide, move, freeze, round trip */
    grid_col_hide(&g, 1);
    check(g.ncols == 3 && grid_col(&g, 1) == 2 && grid_hidden_count(&g) == 1, "hide column: display map skips it");
    grid_sort_click(&g, 1, false);           /* display col 1 = amount */
    check(g.sort_col[0] == 2 && grid_cell(&g, 6, 1) == NULL, "sort key stored by source column");
    grid_col_move(&g, 2, 0);
    check(grid_col(&g, 0) == 3 && grid_col(&g, 1) == 0 && grid_sort_priority(&g, 2, NULL) == 1, "move column: sort follows the source column");
    grid_freeze(&g, 1);
    char lay[256];
    grid_layout_save(&g, lay, sizeof lay);
    check(strcmp(lay, "order=3,0,2;hidden=1;frozen=1") == 0, "layout serialised");
    grid_layout_reset(&g);
    check(grid_layout_default(&g) && g.ncols == 4 && grid_col(&g, 1) == 1, "reset restores source order");
    check(grid_layout_load(&g, lay) && g.ncols == 3 && grid_col(&g, 0) == 3 && g.hidden[1] && g.frozen == 1, "layout restored");
    check(!grid_layout_load(&g, "order=9,0") && !grid_layout_load(&g, "nonsense"), "bad layouts rejected");
    grid_sort_clear(&g);
    grid_select_all(&g);
    char *t2 = grid_copy(&g, FMT_TSV, true, NULL);
    check(strncmp(t2, "flag\tid\tamount\n", 15) == 0, "copy follows the display order and skips hidden columns");
    free(t2);
    grid_layout_reset(&g);

    /* JSON parser (US-020) */
    Json j;
    const char *doc = "{\"a\": [1, 2.5e3, \"x\\u00e9\\n\", null, true], \"b\": {\"c\": {}}}";
    check(json_parse(&j, doc, strlen(doc)) && j.n == 9 && j.nodes[0].nchildren == 2 && j.nodes[1].nchildren == 5, "JSON parse: node count and children");
    check(j.nodes[1].key_len == 1 && j.src[j.nodes[1].key_off] == 'a' && j.nodes[5].type == JN_NULL && j.nodes[6].type == JN_TRUE, "JSON parse: keys and literals");
    char un[32];
    json_unescape(j.src + j.nodes[4].val_off, j.nodes[4].val_len, un, sizeof un);
    check(strcmp(un, "x\xC3\xA9\n") == 0, "JSON unescape: \\u and \\n");
    json_free(&j);
    check(!json_parse(&j, "{\"a\": }", 7) && strstr(j.err, "offset 6") != NULL, "JSON error reports the offset");
    json_free(&j);
    check(!json_parse(&j, "[1, 2] x", 8) && strstr(j.err, "trailing") != NULL, "JSON rejects trailing text");
    json_free(&j);
    check(!json_parse(&j, "NULL", 4), "SQL NULL is not JSON null");
    json_free(&j);

    /* full-result streaming export (US-025) through a fake cursor */
    ResultSet big;
    rs_init(&big);
    rs_add_column(&big, "n", "INTEGER", CK_NUMBER);
    for (int i = 0; i < 3; i++) { int rr = rs_add_row(&big); char b[8]; snprintf(b, sizeof b, "%d", i); rs_set(&big, rr, 0, b, -1); }
    big.db = &fake_db; big.cursor = &fake_left; big.truncated = true;
    fake_left = 5000;
    Grid gb;
    grid_init(&gb, &big);
    grid_sort_click(&gb, 0, false); grid_sort_click(&gb, 0, false);     /* desc locally: must not affect the stream */
    ExportOpts of = { FMT_CSV, true, ',', false, false, NULL_EMPTY, false, false, NULL };
    ExportReport rep;
    long written = grid_export_full(&gb, "/tmp/lume_grid_full.csv", &of, 0, NULL, NULL, &rep);
    f = fopen("/tmp/lume_grid_full.csv", "r");
    lines = 0; bool first_ok = false, last_ok = false;
    while (f && fgets(line, sizeof line, f)) { lines++; if (lines == 2 && strcmp(line, "0\n") == 0) first_ok = true; if (strcmp(line, "5002\n") == 0) last_ok = true; }
    if (f) fclose(f);
    check(written == 5003 && lines == 5004 && first_ok && last_ok, "full export streams 3 loaded + 5000 cursor rows in source order");
    check(big.nrows == 3 && big.cursor == NULL && big.cursor_lost && !big.complete, "streamed rows are not kept; the set is marked partial");
    remove("/tmp/lume_grid_full.csv");
    check(rep.status == EXPORT_OK && rep.bytes > 20000 && rep.rows == 5003, "report: status ok, bytes and rows");
    fake_left = 10; big.cursor = &fake_left; big.cursor_lost = false;
    written = grid_export_full(&gb, "/tmp/lume_grid_full.csv", &of, 7, NULL, NULL, &rep);
    check(written == 7 && rep.status == EXPORT_STOP_ROWS && rep.cursor_released && big.cursor == NULL && big.cursor_lost, "row limit past the loaded rows: stops, releases the cursor (no silent gap), set marked partial");
    remove("/tmp/lume_grid_full.csv");
    /* US-047: a limit inside the loaded rows keeps the cursor usable */
    fake_left = 10; big.cursor = &fake_left; big.cursor_lost = false; big.complete = false;
    written = grid_export_full(&gb, "/tmp/lume_grid_full.csv", &of, 2, NULL, NULL, &rep);
    check(written == 2 && rep.status == EXPORT_STOP_ROWS && !rep.cursor_released && big.cursor != NULL, "row limit inside the loaded rows keeps the cursor open");
    remove("/tmp/lume_grid_full.csv");
    /* cancel from the progress callback: nothing left behind, cursor released */
    fake_left = 9000; big.cursor = &fake_left; big.cursor_lost = false; cancel_at = 4000;
    written = grid_export_full(&gb, "/tmp/lume_grid_full.csv", &of, 0, prog_cancel, NULL, &rep);
    check(written == -2 && rep.status == EXPORT_CANCELLED && access("/tmp/lume_grid_full.csv", F_OK) != 0 && access("/tmp/lume_grid_full.csv.part", F_OK) != 0, "cancel: no file and no .part left");
    check(big.cursor == NULL && big.cursor_lost && rep.cursor_released && big.nrows == 3, "cancel releases the cursor and keeps the loaded rows");
    check(last_pr.total == -1 && last_pr.bytes > 0 && last_pr.rows >= 4000, "progress carries rows, bytes and an unknown total while a cursor is open");
    /* limits (US-047) travel in ExportOpts and are enforced by grid_export_full itself (no callback):
     * the same path the export dialog uses */
    fake_left = 9000; big.cursor = &fake_left; big.cursor_lost = false; big.complete = false; big.db = &slow_db;
    ExportOpts ot = of; ot.max_secs = 0.05;
    written = grid_export_full(&gb, "/tmp/lume_grid_full.csv", &ot, 0, NULL, NULL, &rep);
    check(written > 3 && written < 9003 && rep.status == EXPORT_STOP_TIME && !rep.hard_cap && rep.cursor_released && access("/tmp/lume_grid_full.csv", F_OK) == 0 && access("/tmp/lume_grid_full.csv.part", F_OK) != 0, "time limit: stops after the limit, keeps the partial file under its final name, releases the cursor");
    remove("/tmp/lume_grid_full.csv");
    big.db = &fake_db;
    fake_left = 400000; big.cursor = &fake_left; big.cursor_lost = false; big.complete = false;
    ExportOpts om = of; om.max_mb = 1;
    written = grid_export_full(&gb, "/tmp/lume_grid_full.csv", &om, 0, NULL, NULL, &rep);
    struct stat st_m; bool have_m = stat("/tmp/lume_grid_full.csv", &st_m) == 0;
    check(rep.status == EXPORT_STOP_BYTES && !rep.hard_cap && written < 400003 && have_m && st_m.st_size >= (1 << 20) && st_m.st_size < (1 << 20) + (1 << 18) && rep.cursor_released, "size limit (1 MB): stops within one block past the limit, partial file kept, cursor released");
    remove("/tmp/lume_grid_full.csv");
    /* hard caps: lowered for the test, applied whatever the options say (0 = none) */
    grid_export_set_hard_caps(0, 8192);
    fake_left = 9000; big.cursor = &fake_left; big.cursor_lost = false; big.complete = false;
    written = grid_export_full(&gb, "/tmp/lume_grid_full.csv", &of, 0, NULL, NULL, &rep);
    check(rep.status == EXPORT_STOP_BYTES && rep.hard_cap && rep.bytes >= 8192 && written < 9003 && access("/tmp/lume_grid_full.csv", F_OK) == 0, "hard size cap stops an export with no limit set and says it was the hard cap");
    remove("/tmp/lume_grid_full.csv");
    ExportOpts ob = of; ob.max_mb = 100;
    fake_left = 9000; big.cursor = &fake_left; big.cursor_lost = false; big.complete = false;
    written = grid_export_full(&gb, "/tmp/lume_grid_full.csv", &ob, 0, NULL, NULL, &rep);
    check(rep.status == EXPORT_STOP_BYTES && rep.hard_cap, "a user limit above the hard cap does not lift it");
    remove("/tmp/lume_grid_full.csv");
    grid_export_set_hard_caps(0.05, 0);
    fake_left = 9000; big.cursor = &fake_left; big.cursor_lost = false; big.complete = false; big.db = &slow_db;
    written = grid_export_full(&gb, "/tmp/lume_grid_full.csv", &of, 0, NULL, NULL, &rep);
    check(rep.status == EXPORT_STOP_TIME && rep.hard_cap && written < 9003 && rep.cursor_released, "hard time cap stops an export with no limit set");
    remove("/tmp/lume_grid_full.csv");
    grid_export_set_hard_caps(0, 0);
    big.db = &fake_db;
    /* write failure (file size limit stands in for a full disk): OS error, nothing left */
    struct rlimit rl_old, rl; getrlimit(RLIMIT_FSIZE, &rl_old); rl = rl_old; rl.rlim_cur = 4096;
    signal(SIGXFSZ, SIG_IGN); setrlimit(RLIMIT_FSIZE, &rl);
    fake_left = 9000; big.cursor = &fake_left; big.cursor_lost = false; big.complete = false;
    written = grid_export_full(&gb, "/tmp/lume_grid_full.csv", &of, 0, NULL, NULL, &rep);
    check(written == -1 && rep.status == EXPORT_ERR_WRITE && strstr(rep.err, "write failed") != NULL, "a failed write reports the OS error with its own status");
    printf("      (%s)\n", rep.err);
    check(access("/tmp/lume_grid_full.csv", F_OK) != 0 && access("/tmp/lume_grid_full.csv.part", F_OK) != 0, "a failed export leaves no file and no .part");
    ResultSet many; rs_init(&many); rs_add_column(&many, "t", "TEXT", CK_TEXT);
    for (int i = 0; i < 3000; i++) { int rr = rs_add_row(&many); rs_set(&many, rr, 0, "0123456789abcdef", -1); }
    many.ok = many.has_rows = many.complete = true;
    Grid gm; grid_init(&gm, &many);
    long nm = grid_export(&gm, "/tmp/lume_grid_many.csv", &of, 1, err, sizeof err);
    check(nm == -1 && strstr(err, "write failed") != NULL && access("/tmp/lume_grid_many.csv", F_OK) != 0 && access("/tmp/lume_grid_many.csv.part", F_OK) != 0, "loaded-rows export: write failure reported, nothing left");
    nm = grid_export_xlsx(&gm, "/tmp/lume_grid_many.xlsx", 1, true, false, err, sizeof err);
    check(nm == -1 && strstr(err, "write failed") != NULL && access("/tmp/lume_grid_many.xlsx", F_OK) != 0 && access("/tmp/lume_grid_many.xlsx.part", F_OK) != 0, "XLSX export: write failure reported, nothing left");
    setrlimit(RLIMIT_FSIZE, &rl_old);
    nm = grid_export(&gm, "/tmp/lume_grid_many.csv", &of, 1, err, sizeof err);
    check(nm == 3000 && access("/tmp/lume_grid_many.csv", F_OK) == 0 && access("/tmp/lume_grid_many.csv.part", F_OK) != 0, "after the limit is lifted the export lands under its final name only");
    remove("/tmp/lume_grid_many.csv");
    grid_free(&gm); rs_free(&many);
    grid_free(&gb); rs_free(&big);

    /* US-046: formula guard policy and format fidelity */
    ResultSet inj; rs_init(&inj);
    rs_add_column(&inj, "text", "TEXT", CK_TEXT); rs_add_column(&inj, "num", "NUMERIC", CK_NUMBER);
    const char *it[] = { " =1+1", "\t@cmd", "-abc", "normal", "=HYPERLINK(\"http://x\")" };
    const char *in[] = { "-2.5", "+5", "1e3", "=bad", "-0" };
    for (int i = 0; i < 5; i++) { int rr = rs_add_row(&inj); rs_set(&inj, rr, 0, it[i], -1); rs_set(&inj, rr, 1, in[i], -1); }
    inj.ok = inj.has_rows = inj.complete = true;
    Grid gi; grid_init(&gi, &inj);
    ExportOpts og = { FMT_CSV, true, ',', false, false, NULL_EMPTY, false, true, NULL };
    grid_export(&gi, "/tmp/lume_grid_inj.csv", &og, 1, err, sizeof err);
    char *txt = slurp("/tmp/lume_grid_inj.csv", NULL);
    check(txt && strstr(txt, "\"' =1+1\",-2.5\n") && strstr(txt, "\"'\t@cmd\",+5\n") && strstr(txt, "\"'-abc\",1e3\n"), "guard: = + - @ after spaces or control characters get an apostrophe; numbers in numeric columns are exempt");
    check(txt && strstr(txt, "normal,\"'=bad\"\n") && strstr(txt, "\"'=HYPERLINK(\"\"http://x\"\")\",-0\n"), "guard: a non-number in a numeric column is still guarded; -0 is a number");
    free(txt);
    og.guard = false;
    grid_export(&gi, "/tmp/lume_grid_inj.csv", &og, 1, err, sizeof err);
    txt = slurp("/tmp/lume_grid_inj.csv", NULL);
    check(txt && strstr(txt, " =1+1,-2.5\n") && strstr(txt, "normal,=bad\n"), "guard off: values written verbatim (explicit choice)");
    free(txt);
    og.fmt = FMT_TSV; og.guard = true;
    grid_export(&gi, "/tmp/lume_grid_inj.tsv", &og, 1, err, sizeof err);
    txt = slurp("/tmp/lume_grid_inj.tsv", NULL);
    check(txt && strstr(txt, "' =1+1\t-2.5\n") && strstr(txt, "'\\t@cmd\t+5\n"), "TSV guard: apostrophe prefix, control characters escaped");
    free(txt);
    remove("/tmp/lume_grid_inj.csv"); remove("/tmp/lume_grid_inj.tsv");
    grid_export_xlsx(&gi, "/tmp/lume_grid_inj.xlsx", 1, true, false, err, sizeof err);
    long xl = 0; txt = slurp("/tmp/lume_grid_inj.xlsx", &xl);
    bool no_formula = true, text_cell = false, num_cell = false;
    for (long i = 0; txt && i + 3 < xl; i++) if (memcmp(txt + i, "<f>", 3) == 0 || memcmp(txt + i, "<f ", 3) == 0 || memcmp(txt + i, "t=\"str\"", 7) == 0) no_formula = false;
    for (long i = 0; txt && i + 60 < xl; i++) {
        if (memcmp(txt + i, "t=\"inlineStr\"><is><t xml:space=\"preserve\">=HYPERLINK(&quot;http://x&quot;)</t>", 72) == 0) text_cell = true;
        if (memcmp(txt + i, "\"><v>-2.5</v></c>", 17) == 0) num_cell = true;
    }
    check(txt && no_formula && text_cell && num_cell, "XLSX: formulas never created, '=HYPERLINK' stays an inline string, numbers typed");
    free(txt); remove("/tmp/lume_grid_inj.xlsx");
    grid_free(&gi); rs_free(&inj);

    /* round trips of the awkward set: CSV with \\N keeps NULL vs '' apart; JSON parses back */
    grid_filter_clear(&g); grid_sort_clear(&g); grid_layout_reset(&g); grid_select_none(&g);
    ExportOpts orr = { FMT_CSV, true, ',', false, false, NULL_BACKSLASH_N, false, true, NULL };
    grid_export(&g, "/tmp/lume_grid_rt.csv", &orr, 1, err, sizeof err);
    f = fopen("/tmp/lume_grid_rt.csv", "r");
    char cells[8][128]; int rec = 0; bool rt_ok = f != NULL;
    if (f) csv_record(f, cells, 8);   /* header */
    while (f && csv_record(f, cells, 8) == 4) {
        for (int c = 0; c < 4 && rt_ok; c++) {
            const char *v = rs_cell(&rs, rec, c);
            const char *want = v ? v : "\\N";
            if (c == 1 && v && v[0] == '=') want = "'=SUM(A1)";    /* the guard is the one documented change */
            if (strcmp(cells[c], want) != 0) { rt_ok = false; printf("      row %d col %d: got [%s] want [%s]\n", rec, c, cells[c], want); }
        }
        rec++;
    }
    if (f) fclose(f);
    check(rt_ok && rec == 7, "CSV round trip: NULL (\\N) vs '' vs quotes, commas, newlines and Unicode survive");
    remove("/tmp/lume_grid_rt.csv");
    orr.fmt = FMT_JSON;
    grid_export(&g, "/tmp/lume_grid_rt.json", &orr, 1, err, sizeof err);
    long jl = 0; txt = slurp("/tmp/lume_grid_rt.json", &jl);
    Json jd; bool jok = txt && json_parse(&jd, txt, jl);
    int nulls = 0, empties = 0, bigs = 0, nl = 0;
    for (int i = 0; jok && i < jd.n; i++) {
        JsonNode *nd = &jd.nodes[i];
        if (nd->key_len == 4 && memcmp(txt + nd->key_off, "name", 4) == 0) {
            if (nd->type == JN_NULL) nulls++;
            if (nd->type == JN_STRING && nd->val_len == 0) empties++;
            if (nd->type == JN_STRING) { char b[64]; json_unescape(txt + nd->val_off, nd->val_len, b, sizeof b); if (strcmp(b, "line1\nline2") == 0) nl++; }
        }
        if (nd->type == JN_NUMBER && nd->val_len == 16 && memcmp(txt + nd->val_off, "9007199254740993", 16) == 0) bigs++;
    }
    check(jok && jd.nodes[0].nchildren == 7 && nulls == 1 && empties == 1 && bigs == 1 && nl == 1, "JSON round trip: 7 objects, null vs \"\", big integer exact, newline escaped and decoded");
    if (jok) json_free(&jd);
    free(txt); remove("/tmp/lume_grid_rt.json");

    /* search hits follow layout changes without a full re-search (hide / move / show) */
    grid_layout_reset(&g); grid_filter_clear(&g); grid_sort_clear(&g);
    grid_search(&g, "ana");
    check(g.nhits == 2 && g.hits[1] == 1, "two hits in the name column");
    grid_col_move(&g, 1, 3);
    check(g.nhits == 2 && g.hits[1] == 3, "moving the column moves its hits");
    grid_col_hide(&g, 3);
    check(g.nhits == 0, "hiding the column drops its hits");
    grid_col_show(&g, 1);
    check(g.nhits == 2, "showing it again finds them again");
    grid_search(&g, ""); grid_layout_reset(&g);

    /* US-053: a display time zone never changes what is exported raw */
    g.fmt.tz = TZ_LOCAL; g.fmt.date_style = DATE_DMY; g.fmt.group = true;
    ResultSet tsr; rs_init(&tsr); rs_add_column(&tsr, "ts", "timestamptz", CK_DATE); rs_add_column(&tsr, "n", "numeric", CK_NUMBER);
    { int rr = rs_add_row(&tsr); rs_set(&tsr, rr, 0, "2024-05-01 23:45:10+02:00", -1); rs_set(&tsr, rr, 1, "1234567.891", -1); }
    tsr.ok = tsr.has_rows = tsr.complete = true;
    Grid gt; grid_init(&gt, &tsr); gt.fmt = g.fmt;
    char shown[64];
    const char *disp = grid_display(&gt, 0, 0, shown, sizeof shown);
    ExportOpts oraw = { FMT_CSV, false, ',', false, false, NULL_EMPTY, false, false, NULL };
    grid_export(&gt, "/tmp/lume_grid_tz.csv", &oraw, 1, err, sizeof err);
    txt = slurp("/tmp/lume_grid_tz.csv", NULL);
    check(disp != NULL && strcmp(disp, "2024-05-01 23:45:10+02:00") != 0 && txt && strcmp(txt, "2024-05-01 23:45:10+02:00,1234567.891\n") == 0, "display formats (local time, DD/MM, grouping) leave the raw export untouched");
    free(txt); remove("/tmp/lume_grid_tz.csv");
    oraw.displayed = true;
    grid_export(&gt, "/tmp/lume_grid_tz.csv", &oraw, 1, err, sizeof err);
    txt = slurp("/tmp/lume_grid_tz.csv", NULL);
    check(txt && strstr(txt, "1,234,567.891") != NULL && !strstr(txt, "2024-05-01 23:45:10+02:00"), "'Displayed values' is the separate, explicit choice");
    free(txt); remove("/tmp/lume_grid_tz.csv");
    grid_free(&gt); rs_free(&tsr);
    g.fmt.tz = TZ_AS_STORED; g.fmt.date_style = DATE_AS_STORED; g.fmt.group = false;

    /* OR between filters (US-011) */
    grid_layout_reset(&g);
    grid_filter_add(&g, 1, OP_EQUALS, "Ana", NULL);
    grid_filter_add(&g, 2, OP_EQUALS, "7", NULL);
    check(g.nvis == 0, "AND: no row is both Ana and 7");
    grid_filter_mode(&g, true);
    check(g.nvis == 3, "OR: rows matching either filter (2 Ana + 1 amount 7)");
    grid_filter_mode(&g, false);
    grid_filter_clear(&g);

    /* XLSX (US-026): a zip with the sheet; numbers as numbers, strings inline (no formulas) */
    grid_select_none(&g);
    long xn = grid_export_xlsx(&g, "/tmp/lume_grid_check.xlsx", 1, true, false, err, sizeof err);
    f = fopen("/tmp/lume_grid_check.xlsx", "rb");
    unsigned char hdr[4] = { 0 };
    long fsize = 0;
    if (f) { fread(hdr, 1, 4, f); fseek(f, 0, SEEK_END); fsize = ftell(f); fclose(f); }
    check(xn == 7 && hdr[0] == 'P' && hdr[1] == 'K' && fsize > 1200, "XLSX export: 7 rows, zip signature");
    char *xml = NULL; { FILE *fx = fopen("/tmp/lume_grid_check.xlsx", "rb"); if (fx) { xml = malloc(fsize + 1); fread(xml, 1, fsize, fx); xml[fsize] = 0; fclose(fx); } }
    bool inl = xml && memmem(xml, fsize, "t=\"inlineStr\"><is><t xml:space=\"preserve\">=SUM(A1)</t>", 54) != NULL;
    bool numc = xml && memmem(xml, fsize, "<c r=\"A2\"><v>7</v></c>", 22) != NULL;
    bool boolc = xml && memmem(xml, fsize, "t=\"b\"><v>1</v>", 14) != NULL;
    check(inl && numc && boolc, "XLSX cells: formula text stays a string, numbers numeric, booleans typed");
    free(xml);
    remove("/tmp/lume_grid_check.xlsx");

    /* transposed and compared sets (US-038 / US-040) */
    ResultSet tr;
    grid_transpose(&g, 0, 1, &tr);
    check(tr.ncols == 4 && tr.nrows == 4 && strcmp(rs_cell(&tr, 1, 0), "name") == 0 && strcmp(rs_cell(&tr, 0, 2), "7") == 0 && rs_cell(&tr, 1, 3) != NULL && strcmp(rs_cell(&tr, 1, 3), "") == 0, "transpose: columns become rows, values kept");
    rs_free(&tr);
    ResultSet rsb;
    rs_init(&rsb);
    rs_add_column(&rsb, "id", "INTEGER", CK_NUMBER);
    rs_add_column(&rsb, "name", "TEXT", CK_TEXT);
    const char *bids[] = { "7", "6", "5", "99" }, *bnames[] = { "Ana", "changed", NULL, "new" };
    for (int i = 0; i < 4; i++) { int rr2 = rs_add_row(&rsb); rs_set(&rsb, rr2, 0, bids[i], -1); rs_set(&rsb, rr2, 1, bnames[i], -1); }
    Grid gb2; grid_init(&gb2, &rsb);
    ResultSet diff; CompareStats st;
    grid_compare(&g, &gb2, 0, 0, &diff, &st);
    check(st.same == 2 && st.changed == 1 && st.only_a == 4 && st.only_b == 1 && st.keys_unique, "compare: same / changed / only-in counts on the key column");
    bool arrow = false, onlyb = false;
    for (int i = 0; i < diff.nrows; i++) {
        const char *d = rs_cell(&diff, i, 0), *nm = rs_cell(&diff, i, 2);
        if (d && strcmp(d, "changed") == 0 && nm && strstr(nm, "\xE2\x86\x92 changed")) arrow = true;
        if (d && strcmp(d, "only in B") == 0 && strcmp(rs_cell(&diff, i, 1), "99") == 0) onlyb = true;
    }
    check(arrow && onlyb, "compare: changed cells show old -> new, B-only rows carry their key");
    rs_free(&diff); grid_free(&gb2); rs_free(&rsb);

    /* SQL INSERT of binary cells (US-053): literal follows the source dialect; unloaded values are NULL + comment */
    {
        static const DbDriver pgn = { "PostgreSQL" }, oran = { "Oracle" }, sqn = { "SQLite" };
        Db dpg = { &pgn }, dora = { &oran }, dsq = { &sqn };
        ResultSet rb; rs_init(&rb);
        rs_add_column(&rb, "id", "INTEGER", CK_NUMBER); rs_add_column(&rb, "b", "BLOB", CK_BINARY);
        const char *vals[][2] = { { "1", "\\x00ff10" }, { "2", "0A0B" }, { "3", "<blob 2000000 bytes>" }, { "4", "<lob>" }, { "5", "\\x" } };
        for (int i = 0; i < 5; i++) { int rr = rs_add_row(&rb); rs_set(&rb, rr, 0, vals[i][0], -1); rs_set(&rb, rr, 1, vals[i][1], -1); }
        rb.ok = rb.has_rows = rb.complete = true;
        Grid gbin; grid_init(&gbin, &rb); grid_select_all(&gbin);
        rb.db = &dsq; char *s1 = grid_copy(&gbin, FMT_SQL, true, "t");
        rb.db = &dpg; char *s2 = grid_copy(&gbin, FMT_SQL, true, "t");
        rb.db = &dora; char *s3 = grid_copy(&gbin, FMT_SQL, true, "t");
        check(strstr(s1, "(1, X'00ff10')") && strstr(s1, "(2, X'0A0B')") && strstr(s1, "(5, X'')"), "SQLite: X'..' blob literal");
        check(strstr(s2, "(1, '\\x00ff10'::bytea)"), "PostgreSQL: '\\x..'::bytea");
        check(strstr(s3, "(2, HEXTORAW('0A0B'))"), "Oracle: HEXTORAW('..')");
        check(strstr(s1, "(3, NULL /* <blob 2000000 bytes> not loaded */)") && strstr(s1, "(4, NULL /* <lob> not loaded */)"), "values the driver did not load are NULL with a comment, never made-up bytes");
        free(s1); free(s2); free(s3); grid_free(&gbin); rs_free(&rb);
    }

    /* geometry and binary helpers (US-021 / US-041) */
    Geo ge;
    check(geo_parse("SRID=4326;POINT(-34.9 -8.05)", 28, &ge) && ge.type == GEO_POINT && ge.srid == 4326 && ge.npts == 1 && ge.pts[0].x == -34.9, "EWKT point with SRID");
    geo_free(&ge);
    check(geo_parse("POLYGON((0 0, 4 0, 4 3, 0 3, 0 0),(1 1, 2 1, 2 2, 1 1))", 55, &ge) && ge.type == GEO_POLYGON && ge.nparts == 2 && ge.npts == 9 && ge.maxx == 4, "WKT polygon with a hole");
    geo_free(&ge);
    const char *ewkb = "0101000020E6100000000000000000F03F0000000000000040";
    check(geo_parse(ewkb, strlen(ewkb), &ge) && ge.from_wkb && ge.type == GEO_POINT && ge.srid == 4326 && ge.pts[0].x == 1 && ge.pts[0].y == 2, "EWKB hex point (PostGIS form)");
    geo_free(&ge);
    check(!geo_parse("POINTLESS", 9, &ge) && !geo_parse("12345", 5, &ge), "non-geometry text rejected");
    /* US-059: SRID absent, projected CRS, multipolygon, invalid input, the 200,000-point cap */
    char gdesc[240];
    check(geo_parse("POINT(10 20)", 12, &ge) && ge.srid == 0, "WKT without SRID: srid 0");
    geo_describe(&ge, gdesc, sizeof gdesc);
    check(strstr(gdesc, "SRID not stated  ") && !strstr(gdesc, "stated 0") && strstr(gdesc, "CRS unknown") && strstr(gdesc, "no reprojection") && !strstr(gdesc, "lat"), "no SRID: 'SRID not stated', CRS unknown, never lat/lon");
    geo_free(&ge);
    const char *utm = "SRID=31983;POINT(285000 9110000)";
    check(geo_parse(utm, strlen(utm), &ge) && ge.srid == 31983 && ge.pts[0].x == 285000, "projected CRS (SIRGAS 2000 / UTM 23S) parsed as stored");
    geo_describe(&ge, gdesc, sizeof gdesc);
    check(strstr(gdesc, "SRID 31983") && strstr(gdesc, "CRS units, not lat/lon") && strstr(gdesc, "no reprojection"), "projected CRS: axes in CRS units, explicitly not lat/lon");
    geo_free(&ge);
    const char *mp = "SRID=4326;MULTIPOLYGON(((0 0,1 0,1 1,0 0)),((5 5,6 5,6 6,5 5),(5.2 5.2,5.4 5.2,5.4 5.4,5.2 5.2)))";
    check(geo_parse(mp, strlen(mp), &ge) && ge.type == GEO_MULTIPOLYGON && ge.nparts == 3 && ge.npts == 12 && ge.maxx == 6, "MULTIPOLYGON: two polygons, one with a hole (3 rings, 12 points)");
    geo_describe(&ge, gdesc, sizeof gdesc);
    check(strstr(gdesc, "lon") && strstr(gdesc, "EPSG:4326") && strstr(gdesc, "3 parts, 12 points"), "EPSG:4326 caption names lon / lat and the counts");
    geo_free(&ge);
    const char *bad[] = { "POLYGON((0 0, 1 0", "POINT(1)", "LINESTRING(a b, 1 2)", "SRID=;POINT(1 2)", "SRID=abc;POINT(1 2)", "SRID=4326x;POINT(1 2)", "MULTIPOLYGON(((0 0,1 1)", "0101000020E610", NULL };
    bool bad_ok = true;
    for (int i = 0; bad[i]; i++) { Geo gb_; if (geo_parse(bad[i], strlen(bad[i]), &gb_)) { bad_ok = false; printf("      accepted: %s\n", bad[i]); geo_free(&gb_); } }
    check(bad_ok, "invalid / truncated WKT and EWKB refused without a crash");
    {
        size_t cap = 300000 * 24 + 64; char *big_wkt = malloc(cap); size_t o = 0;
        o += snprintf(big_wkt + o, cap - o, "LINESTRING(");
        for (int i = 0; i < 300000; i++) o += snprintf(big_wkt + o, cap - o, "%s%d %d", i ? "," : "", i, i % 7);
        o += snprintf(big_wkt + o, cap - o, ")");
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        bool okp = geo_parse(big_wkt, o, &ge);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        geo_describe(&ge, gdesc, sizeof gdesc);
        check(okp && ge.truncated && ge.npts == 200000 && strstr(gdesc, "truncated at 200,000") && ms < 250, "300,000-point linestring: stops at 200,000, says so, parses in under 250 ms");
        printf("      (%.1f ms)\n", ms);
        geo_free(&ge); free(big_wkt);
    }
    unsigned char bytes[8];
    check(hex_decode("\\x424d0001", 10, bytes, sizeof bytes) == 4 && bytes[0] == 'B' && bytes[1] == 'M' && looks_like_hex_blob("\\x00ff") && !looks_like_hex_blob("\\xzz"), "hex blob decode");
    char b64[16];
    base64_encode((const unsigned char *)"Lume", 4, b64, sizeof b64);
    check(strcmp(b64, "THVtZQ==") == 0, "base64");

    /* US-053: special float values and control bytes survive export as valid JSON / verbatim CSV */
    {
        ResultSet rs2; rs_init(&rs2);
        rs_add_column(&rs2, "v", "double precision", CK_NUMBER);
        rs_add_column(&rs2, "t", "text", CK_TEXT);
        const char *vals[] = { "NaN", "Infinity", "-inf", "0x1p3", "1e308", "-0" };
        for (int i = 0; i < 6; i++) { int r = rs_add_row(&rs2); rs_set(&rs2, r, 0, vals[i], -1); rs_set(&rs2, r, 1, i ? "a\x01" "b" : "\x7f", -1); }
        Grid g2; grid_init(&g2, &rs2);
        char *j = grid_copy(&g2, FMT_JSON, true, NULL);
        check(strstr(j, "\"v\": \"NaN\"") && strstr(j, "\"v\": \"Infinity\"") && strstr(j, "\"v\": \"-inf\"") && strstr(j, "\"v\": \"0x1p3\""), "JSON: NaN/Infinity/hex floats are quoted (valid JSON)");
        check(strstr(j, "\"v\": 1e308") && strstr(j, "\"v\": -0"), "JSON: finite exponent and -0 stay bare");
        check(strstr(j, "\"a\\u0001b\"") != NULL, "JSON: control byte escaped as \\u0001");
        check(!strstr(j, "NaN,") && !strstr(j, ": NaN"), "JSON: no bare NaN token anywhere");
        free(j);
        char *c = grid_copy(&g2, FMT_CSV, true, NULL);
        check(strstr(c, "Infinity,a\x01" "b") || strstr(c, "Infinity,\"a\x01" "b\""), "CSV: NaN verbatim, control byte kept");
        free(c);
        grid_free(&g2); rs_free(&rs2);
    }

    grid_free(&g);
    rs_free(&rs);
    printf("%s (%d failures)\n", failures ? "GRID MODEL: FAIL" : "GRID MODEL: OK", failures);
    return failures ? 1 : 0;
}
