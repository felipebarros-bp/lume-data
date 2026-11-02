/* SPDX-License-Identifier: AGPL-3.0-only */
/* Object source editor core, headless (OBJ-077): lexer / formatter golden cases per dialect,
 * line diff and three-way conflicts, CREATE header parsing, PostgreSQL identity signatures, the
 * migration planner per engine (AT-01 / 02 / 03 / 04 / 06 / 10 rules) and local diagnostics with
 * quick fixes. No database. */
#include "objdiag.h"
#include "objengine.h"
#include "objlang.h"
#include "objplan_int.h"
#include "objmodel.h"
#include "sqlfmt.h"
#include "sqllex.h"
#include "textdiff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, oks;
#define CHECK(cond, ...) do { if (cond) { oks++; printf("ok    "); } else { fails++; printf("FAIL  "); } printf(__VA_ARGS__); printf("\n"); } while (0)

static ObjKey key(int kind, const char *schema, const char *name, const char *sig) {
    ObjKey k; memset(&k, 0, sizeof k);
    snprintf(k.conn_id, sizeof k.conn_id, "c1"); snprintf(k.schema, sizeof k.schema, "%s", schema);
    snprintf(k.name, sizeof k.name, "%s", name); k.kind = kind; snprintf(k.signature, sizeof k.signature, "%s", sig ? sig : "");
    snprintf(k.native_id, sizeof k.native_id, "16384");
    return k;
}
static ObjSnapshot snap(const char *ddl) {
    ObjSnapshot s; memset(&s, 0, sizeof s);
    s.original = strdup(ddl); s.ddl = strdup(ddl); s.exists = true; s.deps_known = true;
    s.fingerprint = obj_hash(ddl, -1);
    return s;
}
static bool plan_has(const ObjPlan *p, const char *needle) {
    for (int i = 0; i < p->nsql; i++) if (strstr(p->sql[i], needle)) return true;
    return false;
}
static ObjPlan plan(int d, const ObjKey *k, const ObjSnapshot *base, const ObjSnapshot *remote, const char *local, bool destructive) {
    ObjPlanInput in; memset(&in, 0, sizeof in);
    in.dialect = d; in.key = k; in.base = base; in.remote = remote; in.local = local; in.local_len = (int)strlen(local);
    in.allow_destructive = destructive; in.has_driver = true;   /* the core ANDs it with the adapter's own has_driver */
    in.server_version = d == DIALECT_PG ? 160000 : 0;
    ObjPlan p; objplan_make(&in, &p);
    return p;
}

static void test_format(void) {
    SqlStyle st; sql_style_default(&st); FmtResult r;
    const char *q = "select a, b from t join u on t.id = u.id where a > 1 order by b";
    sql_format(q, (int)strlen(q), DIALECT_PG, &st, -1, -1, &r);
    CHECK(r.changed && strstr(r.text, "SELECT a, b\nFROM t\nJOIN u ON t.id = u.id\nWHERE a > 1\nORDER BY b"), "format: one clause per line, keywords upper case");
    fmt_free(&r);
    const char *f = "create or replace function f(a int) returns int as $$ begin  return a+1; end $$ language plpgsql;";
    sql_format(f, (int)strlen(f), DIALECT_PG, &st, -1, -1, &r);
    CHECK(strstr(r.text, "$$ begin  return a+1; end $$") != NULL && strstr(r.note, "kept as written"), "format: PL/pgSQL $$ body copied verbatim, the note says so");
    fmt_free(&r);
    const char *o = "CREATE OR REPLACE PACKAGE BODY p AS PROCEDURE x IS BEGIN NULL; END; END p;";
    sql_format(o, (int)strlen(o), DIALECT_ORACLE, &st, -1, -1, &r);
    CHECK(r.refused && !strcmp(r.text, o), "format: Oracle PL/SQL block refused, text unchanged (no reliable parser)");
    fmt_free(&r);
    const char *dyn = "select 'select * from t where x = 1' as sql_text";
    sql_format(dyn, (int)strlen(dyn), DIALECT_PG, &st, -1, -1, &r);
    CHECK(strstr(r.text, "'select * from t where x = 1'") != NULL, "format: dynamic SQL inside a string is never touched");
    fmt_free(&r);
    const char *two = "select 1;\nselect   a from t;\n";
    sql_format(two, (int)strlen(two), DIALECT_SQLITE, &st, 12, 14, &r);
    CHECK(!strncmp(r.text, "select 1;", 9) && strstr(r.text, "SELECT a\nFROM t;") && strstr(r.note, "widened"), "format selection: widened to the statement, the rest untouched");
    fmt_free(&r);
    const char *ms = "create view v as select [a b] from dbo.t\nGO\n";
    sql_format(ms, (int)strlen(ms), DIALECT_MSSQL, &st, -1, -1, &r);
    CHECK(strstr(r.text, "[a b]") && sql_same_tokens(ms, (int)strlen(ms), r.text, r.len, DIALECT_MSSQL), "format: T-SQL [ident] kept, token-equivalent");
    fmt_free(&r);
    const char *my = "select `order`, \"txt\" from t # comment\nwhere a = 1";
    sql_format(my, (int)strlen(my), DIALECT_MYSQL, &st, -1, -1, &r);
    CHECK(strstr(r.text, "# comment\n") && strstr(r.text, "`order`") && !r.refused, "format: MySQL backticks and # comments survive (line comment keeps its line end)");
    fmt_free(&r);
}

static void test_diff(void) {
    const char *a = "a\nb\nc\nd\n", *b = "a\nB\nc\nd\ne\n";
    TextLines A, B; lines_split(&A, a, (int)strlen(a)); lines_split(&B, b, (int)strlen(b));
    Diff df; text_diff(&A, &B, false, 0, &df);
    CHECK(df.added == 2 && df.removed == 1, "diff: one line changed, one appended (+%d -%d)", df.added, df.removed);
    char *u = diff_unified(&A, &B, &df, -1);
    CHECK(strstr(u, "-b\n+B\n") && strstr(u, "+e\n"), "diff: unified text");
    free(u); diff_free(&df);
    const char *base = "1\n2\n3\n4\n5\n", *loc = "1\nX\n3\n4\n5\n", *rem = "1\nY\n3\n4\nZ\n";
    TextLines L0, L1, L2; lines_split(&L0, base, (int)strlen(base)); lines_split(&L1, loc, (int)strlen(loc)); lines_split(&L2, rem, (int)strlen(rem));
    bool cf[8] = { 0 };
    int c = diff3_conflicts(&L0, &L1, &L2, cf, 0);
    CHECK(c == 1 && cf[1] && !cf[4], "three-way: only the line both sides changed conflicts (%d)", c);
    lines_free(&A); lines_free(&B); lines_free(&L0); lines_free(&L1); lines_free(&L2);
    /* budget: a pathological diff stops and says so */
    char *x = malloc(40000), *y = malloc(40000); int nx = 0, ny = 0;
    for (int i = 0; i < 4000; i++) { nx += snprintf(x + nx, 40000 - (size_t)nx, "a%d\n", i); ny += snprintf(y + ny, 40000 - (size_t)ny, "b%d\n", i); }
    lines_split(&A, x, nx); lines_split(&B, y, ny);
    bool ok = text_diff(&A, &B, false, 100, &df);
    CHECK(!ok && df.too_large, "diff: budget exceeded on a completely different text -> too_large, no wrong result");
    lines_free(&A); lines_free(&B); free(x); free(y);
}

static void test_header(void) {
    ObjHeader h;
    const char *v = "-- note\nCREATE OR REPLACE VIEW public.v_rev (id, amount) WITH (security_barrier) AS SELECT id, amount FROM sales;";
    obj_parse_header(v, (int)strlen(v), DIALECT_PG, &h);
    CHECK(h.found && h.kind == OBJK_VIEW && h.or_replace && !strcmp(h.schema, "public") && !strcmp(h.name, "v_rev") && h.select_start > 0 && !strncmp(v + h.select_start, "SELECT", 6) && h.extra_statements == 0,
          "header: PG view, schema, name, query after AS");
    const char *f = "CREATE FUNCTION \"Calc\"(a integer DEFAULT 1, OUT b text) RETURNS SETOF record LANGUAGE sql AS $$ select 1 $$;";
    obj_parse_header(f, (int)strlen(f), DIALECT_PG, &h);
    CHECK(h.kind == OBJK_FUNCTION && !strcmp(h.name, "Calc") && !strcmp(h.returns, "SETOF record") && h.args_start > 0, "header: quoted function name kept, RETURNS read (%s)", h.returns);
    const char *pk = "create or replace package body billing.pkg_billing as\n procedure p is begin null; end;\nend pkg_billing;\n/\n";
    obj_parse_header(pk, (int)strlen(pk), DIALECT_ORACLE, &h);
    CHECK(h.kind == OBJK_PKG_BODY && !strcmp(h.name, "PKG_BILLING") && !strcmp(h.schema, "BILLING") && h.extra_statements == 0 && pk[h.body_end - 1] == ';',
          "header: Oracle package body, names folded upper case, unit ends at END ...; before /");
    const char *t = "CREATE TRIGGER trg AFTER INSERT ON main.orders BEGIN UPDATE stats SET n = n + 1; END;";
    obj_parse_header(t, (int)strlen(t), DIALECT_SQLITE, &h);
    CHECK(h.kind == OBJK_TRIGGER && !strcmp(h.table, "main.orders") && h.extra_statements == 0, "header: SQLite trigger ON table, BEGIN ... END body is one unit");
    const char *two = "CREATE VIEW v AS SELECT 1; DROP TABLE t; SELECT 2;";
    obj_parse_header(two, (int)strlen(two), DIALECT_SQLITE, &h);
    CHECK(h.extra_statements == 2, "header: two extra statements after a view are counted (%d)", h.extra_statements);
    const char *ms = "CREATE OR ALTER PROCEDURE dbo.usp_x @a int AS BEGIN SELECT @a; END\nGO\n";
    obj_parse_header(ms, (int)strlen(ms), DIALECT_MSSQL, &h);
    CHECK(h.or_alter && h.kind == OBJK_PROCEDURE && !strcmp(h.schema, "dbo") && h.extra_statements == 0, "header: T-SQL CREATE OR ALTER, GO ends the batch");
    const char *my = "CREATE DEFINER=`app`@`%` PROCEDURE shop.p_upd(IN x INT) BEGIN UPDATE t SET a = x WHERE id = 1; END";
    obj_parse_header(my, (int)strlen(my), DIALECT_MYSQL, &h);
    CHECK(h.found && h.kind == OBJK_PROCEDURE && !strcmp(h.name, "p_upd") && !strcmp(h.schema, "shop"), "header: MySQL DEFINER clause skipped");
}

static void test_signature(void) {
    char o[300];
    { const char *a1 = "order_id int, OUT total numeric(10,2), p_date date DEFAULT now(), VARIADIC tags text[]"; pg_identity_types(a1, (int)strlen(a1), false, o, sizeof o); }
    CHECK(!strcmp(o, "integer, date, text[]"), "PG identity: names, OUT, defaults and typmods dropped -> %s", o);
    { const char *a2 = "double precision, character varying(20), \"date\" date"; pg_identity_types(a2, (int)strlen(a2), false, o, sizeof o); }
    CHECK(!strcmp(o, "double precision, character varying, date"), "PG identity: multi-word types without names; a parameter called date -> %s", o);
    { const char *a3 = "IN a int4, OUT b int8"; pg_identity_types(a3, (int)strlen(a3), true, o, sizeof o); }
    CHECK(!strcmp(o, "integer, bigint"), "PG identity: procedures keep OUT arguments -> %s", o);
    pg_norm_type("timestamptz", 11, o, sizeof o);
    CHECK(!strcmp(o, "timestamp with time zone"), "PG type alias timestamptz -> %s", o);
}

static void test_planner(void) {
    /* AT-01: SQLite view -> BEGIN, DROP VIEW, CREATE VIEW, COMMIT */
    ObjKey kv = key(OBJK_VIEW, "main", "v_revenue", NULL);
    ObjSnapshot sv = snap("CREATE VIEW v_revenue AS SELECT id, amount FROM sales");
    sv.nrecreate = 1; sv.recreate = malloc(sizeof(char *)); sv.recreate[0] = strdup("CREATE TRIGGER v_rev_ins INSTEAD OF INSERT ON v_revenue BEGIN SELECT 1; END");
    ObjPlan p = plan(DIALECT_SQLITE, &kv, &sv, &sv, "CREATE VIEW v_revenue AS SELECT id, amount * 2 AS amount FROM sales;", false);
    CHECK(!p.blocked && p.nsql == 6 && !strcmp(p.sql[0], "BEGIN") && !strcmp(p.sql[1], "DROP VIEW \"main\".\"v_revenue\"") && p.create_index == 2 && plan_has(&p, "INSTEAD OF") && plan_has(&p, "pragma_table_info") && !strcmp(p.sql[5], "COMMIT") && p.atomicity == ATOM_TRANSACTIONAL,
          "AT-01 planner: SQLite view = BEGIN / DROP / CREATE / recreate INSTEAD OF trigger / COMMIT (%s)", p.block_reason);
    CHECK(p.sql[2][strlen(p.sql[2]) - 1] != ';', "planner: the CREATE sent to the server has no trailing ';'");
    char *script = objplan_script(&p, DIALECT_SQLITE);
    CHECK(strstr(script, "DROP VIEW \"main\".\"v_revenue\";\n") != NULL, "planner: exportable script joins statements with ';'");
    free(script); objplan_free(&p);
    p = plan(DIALECT_SQLITE, &kv, &sv, &sv, "CREATE OR REPLACE VIEW v_revenue AS SELECT 1;", false);
    CHECK(p.blocked && strstr(p.block_reason, "no CREATE OR REPLACE"), "planner: SQLite OR REPLACE refused with the reason");
    objplan_free(&p);
    p = plan(DIALECT_SQLITE, &kv, &sv, &sv, "CREATE VIEW v_revenue AS SELECT 1; DELETE FROM sales;", false);
    CHECK(p.blocked && strstr(p.block_reason, "more statement"), "planner: a DELETE smuggled after the view is refused");
    objplan_free(&p);
    p = plan(DIALECT_SQLITE, &kv, &sv, &sv, "CREATE VIEW v_other AS SELECT 1;", false);
    CHECK(p.blocked && strstr(p.block_reason, "Renaming"), "planner (OBJ-068): another name is not an edit");
    objplan_free(&p);
    p = plan(DIALECT_SQLITE, &kv, &sv, &sv, "SELECT id, amount * 2 FROM sales", false);
    CHECK(p.blocked && strstr(p.block_reason, "incomplete DDL"), "planner (OBJ-069): a bare query is incomplete DDL");
    objplan_free(&p);
    p = plan(DIALECT_SQLITE, &kv, &sv, &sv, "CREATE VIEW v_revenue AS SELECT id, amount FROM sales;\n", false);
    CHECK(p.blocked && p.no_changes, "planner: no changes -> nothing to apply");
    objplan_free(&p);

    /* AT-04: remote changed */
    ObjSnapshot remote = snap("CREATE VIEW v_revenue AS SELECT id FROM sales");
    p = plan(DIALECT_SQLITE, &kv, &sv, &remote, "CREATE VIEW v_revenue AS SELECT 2 AS id;", false);
    CHECK(p.blocked && p.conflict, "AT-04 planner: remote fingerprint differs -> conflict, nothing runs");
    objplan_free(&p);
    remote.exists = false;
    p = plan(DIALECT_SQLITE, &kv, &sv, &remote, "CREATE VIEW v_revenue AS SELECT 2 AS id;", false);
    CHECK(p.blocked && p.conflict && strstr(p.block_reason, "no longer exists"), "planner: dropped elsewhere -> conflict");
    objplan_free(&p); objsnap_free(&remote);

    /* AT-10: read-only */
    ObjPlanInput in; memset(&in, 0, sizeof in);
    const char *loc = "CREATE VIEW v_revenue AS SELECT 3 AS id;";
    in.dialect = DIALECT_SQLITE; in.key = &kv; in.base = &sv; in.local = loc; in.local_len = (int)strlen(loc); in.has_driver = true; in.read_only = true; in.ro_reason = "marked read-only in the connection";
    objplan_make(&in, &p);
    CHECK(p.blocked && strstr(p.block_reason, "read-only") && p.nsql == 6, "AT-10 planner: read-only blocks Apply but still builds the script for export");
    objplan_free(&p);
    objsnap_free(&sv);

    /* AT-02: PG view contract */
    ObjKey kp = key(OBJK_VIEW, "public", "v_sales", NULL);
    ObjSnapshot sp = snap("CREATE OR REPLACE VIEW public.v_sales AS\n SELECT id, amount FROM sales;");
    ObjColumn before[2] = { { "id", "integer" }, { "amount", "numeric(12,2)" } };
    sp.cols = malloc(sizeof before); memcpy(sp.cols, before, sizeof before); sp.ncols = 2;
    ObjColumn changed[2] = { { "id", "integer" }, { "amount", "text" } };
    const char *lv = "CREATE OR REPLACE VIEW public.v_sales AS SELECT id, amount::text AS amount FROM sales;";
    memset(&in, 0, sizeof in);
    in.dialect = DIALECT_PG; in.key = &kp; in.base = &sp; in.remote = &sp; in.local = lv; in.local_len = (int)strlen(lv); in.has_driver = true; in.new_cols = changed; in.n_new_cols = 2;
    objplan_make(&in, &p);
    CHECK(p.blocked && p.destructive_available && strstr(p.block_reason, "changes type") && !plan_has(&p, "CASCADE"),
          "AT-02 planner: type change -> no simple CREATE OR REPLACE; explicit drop + create offered (%s)", p.block_reason);
    objplan_free(&p);
    in.allow_destructive = true; objplan_make(&in, &p);
    CHECK(!p.blocked && p.destructive && p.risk == RISK_DESTRUCTIVE && plan_has(&p, "DROP VIEW \"public\".\"v_sales\"") && !plan_has(&p, "CASCADE"),
          "AT-02 planner: on explicit request, DROP VIEW (never CASCADE) + CREATE in one transaction, risk destructive");
    objplan_free(&p);
    sp.ndeps = 1; sp.deps = calloc(1, sizeof(ObjDep)); snprintf(sp.deps[0].name, sizeof sp.deps[0].name, "public.v_report"); snprintf(sp.deps[0].how, sizeof sp.deps[0].how, "view");
    objplan_make(&in, &p);
    CHECK(p.blocked && strstr(p.block_reason, "CASCADE"), "planner: dependents block the drop (CASCADE is never used)");
    objplan_free(&p);
    ObjColumn appended[3] = { { "id", "int4" }, { "amount", "numeric" }, { "note", "text" } };
    in.allow_destructive = false; in.new_cols = appended; in.n_new_cols = 3;
    objplan_make(&in, &p);
    CHECK(!p.blocked && p.nsql == 3 && strstr(p.strategy, "CREATE OR REPLACE") && p.risk == RISK_NON_DESTRUCTIVE, "planner: a column appended at the end keeps the contract -> CREATE OR REPLACE");
    objplan_free(&p);
    objsnap_free(&sp);

    /* AT-03: PG overloads */
    ObjKey kf = key(OBJK_FUNCTION, "public", "calculate", "integer");
    ObjSnapshot sf = snap("CREATE OR REPLACE FUNCTION public.calculate(p integer)\n RETURNS numeric\n LANGUAGE sql\nAS $function$ select p * 2.0 $function$\n");
    snprintf(sf.result_type, sizeof sf.result_type, "numeric");
    p = plan(DIALECT_PG, &kf, &sf, &sf, "CREATE OR REPLACE FUNCTION public.calculate(p int) RETURNS numeric LANGUAGE sql AS $function$ select p * 3.0 $function$;", false);
    CHECK(!p.blocked && strstr(p.probe_signature, "\"public\".\"calculate\"(integer)") && p.risk == RISK_NON_DESTRUCTIVE, "AT-03 planner: same identity (int == integer) -> replace; probe %s", p.probe_signature);
    objplan_free(&p);
    p = plan(DIALECT_PG, &kf, &sf, &sf, "CREATE OR REPLACE FUNCTION public.calculate(p text) RETURNS numeric LANGUAGE sql AS $function$ select 1 $function$;", false);
    CHECK(p.blocked && strstr(p.block_reason, "different overload"), "AT-03 planner: changing the argument type would hit calculate(text) / create a new overload -> refused");
    objplan_free(&p);
    p = plan(DIALECT_PG, &kf, &sf, &sf, "CREATE OR REPLACE FUNCTION public.calculate(p integer) RETURNS text LANGUAGE sql AS $function$ select '1' $function$;", false);
    CHECK(p.blocked && p.destructive_available && strstr(p.block_reason, "return type"), "planner: PG return type change needs the explicit drop + create");
    objplan_free(&p);
    p = plan(DIALECT_PG, &kf, &sf, &sf, "CREATE OR REPLACE FUNCTION public.calculate(p integer) RETURNS text LANGUAGE sql AS $function$ select '1' $function$;", true);
    CHECK(!p.blocked && plan_has(&p, "DROP FUNCTION \"public\".\"calculate\"(integer)"), "planner: explicit DROP FUNCTION names the exact signature");
    objplan_free(&p);
    objsnap_free(&sf);

    /* PG trigger before 14 */
    ObjKey kt = key(OBJK_TRIGGER, "public", "trg_audit", NULL); snprintf(kt.table, sizeof kt.table, "public.orders");
    ObjSnapshot st = snap("CREATE TRIGGER trg_audit AFTER INSERT ON public.orders FOR EACH ROW EXECUTE FUNCTION audit()");
    memset(&in, 0, sizeof in);
    const char *lt = "CREATE TRIGGER trg_audit AFTER INSERT OR UPDATE ON public.orders FOR EACH ROW EXECUTE FUNCTION audit();";
    in.dialect = DIALECT_PG; in.key = &kt; in.base = &st; in.local = lt; in.local_len = (int)strlen(lt); in.has_driver = true; in.server_version = 130000;
    objplan_make(&in, &p);
    CHECK(!p.blocked && plan_has(&p, "DROP TRIGGER \"trg_audit\" ON \"public\".\"orders\"") && p.atomicity == ATOM_TRANSACTIONAL, "planner: PG 13 trigger = DROP TRIGGER ... ON table + CREATE in one transaction");
    objplan_free(&p); objsnap_free(&st);

    /* AT-05 (planner side): Oracle package body */
    ObjKey ko = key(OBJK_PKG_BODY, "BILLING", "PKG_BILLING", NULL);
    ObjSnapshot so = snap("CREATE OR REPLACE PACKAGE BODY BILLING.PKG_BILLING AS\nPROCEDURE p IS BEGIN NULL; END;\nEND;");
    p = plan(DIALECT_ORACLE, &ko, &so, &so, "create or replace package body billing.pkg_billing as\nprocedure p is begin x := 1; end;\nend;\n/\n", false);
    CHECK(!p.blocked && p.nsql == 1 && p.atomicity == ATOM_AUTOCOMMIT && strstr(p.sql[0], "end;") && !strstr(p.sql[0], "/") && p.npre > 0 && strstr(p.pre[p.npre - 1], "VALID"),
          "AT-05 planner: Oracle CREATE OR REPLACE, '/' dropped, status read after (never assumed)");
    char *os = objplan_script(&p, DIALECT_ORACLE);
    CHECK(strstr(os, "end;\n/\n") != NULL, "planner: Oracle script puts '/' after a PL/SQL unit");
    free(os); objplan_free(&p);
    memset(&in, 0, sizeof in);
    const char *lo = "CREATE OR REPLACE PACKAGE BODY BILLING.PKG_BILLING AS\nPROCEDURE p IS BEGIN NULL; NULL; END;\nEND;";
    in.dialect = DIALECT_ORACLE; in.key = &ko; in.base = &so; in.local = lo; in.local_len = (int)strlen(lo); in.has_driver = true; in.tx_open = true;
    objplan_make(&in, &p);
    CHECK(p.blocked && strstr(p.block_reason, "open transaction"), "planner (OBJ-039): Oracle DDL refused while the session has an open transaction");
    objplan_free(&p); objsnap_free(&so);

    /* AT-06: MySQL procedure */
    ObjKey km = key(OBJK_PROCEDURE, "shop", "p_upd", NULL);
    ObjSnapshot sm = snap("CREATE PROCEDURE shop.p_upd(IN x INT) BEGIN UPDATE t SET a = x WHERE id = 1; END");
    p = plan(DIALECT_MYSQL, &km, &sm, NULL, "CREATE PROCEDURE shop.p_upd(IN x INT) BEGIN UPDATE t SET a = x + 1 WHERE id = 1; END", false);
    bool definer = false; for (int i = 0; i < p.nimpact; i++) if (strstr(p.impact[i], "DEFINER")) definer = true;
    CHECK(p.blocked && p.destructive_available && definer && !p.script_only, "AT-06 planner: MySQL body change = DROP + CREATE, warns definer / grants / downtime, needs explicit consent (runnable: the MySQL driver exists)");
    objplan_free(&p);
    p = plan(DIALECT_MYSQL, &km, &sm, NULL, "CREATE PROCEDURE shop.p_upd(IN x INT) BEGIN UPDATE t SET a = x + 1 WHERE id = 1; END", true);
    char *ms = objplan_script(&p, DIALECT_MYSQL);
    CHECK(p.destructive && strstr(ms, "DROP PROCEDURE `shop`.`p_upd`;") && strstr(ms, "DELIMITER $$"), "planner: MySQL script with DELIMITER around the routine");
    free(ms); objplan_free(&p);
    p = plan(DIALECT_MARIADB, &km, &sm, NULL, "CREATE OR REPLACE PROCEDURE shop.p_upd(IN x INT) BEGIN UPDATE t SET a = x + 2 WHERE id = 1; END", false);
    CHECK(!p.destructive && strstr(p.strategy, "MariaDB"), "planner (OBJ-054): MariaDB CREATE OR REPLACE is not treated as MySQL");
    objplan_free(&p); objsnap_free(&sm);

    /* SQL Server */
    ObjKey kq = key(OBJK_VIEW, "dbo", "v_x", NULL);
    ObjSnapshot sq = snap("CREATE VIEW dbo.v_x AS SELECT 1 AS a");
    p = plan(DIALECT_MSSQL, &kq, &sq, NULL, "CREATE OR ALTER VIEW dbo.v_x AS SELECT 2 AS a\nGO\n", false);
    char *qs = objplan_script(&p, DIALECT_MSSQL);
    CHECK(!p.script_only && strstr(p.strategy, "CREATE OR ALTER") && strstr(qs, "\nGO\n") && !strstr(p.sql[0], "GO"), "planner (OBJ-052): SQL Server CREATE OR ALTER (runnable: db_mssql), GO only in the exported script");
    free(qs); objplan_free(&p); objsnap_free(&sq);

    /* unsupported kind */
    ObjKey kx = key(OBJK_FUNCTION, "main", "f", NULL);
    p = plan(DIALECT_SQLITE, &kx, NULL, NULL, "CREATE FUNCTION f() ...", false);
    CHECK(p.blocked && strstr(p.block_reason, "not supported"), "planner: SQLite has no stored functions -> no fictional action");
    objplan_free(&p);
}

static bool has_code(const ObjDiag *d, int n, const char *code) { for (int i = 0; i < n; i++) if (!strcmp(d[i].code, code)) return true; return false; }
static const ObjDiag *find(const ObjDiag *d, int n, const char *code) { for (int i = 0; i < n; i++) if (!strcmp(d[i].code, code)) return &d[i]; return NULL; }

static void test_diag(void) {
    ObjDiag d[64]; ObjDiagCtx c; memset(&c, 0, sizeof c);
    ObjKey kv = key(OBJK_VIEW, "main", "v_revenue", NULL);
    c.dialect = DIALECT_SQLITE; c.key = &kv;
    const char *t1 = "CREATE OR REPLACE VIEW v_revenue AS SELECT * FROM sales WHERE note = 'open";
    int n = obj_diagnose(t1, (int)strlen(t1), &c, d, 64);
    const ObjDiag *u = find(d, n, "LEX001");
    CHECK(u && u->severity == DIAG_ERROR && u->line == 1 && u->col == (int)(strchr(t1, '\'') - t1) + 1, "diag: unterminated string with line / column (%d:%d)", u ? u->line : 0, u ? u->col : 0);
    CHECK(has_code(d, n, "DLC001") && has_code(d, n, "INS001"), "diag: SQLite OR REPLACE error + SELECT * inspection");
    const ObjDiag *f = find(d, n, "DLC001"); int fl = 0; char *fixed = f ? obj_apply_fix(t1, (int)strlen(t1), f, &fl) : NULL;
    CHECK(fixed && !strncmp(fixed, "CREATE VIEW v_revenue", 21), "quick fix (OBJ-028): 'Use CREATE' rewrites only the verb");
    free(fixed);
    CHECK(d[0].severity == DIAG_ERROR && d[n - 1].severity >= d[0].severity, "diag: errors listed first");
    const char *t2 = "CREATE VIEW v_rev AS SELECT id FROM sales;";
    n = obj_diagnose(t2, (int)strlen(t2), &c, d, 64);
    f = find(d, n, "HDR003"); fixed = f ? obj_apply_fix(t2, (int)strlen(t2), f, &fl) : NULL;
    CHECK(fixed && strstr(fixed, "CREATE VIEW v_revenue AS"), "quick fix: restore the object name");
    free(fixed);
    const char *known[] = { "main.sales", "orders" };
    c.known = known; c.nknown = 2;
    const char *t3 = "CREATE VIEW v_revenue AS WITH x AS (SELECT 1) SELECT s.id FROM sales s JOIN missing_tbl m ON m.id = s.id JOIN x ON 1 = 1";
    n = obj_diagnose(t3, (int)strlen(t3), &c, d, 64);
    const ObjDiag *r = find(d, n, "REF001");
    CHECK(r && strstr(r->message, "missing_tbl") && r->source == DSRC_METADATA && r->confidence == 2 && n == 1, "diag (OBJ-027): unknown table from the catalog, CTE and known tables not reported (%d)", n);
    c.known = NULL; c.nknown = 0;
    /* PG function body */
    ObjKey kf = key(OBJK_FUNCTION, "public", "calc_total", "integer");
    c.dialect = DIALECT_PG; c.key = &kf;
    const char *cols[] = { "public.orders.order_id", "public.orders.total" };
    c.columns = cols; c.ncolumns = 2;
    const char *t4 = "CREATE OR REPLACE FUNCTION public.calc_total(order_id int) RETURNS numeric AS $$\nBEGIN\n  DELETE FROM orders;\n  EXECUTE 'select 1';\n  RETURN (SELECT total FROM orders o WHERE o.order_id = order_id);\n  IF true THEN NULL;\nEND;\n$$ LANGUAGE plpgsql;";
    n = obj_diagnose(t4, (int)strlen(t4), &c, d, 64);
    const ObjDiag *dw = find(d, n, "INS002"), *dy = find(d, n, "INS003"), *sh = find(d, n, "INS004"), *bl = find(d, n, "BLK003");
    CHECK(dw && dw->line == 3, "diag (OBJ-029): DELETE without WHERE inside the $$ body, mapped to line %d", dw ? dw->line : 0);
    CHECK(dy && dy->severity == DIAG_INFO, "diag (OBJ-032): dynamic SQL reported as not analysed");
    CHECK(sh != NULL, "diag (OBJ-026): parameter order_id shadows orders.order_id");
    CHECK(bl != NULL, "diag: IF without END IF inside PL/pgSQL");
    const char *t5 = "CREATE OR REPLACE FUNCTION public.calc_total(order_id text) RETURNS numeric AS $$ select 1 $$ LANGUAGE sql;";
    n = obj_diagnose(t5, (int)strlen(t5), &c, d, 64);
    CHECK(has_code(d, n, "PG001"), "diag: argument types differ from the function being edited (overload)");
    c.columns = NULL; c.ncolumns = 0;
    /* Oracle */
    ObjKey ko = key(OBJK_PROCEDURE, "HR", "P_RAISE", NULL);
    c.dialect = DIALECT_ORACLE; c.key = &ko;
    const char *t6 = "CREATE PROCEDURE hr.p_raise IS BEGIN EXECUTE IMMEDIATE 'truncate table t'; UPDATE emp SET sal = sal * 2;";
    n = obj_diagnose(t6, (int)strlen(t6), &c, d, 64);
    CHECK(has_code(d, n, "DLC002") && has_code(d, n, "BLK001") && has_code(d, n, "INS002") && has_code(d, n, "INS003"), "diag: Oracle CREATE without OR REPLACE, BEGIN without END, UPDATE without WHERE, EXECUTE IMMEDIATE");
    /* trigger events are not statements */
    ObjKey kt = key(OBJK_TRIGGER, "main", "trg", NULL);
    c.dialect = DIALECT_SQLITE; c.key = &kt;
    const char *t7 = "CREATE TRIGGER trg AFTER UPDATE OF a ON t BEGIN UPDATE u SET n = 1 WHERE id = new.id; END;";
    n = obj_diagnose(t7, (int)strlen(t7), &c, d, 64);
    CHECK(n == 0, "diag: 'AFTER UPDATE OF a ON t' is an event, a clean trigger has no findings (%d: %s)", n, n ? d[0].message : "");
}


/* OBJ-080: a database and a language added from outside the core. */
static ObjCaps duck_caps(int k, int v) { ObjCaps c; memset(&c, 0, sizeof c); if (k == OBJK_VIEW) { c.available = c.source_edit = c.source_read = c.format = true; c.replace_in_place = true; } return c; }
static int duck_plan_calls;
static void duck_plan(const ObjPlanInput *in, const ObjHeader *h, int cs, int ce, ObjPlan *p) {
    duck_plan_calls++;
    snprintf(p->strategy, sizeof p->strategy, "CREATE OR REPLACE VIEW (duck)");
    p->atomicity = ATOM_TRANSACTIONAL; p->risk = RISK_NON_DESTRUCTIVE;
    plan_create(p, in->local + cs, ce - cs);
}
static bool md_handles(int d, const char *ext) { return ext && !strcmp(ext, "md"); }
static int md_lex(const char *t, int len, int d, SqlTok *out, int max) { if (max < 1) return 0; out[0] = (SqlTok){ TK_OTHER, 0, len, false }; return 1; }

static void test_extensibility(void) {
    SqlLexRules rules; memset(&rules, 0, sizeof rules); rules.dollar_bodies = true; rules.backtick_ident = true;
    int dd = dialect_register("DuckDB", "duckdb", &rules);
    CHECK(dd >= DIALECT_COUNT && !strcmp(dialect_name(dd), "DuckDB") && dialect_for_driver("duckdb") == dd, "extension: a new dialect registers its lexical rules (id %d)", dd);
    SqlTok t[8]; const char *src = "select $x$ a;b $x$";
    int n = sql_lex(src, (int)strlen(src), dd, t, 8);
    bool body = false; for (int i = 0; i < n; i++) if (t[i].kind == TK_BODY) body = true;
    CHECK(body, "extension: the lexer applies the new dialect's rules ($tag$ bodies)");
    ObjEngine bad = { .api_version = OBJ_ENGINE_API + 1, .id = "duckdb", .dialect = dd, .caps = duck_caps, .plan = duck_plan };
    CHECK(!obj_engine_register(&bad), "extension: an adapter built for another API version is refused");
    static ObjEngine duck; duck = (ObjEngine){ .api_version = OBJ_ENGINE_API, .id = "duckdb", .dialect = dd, .has_driver = true, .caps = duck_caps, .plan = duck_plan };
    CHECK(obj_engine_register(&duck) && obj_engine_for_driver("duckdb") == &duck && objcaps_for(dd, OBJK_VIEW).replace_in_place, "extension: adapter registered, capabilities come from it");
    ObjKey k = key(OBJK_VIEW, "main", "v", NULL);
    ObjSnapshot b = snap("CREATE OR REPLACE VIEW v AS SELECT 1");
    ObjPlan p = plan(dd, &k, &b, &b, "CREATE OR REPLACE VIEW main.v AS SELECT 2; DROP TABLE t;", false);
    CHECK(p.blocked && duck_plan_calls == 0, "extension: core checks (extra statements) run before the adapter is consulted");
    objplan_free(&p);
    p = plan(dd, &k, &b, &b, "CREATE OR REPLACE VIEW main.v AS SELECT 2", false);
    if (p.blocked) printf("      reason: %s\n", p.block_reason);
    CHECK(!p.blocked && duck_plan_calls == 1 && strstr(p.strategy, "duck") && p.create_index == 0, "extension: the planner delegates to the registered adapter");
    objplan_free(&p);
    ObjKey kf = key(OBJK_FUNCTION, "main", "f", NULL);
    p = plan(dd, &kf, NULL, NULL, "CREATE FUNCTION f() AS 1", false);
    CHECK(p.blocked && strstr(p.block_reason, "not supported"), "extension: a kind the adapter does not declare gets no action");
    objplan_free(&p); objsnap_free(&b);
    static const LangService md = { LANG_API, "lume.markdown", "Markdown", md_handles, md_lex, NULL, NULL };
    static const LangService md_old = { LANG_API + 1, "lume.markdown.old", "Markdown", md_handles, md_lex, NULL, NULL };
    CHECK(!lang_register(&md_old) && lang_register(&md), "extension: language service registered; wrong API version refused");
    CHECK(lang_for(-1, "md") == &md && lang_for(DIALECT_PG, NULL) == &lang_sql && lang_for(-1, "md")->format == NULL,
          "extension: lookup by file type / dialect; a language without formatter offers no Format");
    LumeCancel cancel = { 1 }; FmtResult r; SqlStyle st; sql_style_default(&st);
    bool ok = lang_sql.format("select 1", 8, DIALECT_PG, &st, -1, -1, &r, &cancel);
    CHECK(!ok && r.refused && strstr(r.note, "Cancelled"), "language service: a cancelled format returns without work");
}

static void test_symbols(void) {
    ObjSymbol sy[32]; int n;
    const char *pg = "CREATE OR REPLACE FUNCTION s.f(p_id integer, OUT p_total numeric, p_note text DEFAULT 'x') RETURNS record LANGUAGE plpgsql AS $$\nDECLARE\n  v_count int := 0;\n  v_name text;\nBEGIN\n  RETURN;\nEND $$;";
    n = obj_symbols(pg, (int)strlen(pg), DIALECT_PG, sy, 32);
    CHECK(n == 5 && sy[0].parameter && !strcmp(sy[0].name, "p_id") && !strcmp(sy[0].type, "integer") && !strcmp(sy[2].name, "p_note") && !strcmp(sy[2].type, "text") &&
          !sy[3].parameter && !strcmp(sy[3].name, "v_count") && !strcmp(sy[3].type, "int") && !strcmp(sy[4].name, "v_name"),
          "OBJ-017: PL/pgSQL parameters (mode and DEFAULT skipped) and DECLARE variables inside $$ (%d)", n);
    const char *ora = "CREATE OR REPLACE PROCEDURE hr.raise(p_emp IN NUMBER, p_pct IN OUT NUMBER) IS\n  l_sal  emp.sal%TYPE;\n  CURSOR c IS SELECT 1 FROM dual;\n  l_done BOOLEAN := FALSE;\nBEGIN\n  NULL;\nEND;";
    n = obj_symbols(ora, (int)strlen(ora), DIALECT_ORACLE, sy, 32);
    CHECK(n == 4 && !strcmp(sy[1].name, "p_pct") && !strcmp(sy[1].type, "NUMBER") && !strcmp(sy[2].name, "l_sal") && !strcmp(sy[3].name, "l_done"),
          "OBJ-017: PL/SQL parameters (IN OUT) and the IS section variables, cursors skipped (%d)", n);
    const char *ms = "CREATE OR ALTER PROCEDURE dbo.p @id int AS BEGIN DECLARE @total money; DECLARE @n int = 1; SELECT @id; END";
    n = obj_symbols(ms, (int)strlen(ms), DIALECT_MSSQL, sy, 32);
    CHECK(n == 2 && !strcmp(sy[0].name, "@total") && !strcmp(sy[0].type, "money") && !strcmp(sy[1].name, "@n"), "OBJ-017: T-SQL DECLARE @variables (%d)", n);
}

static void test_refactor(void) {
    char why[200]; int n;
    const char *v = "CREATE OR REPLACE VIEW s.v AS SELECT t.id, x.total FROM t JOIN (SELECT id, sum(a) AS total FROM o GROUP BY id) x ON x.id = t.id";
    const char *sub = strstr(v, "(SELECT"); int s0 = (int)(sub - v), s1 = (int)(strstr(v, ") x") - v) + 1;
    char *r = obj_extract_cte(v, (int)strlen(v), DIALECT_PG, s0, s1, "totals", why, sizeof why);
    CHECK(r && strstr(r, "AS WITH totals AS (SELECT id, sum(a) AS total FROM o GROUP BY id)\nSELECT t.id, x.total FROM t JOIN totals x ON") && sql_same_tokens(r, (int)strlen(r), r, (int)strlen(r), DIALECT_PG),
          "OBJ-021: Extract to CTE: the JOIN subquery becomes WITH totals AS (...) and the JOIN uses the name");
    free(r);
    const char *w = "WITH a AS (SELECT 1 AS id) SELECT * FROM a, (SELECT 2 AS id) b";
    sub = strstr(w, "(SELECT 2"); r = obj_extract_cte(w, (int)strlen(w), DIALECT_PG, (int)(sub - w), (int)(strstr(w, ") b") - w) + 1, "two", why, sizeof why);
    CHECK(r && !strncmp(r, "WITH two AS (SELECT 2 AS id),\na AS", 34) && strstr(r, "FROM a, two b"), "OBJ-021: an existing WITH gets one more entry; FROM-list comma context accepted");
    free(r);
    const char *in = "SELECT * FROM t WHERE id IN (SELECT id FROM o)";
    sub = strstr(in, "(SELECT"); r = obj_extract_cte(in, (int)strlen(in), DIALECT_PG, (int)(sub - in), (int)strlen(in), "ids", why, sizeof why);
    CHECK(!r && strstr(why, "FROM / JOIN"), "OBJ-021: a subquery in IN (...) is refused: it would change the query (%s)", why);
    r = obj_extract_cte(in, (int)strlen(in), DIALECT_PG, 0, 6, "x", why, sizeof why);
    CHECK(!r, "OBJ-021: a selection that is not (SELECT ...) is refused");
    const char *f = "CREATE OR REPLACE FUNCTION s.f(p_id integer, OUT p_sum numeric, p_tag text) LANGUAGE sql AS $$ SELECT sum(a) FROM o WHERE id = $1 AND tag = $2 AND note <> '$1' $$;";
    r = obj_named_params(f, (int)strlen(f), DIALECT_PG, &n, why, sizeof why);
    CHECK(r && n == 2 && strstr(r, "id = p_id AND tag = p_tag AND note <> '$1'"), "OBJ-021: $1 / $2 become p_id / p_tag (OUT not numbered; the string '$1' untouched) (%d)", n);
    free(r);
    const char *g = "CREATE OR REPLACE FUNCTION s.g(integer) RETURNS int LANGUAGE sql AS $$ SELECT $1 $$;";
    r = obj_named_params(g, (int)strlen(g), DIALECT_PG, &n, why, sizeof why);
    CHECK(!r && strstr(why, "$1 has no named parameter"), "OBJ-021: an unnamed parameter is not invented (%s)", why);
    const char *cols[] = { "main.orders.id", "main.orders.total", "main.orders.customer_id", "main.customers.id", "main.customers.name" };
    const char *q = "CREATE VIEW v AS SELECT o.id AS oid, total, name, id, upper(name) AS up FROM orders o JOIN customers c ON c.id = customer_id WHERE total > 10 AND 'name' <> name";
    r = obj_qualify_columns(q, (int)strlen(q), DIALECT_SQLITE, cols, 5, &n, why, sizeof why);
    CHECK(r && n == 6 && strstr(r, "SELECT o.id AS oid, o.total, c.name, id, upper(c.name) AS up FROM orders o JOIN customers c ON c.id = o.customer_id WHERE o.total > 10 AND 'name' <> c.name"),
          "OBJ-021: Qualify Columns: total / name / customer_id get their alias; id (in both tables) and the string are left alone (%d)", n);
    free(r);
}

int main(void) {
    test_format();
    test_diff();
    test_header();
    test_signature();
    test_planner();
    test_diag();
    test_extensibility();
    test_symbols();
    test_refactor();
    printf("%s: %d ok, %d failed\n", fails ? "OBJECTS CORE FAILED" : "OBJECTS CORE OK", oks, fails);
    return fails ? 1 : 0;
}
