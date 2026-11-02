/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Schema Explorer v2 model, headless (DBX-077..126): adapters' SQL, featured list, Others pages,
 * server-side search, exact counts only, pins / Move to Others, persistence, stale answers,
 * timeouts and errors, against a simulated catalog of 170,000 schemas.
 *
 * The fake server reads the SQL the adapter wrote (keyset cursor, LIKE pattern, row limit, system
 * filter) and answers from a sorted name array, so the test checks the contract the real servers
 * see: one page per request, no scan of the catalog for search, no per-schema query.
 */
#include "nsx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static int fails, oks;
#define EXPECT(cond, ...) do { if (cond) { oks++; printf("ok    "); } else { fails++; printf("FAIL  "); } printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- the simulated catalog ---- */
static char **cat; static unsigned char *catsys; static int ncat;
static int scmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void cat_build(int users, const char *const *extra, int nextra, const char *const *sys, int nsys) {
    for (int i = 0; i < ncat; i++) free(cat[i]);
    free(cat); free(catsys);
    ncat = users + nextra + nsys;
    cat = malloc(sizeof *cat * (size_t)ncat); catsys = calloc((size_t)ncat, 1);
    int k = 0; char b[32];
    for (int i = 0; i < users; i++) { snprintf(b, sizeof b, "USR_%06d", i); cat[k++] = strdup(b); }
    for (int i = 0; i < nextra; i++) cat[k++] = strdup(extra[i]);
    for (int i = 0; i < nsys; i++) cat[k++] = strdup(sys[i]);
    qsort(cat, (size_t)ncat, sizeof *cat, scmp);
    for (int i = 0; i < ncat; i++) for (int j = 0; j < nsys; j++) if (!strcmp(cat[i], sys[j])) catsys[i] = 1;
}

/* what the fake server saw */
static int pages_served, rows_scanned_by_search, count_queries, object_queries, scope_queries, names_queries;
static char last_sql[16384];

/* text of the first SQL string literal after `marker`, unescaped ('' -> ', \x kept) */
static bool lit_after(const char *sql, const char *marker, char *out, int n) {
    const char *p = strstr(sql, marker);
    if (!p) return false;
    p = strchr(p + strlen(marker) - 1, '\'');
    if (!p) return false;
    p++;
    int o = 0;
    while (*p && o < n - 1) { if (*p == '\'' && p[1] == '\'') { out[o++] = '\''; p += 2; continue; } if (*p == '\'') break; out[o++] = *p++; }
    out[o] = 0;
    return true;
}
static int limit_of(const char *sql) {
    const char *p;
    if ((p = strstr(sql, "ROWNUM <= "))) return atoi(p + 10);
    if ((p = strstr(sql, "LIMIT "))) return atoi(p + 6);
    if ((p = strstr(sql, "TOP ("))) return atoi(p + 5);
    return 1000000;
}
/* LIKE pattern with \ escapes -> literal prefix / substring */
static void unlike(const char *pat, char *out, int n, bool *sub) {
    int o = 0; *sub = pat[0] == '%';
    for (const char *p = pat + (*sub ? 1 : 0); *p && o < n - 1; p++) {
        if (*p == '\\' && p[1]) { out[o++] = *++p; continue; }
        if (*p == '%') break;
        out[o++] = *p;
    }
    out[o] = 0;
}

static ResultSet rs2(void) { ResultSet r; rs_init(&r); rs_add_column(&r, "a", "", 0); rs_add_column(&r, "b", "", 0); rs_add_column(&r, "c", "", 0); r.ok = r.has_rows = true; return r; }
static void row(ResultSet *r, const char *a, const char *b, const char *c) { int i = rs_add_row(r); rs_set(r, i, 0, a, -1); rs_set(r, i, 1, b, -1); rs_set(r, i, 2, c, -1); }

/* scope answer */
static char scope_cur[64] = "ABC", scope_user[64] = "ABC", scope_con[64] = "PDB1";
/* failure injection for the next request of a kind */
static int fail_kind = -1; static const char *fail_err;
static int timeout_kind = -1;

static void serve(NsState *s, uint64_t now) {
    const NsReq *q = nsx_next_request(s, now);
    if (!q) return;
    snprintf(last_sql, sizeof last_sql, "%s", q->sql);
    int kind = q->kind;
    if (timeout_kind == kind) { timeout_kind = -1; nsx_complete(s, NULL, NULL, true, true, q->timeout_ms, now); return; }
    if (fail_kind == kind) { fail_kind = -1; nsx_complete(s, NULL, fail_err, false, false, 3, now); return; }
    ResultSet r = rs2();
    const char *sql = q->sql;
    if (kind == NSR_SCOPE) { scope_queries++; row(&r, scope_cur, scope_user, scope_con); }
    else if (kind == NSR_COUNT) { count_queries++; char n[32]; int c = 0; bool sys = !strstr(sql, "NOT "); for (int i = 0; i < ncat; i++) c += sys || !catsys[i]; snprintf(n, sizeof n, "%d", c); row(&r, n, "", ""); }
    else if (kind == NSR_NAMES) {
        names_queries++;
        for (int i = 0; i < ncat; i++) { char needle[80]; snprintf(needle, sizeof needle, "'%s'", cat[i]); if (strstr(strstr(sql, " IN ("), needle)) row(&r, cat[i], catsys[i] ? "1" : "0", ""); }
    } else if (kind == NSR_PAGE || kind == NSR_SEARCH) {
        char after[200] = "", pat[200] = "", pfx[200] = ""; bool sub = false;
        bool has_after = lit_after(sql, "name > ", after, sizeof after) || lit_after(sql, "username > ", after, sizeof after);
        /* the search predicate, not the system filter (PG's "nspname LIKE 'pg\_%'") */
        bool has_pat = lit_after(sql, "nspname ILIKE ", pat, sizeof pat) || lit_after(sql, "UPPER(username) LIKE ", pat, sizeof pat) ||
                       lit_after(sql, "AND username LIKE ", pat, sizeof pat) || lit_after(sql, "schema_name LIKE ", pat, sizeof pat) || lit_after(sql, "AND name LIKE ", pat, sizeof pat);
        if (has_pat) unlike(pat, pfx, sizeof pfx, &sub);
        bool nosys = strstr(sql, "oracle_maintained = 'N'") || strstr(sql, "AND NOT ");
        int lim = limit_of(sql);
        /* keyset: binary search the start, like an index range scan */
        int lo = 0, hi = ncat;
        const char *start = has_after ? after : has_pat && !sub ? pfx : "";
        while (lo < hi) { int m = (lo + hi) / 2; if (strcmp(cat[m], start) < 0 || (has_after && !strcmp(cat[m], start))) lo = m + 1; else hi = m; }
        int got = 0;
        for (int i = sub ? 0 : lo; i < ncat && got < lim; i++) {
            if (kind == NSR_SEARCH) rows_scanned_by_search++;
            if (nosys && catsys[i]) continue;
            if (has_pat && !sub && strncmp(cat[i], pfx, strlen(pfx))) break;   /* past the prefix range */
            if (has_pat && sub && !strcasestr(cat[i], pfx)) continue;
            row(&r, cat[i], catsys[i] ? "1" : "0", ""); got++;
        }
        if (kind == NSR_PAGE) pages_served++;
    } else if (kind == NSR_PROBE) {
        object_queries++;
        if (strstr(sql, "'USR_000001'")) row(&r, "1", "", "");   /* only this one has a visible object */
    } else if (kind == NSR_TABLES) {
        object_queries++;
        row(&r, "USR_000001.T1", "0", ""); row(&r, "USR_000001.V1", "1", "");
    } else if (kind == NSR_OBJSEARCH) {
        object_queries++;
        row(&r, "USR_000777", "VIEW", "VW_CLIENTES");
    } else if (kind == NSR_DISCOVER) {
        object_queries++;
        for (int i = 0; i < 30; i++) { char b[32]; snprintf(b, sizeof b, "USR_%06d", 1000 + i); row(&r, b, "", ""); }
    }
    nsx_complete(s, &r, NULL, false, false, 5, now);
    rs_free(&r);
}
static void drain(NsState *s, uint64_t *now) { for (int i = 0; i < 200 && nsx_busy(s); i++) { *now += 300; serve(s, *now); } }

int main(void) {
    char dir[] = "/tmp/nsx_check_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    nsx_set_dir(dir);
    uint64_t now = 1000;

    /* ---- adapters: SQL text (DBX-091 / 094 / 099..103) ---- */
    char sql[16384];
    const NsAdapter *ora = nsx_adapter("oracle"), *pg = nsx_adapter("postgres"), *my = nsx_adapter("mysql"), *ms = nsx_adapter("sqlserver");
    EXPECT(ora && pg && my && ms && nsx_adapter("mariadb"), "DBX-103: adapters for oracle, postgres, mysql, mariadb, sqlserver");
    EXPECT(!nsx_adapter("sqlite"), "DBX-102: SQLite has no Schemas / Others adapter (main / temp / attached keep the native tree)");
    EXPECT(!strcmp(my->label, "Databases") && !strcmp(pg->label, "Schemas"), "DBX-101: MySQL keeps the word Databases; the others say Schemas");
    EXPECT(ora->caps.paged_names && ora->caps.server_search && ora->caps.visibility_probe && !ora->caps.cheap_count, "DBX-103: Oracle declares paged names, server search, probe and NO cheap count");
    NsPageReq pr = { "USR_000099", "", false, 101, false, false };
    ora->page_sql(&pr, sql, sizeof sql);
    EXPECT(strstr(sql, "FROM all_users") && strstr(sql, "username > 'USR_000099'") && strstr(sql, "ROWNUM <= 101") && strstr(sql, "ORDER BY username"), "DBX-094: Oracle page = ALL_USERS keyset by USERNAME, bounded: %s", sql);
    EXPECT(!strstr(sql, "all_objects") && !strstr(sql, "dba_") && !strstr(sql, "DBA_") && !strstr(sql, "COUNT"), "DBX-091 / 083: the page reads no ALL_OBJECTS, no DBA_* view, no COUNT");
    EXPECT(strstr(sql, "oracle_maintained = 'N'") != NULL, "DBX-082: system accounts filtered by ORACLE_MAINTAINED, not by a name prefix");
    NsPageReq pr2 = { "", "usr_165000", false, 201, true, false };
    ora->page_sql(&pr2, sql, sizeof sql);
    EXPECT(strstr(sql, "username LIKE 'USR\\_165000%' ESCAPE '\\'") && !strstr(sql, "username >"), "DBX-085 / 094: search USR_165000 = one prefix query on USERNAME (escaped _), no cursor walk: %s", sql);
    NsPageReq pr3 = { "", "O'X", true, 11, true, true };
    ora->page_sql(&pr3, sql, sizeof sql);
    EXPECT(strstr(sql, "'%O''X%'") && strstr(sql, "0 AS sys") && !strstr(sql, "oracle_maintained"), "quotes doubled in literals; substring is explicit; no-ORACLE_MAINTAINED fallback");
    ora->probe_sql("ABC", NSK_ALL, sql, sizeof sql);
    EXPECT(strstr(sql, "owner = 'ABC'") && strstr(sql, "subobject_name IS NULL") && strstr(sql, "ROWNUM = 1"), "DBX-092 / 096: probe = one owner, main objects only (SUBOBJECT_NAME IS NULL), first row: %s", sql);
    pg->page_sql(&pr, sql, sizeof sql);
    EXPECT(strstr(sql, "nspname LIKE 'pg\\_%')") && strstr(sql, "AND NOT (nspname IN"), "PostgreSQL system filter survives formatting ('pg\\_%%' intact): %s", sql);
    EXPECT(strstr(sql, "pg_namespace") && strstr(sql, "has_schema_privilege") && strstr(sql, "nspname > 'USR_000099'") && strstr(sql, "LIMIT 101"), "DBX-099: PostgreSQL page = pg_namespace with USAGE privilege, keyset, LIMIT");
    NsPageReq pr4 = { "a\\b'", "x_", false, 5, true, false };
    my->page_sql(&pr4, sql, sizeof sql);
    EXPECT(strstr(sql, "'a\\\\b'''") && strstr(sql, "LIKE 'x\\\\_%'") && strstr(sql, "information_schema.schemata"), "DBX-101: MySQL literals double backslashes and quotes; LIKE escape survives: %s", sql);
    ms->page_sql(&pr, sql, sizeof sql);
    EXPECT(strstr(sql, "TOP (101)") && strstr(sql, "sys.schemas") && strstr(sql, "name > N'USR_000099'"), "DBX-100: SQL Server page = sys.schemas, TOP, N'' literal (metadata visibility applies)");

    /* ---- Oracle, 170,000 schemas ---- */
    const char *extra[] = { "ABC", "ATENDIMENTO", "SYN_FAT", "XPTO", "CLIENTES", "PRIVADO" };
    const char *sys[] = { "SYS", "SYSTEM", "AUDSYS" };
    cat_build(170000, extra, 6, sys, 3);
    NsState *s = nsx_get("c1", "oracle");
    nsx_connected(s, "", (double)now);
    EXPECT(nsx_busy(s), "DBX-083: connecting queues the scope read only");
    serve(s, now);
    EXPECT(s->scope_known && !strcmp(s->current, "ABC") && !strcmp(s->container, "PDB1"), "DBX-095: scope = current schema ABC in container PDB1");
    EXPECT(!nsx_busy(s) && scope_queries == 1 && pages_served == 0 && count_queries == 0 && object_queries == 0, "DBX-083 / 093: after connect: 1 query, no page, no count, no object query (no pins yet)");
    nsx_featured(s);
    char lbl[64]; nsx_others_label(s, lbl, sizeof lbl);
    EXPECT(s->nfeatured == 1 && nsx_others_shown(s) && !strcmp(lbl, "Others"), "DBX-077 / 121: Schemas = ABC, then 'Others' without a number (total unknown)");

    nsx_expand_others(s);
    serve(s, now);
    nsx_featured(s);
    const char *on[400]; int k = nsx_others_names(s, on, 400);
    EXPECT(pages_served == 1 && s->pager.n == 100 && s->pager.has_more, "DBX-079 / 084: expanding Others reads one page of 100 names (has more)");
    EXPECT(k == 99 && strcmp(on[0], "ABC") != 0, "DBX-084.3: the featured ABC is not repeated in Others (99 of the page shown)");
    EXPECT(object_queries == 0, "DBX-079.5 / 120.4: expanding Others ran no object query for any candidate");
    nsx_load_more(s); serve(s, now); nsx_load_more(s); serve(s, now);
    EXPECT(s->pager.n == 300 && pages_served == 3 && strstr(last_sql, "username > '"), "DBX-084: Load more continues from the cursor (keyset), 300 names after 3 pages");
    EXPECT(s->pager.n < 1000, "DBX-088 / 120.1: names held = names loaded (%d), not 170,000", s->pager.n);

    /* search far away */
    int before = pages_served; rows_scanned_by_search = 0;
    nsx_search(s, "USR_165000", false, false, "", now);
    EXPECT(!nsx_next_request(s, now + 10), "DBX-119: search is debounced (nothing sent within 250 ms)");
    now += 300; serve(s, now);
    EXPECT(s->search.done && s->search.nhits == 1 && !strcmp(s->search.hits[0].schema, "USR_165000"), "DBX-085: USR_165000 found by the server");
    EXPECT(pages_served == before && rows_scanned_by_search <= 2, "DBX-120.3: the search did not walk 1,650 pages (pages %d, rows scanned %d)", pages_served - before, rows_scanned_by_search);
    EXPECT(nsx_info(s, "USR_165000", false) == NULL || nsx_info(s, "USR_165000", false)->vis != NSV_HAS, "DBX-085.4: a found schema is not marked as having visible objects");

    /* pin the result, then move it back */
    nsx_pin(s, "USR_165000"); drain(s, &now);
    nsx_featured(s);
    EXPECT(nsx_is_featured(s, "USR_165000") && (nsx_featured_flags(s, "USR_165000") & NSF_PINNED) && names_queries == 1, "DBX-081 / 106: Pin to Schemas moves it under Schemas (existence checked once)");
    nsx_unpin(s, "USR_165000"); nsx_featured(s);
    k = nsx_others_names(s, on, 400);
    EXPECT(!nsx_is_featured(s, "USR_165000") && k > 0 && !strcmp(on[0], "USR_165000"), "DBX-106.2: moved to Others beyond the loaded pages: anchored there, no page walk");
    EXPECT(!nsx_can_unpin(s, "ABC"), "DBX-081.2 / 123.2: the current schema cannot be moved out of Schemas");
    int dupes = 0; for (int i = 0; i < k; i++) if (!strcmp(on[i], "ABC")) dupes++;
    EXPECT(dupes == 0, "DBX-077.3: no duplicates between Schemas and Others");

    /* stale answers and timeouts */
    nsx_search(s, "USR_0001", false, false, "", now); now += 300;
    const NsReq *q = nsx_next_request(s, now);
    nsx_search(s, "USR_00012", false, false, "", now);
    EXPECT(q && nsx_running_stale(s), "DBX-119.1: a newer search makes the running one stale (cancel it)");
    ResultSet r = rs2(); row(&r, "USR_000100", "0", "");
    nsx_complete(s, &r, NULL, false, false, 4, now); rs_free(&r);
    EXPECT(s->m.skipped_stale == 1 && !s->search.done, "DBX-119.1: the stale answer was dropped by its generation");
    drain(s, &now);
    EXPECT(s->search.done && s->search.nhits == 10 && !strcmp(s->search.hits[0].schema, "USR_000120"), "the newest search answered (USR_00012*: 10 hits)");
    nsx_search(s, "USR_0", false, false, "", now); drain(s, &now);
    EXPECT(s->search.partial && s->search.nhits == 200, "DBX-085.5: more matches than the limit are flagged partial");
    nsx_search(s, "_1650", true, false, "", now); drain(s, &now);
    EXPECT(s->search.done && s->search.nhits == 100 && strstr(last_sql, "UPPER(username) LIKE '%\\_1650%'"), "DBX-085.2: substring search is a separate, explicit request");

    timeout_kind = NSR_PROBE; nsx_probe(s, "PRIVADO"); drain(s, &now);
    NsInfo *in = nsx_info(s, "PRIVADO", false);
    EXPECT(in && in->vis == NSV_UNKNOWN && strstr(in->note, "Not verified") && s->m.timeouts == 1, "DBX-124.2: a probe that times out leaves UNKNOWN ('Not verified'), no error group");
    nsx_probe(s, "PRIVADO"); drain(s, &now);
    EXPECT(in->vis == NSV_NONE && strstr(nsx_vis_text(in->vis), "not proof"), "DBX-078 / 092: no visible object = 'for this session', never 'empty'");
    nsx_probe(s, "USR_000001"); drain(s, &now);
    EXPECT(nsx_info(s, "USR_000001", false)->vis == NSV_HAS, "DBX-107: Verify Visible Objects finds one");
    fail_kind = NSR_PROBE; fail_err = "ORA-01031: insufficient privileges"; nsx_probe(s, "XPTO"); drain(s, &now);
    in = nsx_info(s, "XPTO", false);
    EXPECT(in->vis == NSV_ERROR && strstr(in->note, "insufficient privileges for this catalog query (ORA-01031)"), "DBX-098: ORA-01031 explained specifically: %s", in->note);
    char hint[200]; nsx_error_hint("ORA-00942: table or view does not exist", hint, sizeof hint);
    EXPECT(strstr(hint, "not accessible to this user") != NULL, "DBX-098: ORA-00942 explained");
    nsx_featured(s);
    EXPECT(nsx_is_featured(s, "USR_000001") && (nsx_featured_flags(s, "USR_000001") & NSF_AUTO), "DBX-086: a schema with objects found by a probe may be auto-highlighted");

    /* tables of a schema (DBX-090) */
    nsx_load_tables(s, "USR_000001"); drain(s, &now);
    in = nsx_info(s, "USR_000001", false);
    EXPECT(in->tables_loaded && in->ntables == 2 && in->tables[1].is_view && !strcmp(in->tables[0].name, "USR_000001.T1"), "DBX-090: expanding a schema loads its table / view names only");

    /* discovery: opt-in, bounded, capped (DBX-093 / 097) */
    int oq = object_queries;
    nsx_set_option(s, "discover", 1); drain(s, &now);
    nsx_featured(s);
    int autos = 0; for (int i = 0; i < s->nfeatured; i++) if (s->featured[i].flags & NSF_AUTO) autos++;
    EXPECT(object_queries == oq + 1 && autos <= 20 && s->discover_done, "DBX-086.2 / 093: opt-in discovery = 1 bounded query; automatic highlights capped at 20 (%d)", autos);
    nsx_unpin(s, "USR_001000"); nsx_featured(s);
    EXPECT(!nsx_is_featured(s, "USR_001000"), "DBX-086.5: Move to Others keeps an auto-highlighted schema out");
    nsx_set_option(s, "discover", 0);

    /* Oracle without ORACLE_MAINTAINED (before 12c): documented fallback */
    fail_kind = NSR_PAGE; fail_err = "ORA-00904: \"ORACLE_MAINTAINED\": invalid identifier";
    nsx_refresh_others(s); drain(s, &now);
    EXPECT(s->sysflag_unavailable && s->pager.n == 300 && strstr(last_sql, "0 AS sys"), "DBX-082.3: ORA-00904 -> retried without ORACLE_MAINTAINED; Refresh Others reloads the 3 listed pages");

    /* scope change: another PDB */
    snprintf(scope_con, sizeof scope_con, "PDB2");
    nsx_refresh(s); drain(s, &now);
    nsx_featured(s);
    EXPECT(!strcmp(s->container, "PDB2") && !nsx_info(s, "XPTO", false) && s->pager.n == 0, "DBX-095 / 110: another container drops classifications and pages");
    EXPECT(s->prefs.nsup >= 1, "DBX-110.2 / 124.3: preferences (suppressed) survive the scope change");
    snprintf(scope_con, sizeof scope_con, "PDB1");

    /* persistence (DBX-112) */
    nsx_pin(s, "CLIENTES"); drain(s, &now);
    nsx_set_mode(s, NSM_PINNED);
    nsx_shutdown();
    nsx_set_dir(dir); nsx_load();
    s = nsx_get("c1", "oracle");
    EXPECT(s && s->prefs.npins == 1 && !strcmp(s->prefs.pins[0], "CLIENTES") && s->prefs.mode == NSM_PINNED, "DBX-112: pins and tree mode persist per connection");
    char path[256]; snprintf(path, sizeof path, "%s/schemas.txt", dir);
    FILE *f = fopen(path, "r"); char filebuf[4096] = ""; if (f) { size_t n = fread(filebuf, 1, sizeof filebuf - 1, f); filebuf[n] = 0; fclose(f); }
    EXPECT(!strstr(filebuf, "password") && !strstr(filebuf, "USR_000001.T1") && strstr(filebuf, "pin=CLIENTES"), "DBX-112.4 / 114: schemas.txt keeps tree preferences only (no secrets, no cached metadata)");
    nsx_connected(s, "", (double)now); drain(s, &now);
    nsx_featured(s);
    EXPECT(nsx_is_featured(s, "CLIENTES") && nsx_is_featured(s, "ABC") && s->nfeatured == 2, "DBX-080: Pinned only = current + pinned, after reconnect");
    nsx_reset_prefs(s); nsx_featured(s);
    EXPECT(s->prefs.npins == 0 && s->prefs.mode == NSM_COMPACT && s->nfeatured == 1, "DBX-108.4: Reset for this connection");

    /* ---- PostgreSQL: exact counts (cheap catalog) ---- */
    cat_build(0, (const char *[]){ "public", "vendas", "rh", "fin", "logs" }, 5, (const char *[]){ "pg_catalog", "information_schema" }, 2);
    snprintf(scope_cur, sizeof scope_cur, "public"); snprintf(scope_user, sizeof scope_user, "app"); snprintf(scope_con, sizeof scope_con, "db1");
    NsState *p = nsx_get("c2", "postgres");
    count_queries = 0;
    nsx_connected(p, "", (double)now); drain(p, &now);
    nsx_featured(p);
    { char d[1024]; nsx_dump(p, d, sizeof d, true); printf("  %s\n", d); }
    EXPECT(count_queries == 1 && p->total_known && p->total == 5, "DBX-121: PostgreSQL counts namespaces (cheap) without system ones: 5");
    EXPECT(p->nfeatured == 5 && !nsx_others_shown(p), "DBX-099.2 / 047: a small catalog is shown whole, no Others (absence proven)");
    nsx_unpin(p, "logs"); nsx_featured(p);
    nsx_others_label(p, lbl, sizeof lbl);
    EXPECT(!nsx_is_featured(p, "logs") && nsx_others_shown(p) && !strcmp(lbl, "Others (1)"), "Move to Others in a small catalog: Others (1)");
    nsx_set_system(p, true); drain(p, &now); nsx_featured(p);
    EXPECT(p->total == 7 && nsx_is_featured(p, "pg_catalog"), "DBX-082.2 / 121.4: Show system schemas changes the scope of the count (7)");
    nsx_set_system(p, false); drain(p, &now);

    cat_build(170000, (const char *[]){ "public" }, 1, NULL, 0);
    NsState *b = nsx_get("c3", "postgres");
    nsx_connected(b, "", (double)now); drain(b, &now);
    nsx_featured(b); nsx_others_label(b, lbl, sizeof lbl);
    EXPECT(!strcmp(lbl, "Others (170,000)"), "DBX-121.1: exact total minus featured: %s", lbl);
    nsx_pin(b, "USR_000042"); drain(b, &now); nsx_featured(b); nsx_others_label(b, lbl, sizeof lbl);
    EXPECT(!strcmp(lbl, "Others (169,999)"), "DBX-106.3 / 121.4: pinning updates the exact count: %s", lbl);
    nsx_pin(b, "NOPE"); nsx_featured(b); nsx_others_label(b, lbl, sizeof lbl);
    EXPECT(!strcmp(lbl, "Others"), "DBX-121.4: a pin not yet checked makes the number unknown (no guess): %s", lbl);
    drain(b, &now); nsx_featured(b); nsx_others_label(b, lbl, sizeof lbl);
    EXPECT(!strcmp(lbl, "Others (169,999)"), "after the check (NOPE does not exist) the count is exact again: %s", lbl);
    fail_kind = NSR_COUNT; fail_err = "canceling statement due to statement timeout";
    nsx_refresh(b); drain(b, &now); nsx_featured(b); nsx_others_label(b, lbl, sizeof lbl);
    EXPECT(!strcmp(lbl, "Others") && nsx_others_shown(b), "DBX-121.2 / 118: a failed count leaves 'Others' without a number");

    /* mode All: unified, paged */
    nsx_set_mode(b, NSM_ALL); drain(b, &now);
    EXPECT(!nsx_others_shown(b) && b->pager.n == 100, "DBX-080 / 104.3: Show All Schemas = one paged list (first page only)");

    /* object search (DBX-087) */
    nsx_search(b, "vw_cli", false, true, "", now); drain(b, &now);
    EXPECT(b->search.nhits == 1 && !strcmp(b->search.hits[0].name, "VW_CLIENTES") && !strcmp(b->search.hits[0].type, "VIEW"), "DBX-087: Find Objects returns schema, type and name");

    /* copy loaded names (DBX-104.4) */
    char cp[200000]; int nc = nsx_copy_loaded(b, cp, sizeof cp);
    EXPECT(nc == b->pager.n + 1 || nc == b->pager.n + 2, "DBX-104.4: Copy Loaded Names copies only loaded names (%d)", nc);

    char dump[2048]; nsx_dump(b, dump, sizeof dump, false);
    EXPECT(!strstr(dump, "USR_") && strstr(dump, "queries="), "DBX-114 / 117: the diagnostics line carries counts, not names");

    nsx_shutdown();
    for (int i = 0; i < ncat; i++) free(cat[i]);
    free(cat); free(catsys);
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}
    printf("NSX: %s (%d ok, %d failures)\n", fails ? "FAILED" : "passed", oks, fails);
    return fails ? 1 : 0;
}
