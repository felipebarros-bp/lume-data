/* SPDX-License-Identifier: AGPL-3.0-only */
/* Object model helpers: names, keys, capabilities, snapshot / plan lifetime. See objmodel.h. */
#include "objengine.h"
#include "sqllex.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *objkind_name(int k) {
    static const char *n[] = { "VIEW", "MATERIALIZED VIEW", "FUNCTION", "PROCEDURE", "PACKAGE", "PACKAGE BODY",
                               "TRIGGER", "TYPE", "TYPE BODY", "OPERATOR", "EVENT TRIGGER", "VIRTUAL TABLE" };
    return k >= 0 && k < OBJK_COUNT ? n[k] : "OBJECT";
}
const char *objkind_word(int k) {
    static const char *n[] = { "view", "materialized view", "function", "procedure", "package specification", "package body",
                               "trigger", "type", "type body", "operator", "event trigger", "virtual table" };
    return k >= 0 && k < OBJK_COUNT ? n[k] : "object";
}

void objkey_display(const ObjKey *k, char *out, int n) {
    char q[300];
    if (k->schema[0]) snprintf(q, sizeof q, "%s.%s", k->schema, k->name); else snprintf(q, sizeof q, "%s", k->name);
    if (k->kind == OBJK_FUNCTION || k->kind == OBJK_PROCEDURE || k->kind == OBJK_OPERATOR) {
        if (k->signature[0] || k->schema[0]) snprintf(out, (size_t)n, "%s %s(%s)", objkind_name(k->kind), q, k->signature);
        else snprintf(out, (size_t)n, "%s %s", objkind_name(k->kind), q);
    } else if (k->kind == OBJK_TRIGGER && k->table[0]) snprintf(out, (size_t)n, "TRIGGER %s ON %s", q, k->table);
    else snprintf(out, (size_t)n, "%s %s", objkind_name(k->kind), q);
}

bool objkey_equal(const ObjKey *a, const ObjKey *b) {
    return a->kind == b->kind && !strcmp(a->conn_id, b->conn_id) && !strcmp(a->schema, b->schema) &&
           !strcmp(a->name, b->name) && !strcmp(a->signature, b->signature) && !strcmp(a->table, b->table);
}

/* Capabilities come from the engine adapter (objengine.h); none for an unknown engine. */
ObjCaps objcaps_for(int d, int k) {
    const ObjEngine *e = obj_engine_for_dialect(d);
    ObjCaps c; memset(&c, 0, sizeof c);
    return e ? e->caps(k, 0) : c;
}

void objsnap_free(ObjSnapshot *s) {
    if (!s) return;
    free(s->original); free(s->ddl); free(s->cols); free(s->errors); free(s->deps);
    for (int i = 0; i < s->nrecreate; i++) free(s->recreate[i]);
    free(s->recreate);
    memset(s, 0, sizeof *s);
}

uint64_t obj_hash(const char *s, int n) {
    uint64_t h = 1469598103934665603ULL;
    if (n < 0) n = s ? (int)strlen(s) : 0;
    for (int i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

const char *risk_name(int r) {
    switch (r) {
    case RISK_NON_DESTRUCTIVE: return "non-destructive";
    case RISK_POTENTIAL: return "potentially destructive";
    case RISK_DESTRUCTIVE: return "destructive";
    default: return "unknown";
    }
}
const char *atomicity_name(int a) {
    switch (a) {
    case ATOM_TRANSACTIONAL: return "one transaction (rolled back on failure)";
    case ATOM_AUTOCOMMIT: return "not atomic: each DDL commits on its own";
    case ATOM_NON_ATOMIC: return "not atomic";
    default: return "unknown";
    }
}
const char *diag_severity_name(int s) {
    switch (s) { case DIAG_ERROR: return "error"; case DIAG_WARNING: return "warning"; case DIAG_INFO: return "info"; default: return "hint"; }
}
const char *diag_source_name(int s) {
    switch (s) { case DSRC_PARSER: return "parser"; case DSRC_METADATA: return "metadata"; case DSRC_DRIVER: return "driver"; default: return "server"; }
}

void objplan_free(ObjPlan *p) {
    for (int i = 0; i < p->nsql; i++) free(p->sql[i]);
    memset(p, 0, sizeof *p);
    p->create_index = -1;
}

/* True when an Oracle statement is a PL/SQL unit (needs "/" after it in a script). */
static bool oracle_plsql_unit(const char *s) {
    SqlTok t[16]; int n = sql_lex(s, (int)strlen(s), DIALECT_ORACLE, t, 16);
    for (int i = 0; i < n; i++) {
        if (t[i].kind != TK_WORD) continue;
        if (tok_is(s, &t[i], "FUNCTION") || tok_is(s, &t[i], "PROCEDURE") || tok_is(s, &t[i], "PACKAGE") ||
            tok_is(s, &t[i], "TRIGGER") || tok_is(s, &t[i], "TYPE")) return true;
        if (tok_is(s, &t[i], "VIEW") || tok_is(s, &t[i], "TABLE") || tok_is(s, &t[i], "INDEX") || tok_is(s, &t[i], "GRANT")) return false;
    }
    return false;
}
static bool has_begin(const char *s) {
    SqlTok *t = malloc(sizeof *t * (strlen(s) + 1)); int n = sql_lex(s, (int)strlen(s), DIALECT_MYSQL, t, (int)strlen(s) + 1);
    bool b = false; for (int i = 0; i < n && !b; i++) if (tok_is(s, &t[i], "BEGIN")) b = true;
    free(t); return b;
}

char *objplan_script(const ObjPlan *p, int d) {
    size_t cap = 256; for (int i = 0; i < p->nsql; i++) cap += strlen(p->sql[i]) + 64;
    char *o = malloc(cap); size_t n = 0; o[0] = 0;
#define ADD(s) do { size_t l_ = strlen(s); memcpy(o + n, s, l_); n += l_; o[n] = 0; } while (0)
    for (int i = 0; i < p->nsql; i++) {
        const char *s = p->sql[i];
        if (d == DIALECT_MSSQL) { ADD(s); ADD("\nGO\n"); }
        else if (d == DIALECT_ORACLE && oracle_plsql_unit(s)) { ADD(s); ADD("\n/\n"); }
        else if ((d == DIALECT_MYSQL || d == DIALECT_MARIADB) && has_begin(s)) { ADD("DELIMITER $$\n"); ADD(s); ADD(" $$\nDELIMITER ;\n"); }
        else { ADD(s); ADD(";\n"); }
    }
#undef ADD
    return o;
}
