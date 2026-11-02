/* SPDX-License-Identifier: AGPL-3.0-only */
/* Dialect-aware SQL lexer: see sqllex.h. */
#include "sqllex.h"
#include <ctype.h>
#include <string.h>
#include <strings.h>

/* Built-in dialects, then the ones engine adapters register (dialect_register). */
static SqlDialect dialects[DIALECT_MAX] = {
    /*  name          driver        #cmt  nest  $$    q''   "str  \esc  `id`  [id]  @var  :bind GO */
    { "SQL",        NULL,        { false, false, false, false, false, false, true,  false, false, false, false } },
    { "SQLite",     "sqlite",    { false, false, false, false, false, false, true,  true,  false, false, false } },
    { "PostgreSQL", "postgres",  { false, true,  true,  false, false, false, false, false, false, false, false } },
    { "Oracle",     "oracle",    { false, false, false, true,  false, false, false, false, false, true,  false } },
    { "MySQL",      "mysql",     { true,  false, false, false, true,  true,  true,  false, false, false, false } },
    { "MariaDB",    "mariadb",   { true,  false, false, false, true,  true,  true,  false, false, false, false } },
    { "SQL Server", "sqlserver", { false, false, false, false, false, false, true,  true,  true,  false, true  } },
};
static int ndialects = DIALECT_COUNT;

int dialect_register(const char *name, const char *driver_id, const SqlLexRules *rules) {
    for (int i = 0; i < ndialects; i++) if (driver_id && dialects[i].driver_id && !strcmp(dialects[i].driver_id, driver_id)) return i;
    if (ndialects >= DIALECT_MAX || !name || !rules) return -1;
    dialects[ndialects].name = name; dialects[ndialects].driver_id = driver_id; dialects[ndialects].rules = *rules;
    return ndialects++;
}
const SqlLexRules *sql_lex_rules(int d) { return &dialects[d >= 0 && d < ndialects ? d : DIALECT_GENERIC].rules; }
int dialect_count(void) { return ndialects; }

const char *dialect_name(int d) { return d >= 0 && d < ndialects ? dialects[d].name : "SQL"; }

int dialect_for_driver(const char *id) {
    if (!id) return DIALECT_GENERIC;
    for (int i = 1; i < ndialects; i++) if (dialects[i].driver_id && !strcmp(dialects[i].driver_id, id)) return i;
    return DIALECT_GENERIC;
}

bool tok_is(const char *text, const SqlTok *t, const char *kw) {
    size_t n = strlen(kw);
    return t->kind == TK_WORD && (size_t)t->len == n && !strncasecmp(text + t->off, kw, n);
}

static const char *const keywords[] = {
    "ADD", "ALL", "ALTER", "AND", "ANY", "AS", "ASC", "BEGIN", "BETWEEN", "BODY", "BY", "CALL", "CASCADE", "CASE", "CAST",
    "CHECK", "COLUMN", "COMMIT", "CONSTRAINT", "CREATE", "CROSS", "CURSOR", "DECLARE", "DEFAULT", "DELETE", "DESC",
    "DISTINCT", "DROP", "EACH", "ELSE", "ELSIF", "END", "EXCEPT", "EXCEPTION", "EXECUTE", "EXISTS", "FOR", "FOREIGN",
    "FROM", "FULL", "FUNCTION", "GROUP", "HAVING", "IF", "IN", "INNER", "INSERT", "INTERSECT", "INTO", "IS", "JOIN",
    "KEY", "LANGUAGE", "LEFT", "LIKE", "LIMIT", "LOOP", "MATERIALIZED", "NOT", "NULL", "OF", "OFFSET", "ON", "OR",
    "ORDER", "OUT", "OUTER", "PACKAGE", "PRIMARY", "PROCEDURE", "REFERENCES", "REPLACE", "RETURN", "RETURNS",
    "RIGHT", "ROLLBACK", "ROW", "SELECT", "SET", "TABLE", "THEN", "TRIGGER", "UNION", "UPDATE", "USING", "VALUES",
    "VIEW", "WHEN", "WHERE", "WHILE", "WITH", "INSTEAD", "AFTER", "BEFORE", "RETURNING", "IMMUTABLE", "STABLE",
    "VOLATILE", "STRICT", "SECURITY", "DEFINER", "INVOKER", "TYPE", "OVER", "PARTITION", "WINDOW", "LATERAL",
    NULL
};

bool sql_is_keyword(const char *s, int len) {
    for (int i = 0; keywords[i]; i++) if ((int)strlen(keywords[i]) == len && !strncasecmp(keywords[i], s, (size_t)len)) return true;
    return false;
}

/* A PostgreSQL dollar-quote tag starting at text[i] ('$' then [A-Za-z_][A-Za-z0-9_]* then '$', or "$$"); its length or 0. */
static int dollar_tag(const char *t, int len, int i) {
    if (t[i] != '$') return 0;
    int j = i + 1;
    if (j < len && t[j] == '$') return 2;
    if (j < len && (isalpha((unsigned char)t[j]) || t[j] == '_')) {
        while (j < len && (isalnum((unsigned char)t[j]) || t[j] == '_')) j++;
        if (j < len && t[j] == '$') return j - i + 1;
    }
    return 0;
}

int sql_lex(const char *t, int len, int d, SqlTok *out, int max) { return sql_lex_with(t, len, sql_lex_rules(d), out, max); }

int sql_lex_with(const char *t, int len, const SqlLexRules *R, SqlTok *out, int max) {
    int n = 0, i = 0;
    bool line_start = true;
    while (i < len && n < max) {
        SqlTok tk = { TK_OTHER, i, 1, false };
        char c = t[i];
        if (isspace((unsigned char)c)) {
            int j = i; while (j < len && isspace((unsigned char)t[j])) { if (t[j] == '\n') line_start = true; j++; }
            tk.kind = TK_WS; tk.len = j - i; out[n++] = tk; i = j; continue;
        }
        bool was_line_start = line_start; line_start = false;
        if (c == '-' && i + 1 < len && t[i + 1] == '-') { int j = i; while (j < len && t[j] != '\n') j++; tk.kind = TK_COMMENT; tk.len = j - i; }
        else if (c == '#' && R->hash_comments) { int j = i; while (j < len && t[j] != '\n') j++; tk.kind = TK_COMMENT; tk.len = j - i; }
        else if (c == '/' && i + 1 < len && t[i + 1] == '*') {
            int j = i + 2, depth = 1;
            while (j < len && depth) {
                if (R->nested_comments && t[j] == '/' && j + 1 < len && t[j + 1] == '*') { depth++; j += 2; continue; }   /* PG nests block comments */
                if (t[j] == '*' && j + 1 < len && t[j + 1] == '/') { depth--; j += 2; continue; }
                j++;
            }
            tk.kind = TK_COMMENT; tk.len = j - i; tk.unterminated = depth > 0;
        }
        else if (R->dollar_bodies && c == '$' && dollar_tag(t, len, i)) {
            int tl = dollar_tag(t, len, i), j = i + tl; bool closed = false;
            while (j < len) { if (t[j] == '$' && j + tl <= len && !memcmp(t + j, t + i, (size_t)tl)) { j += tl; closed = true; break; } j++; }
            tk.kind = TK_BODY; tk.len = j - i; tk.unterminated = !closed;
        }
        else if (R->q_quote && (c == 'q' || c == 'Q') && i + 2 < len && t[i + 1] == '\'') {
            char open = t[i + 2], close = open == '[' ? ']' : open == '{' ? '}' : open == '(' ? ')' : open == '<' ? '>' : open;
            int j = i + 3; bool closed = false;
            while (j + 1 < len) { if (t[j] == close && t[j + 1] == '\'') { j += 2; closed = true; break; } j++; }
            if (!closed) j = len;
            tk.kind = TK_STRING; tk.len = j - i; tk.unterminated = !closed;
        }
        else if (c == '\'' || ((c == 'E' || c == 'e' || c == 'N' || c == 'n' || c == 'X' || c == 'x') && i + 1 < len && t[i + 1] == '\'') ||
                 (c == '"' && R->dq_strings)) {
            char q = c == '"' ? '"' : '\'';
            bool backslash = (c == 'E' || c == 'e') || R->backslash_escapes;
            int j = i + (c == q ? 1 : 2); bool closed = false;
            while (j < len) {
                if (backslash && t[j] == '\\') { j += 2; continue; }
                if (t[j] == q) { if (j + 1 < len && t[j + 1] == q) { j += 2; continue; } j++; closed = true; break; }
                j++;
            }
            if (j > len) j = len;
            tk.kind = TK_STRING; tk.len = j - i; tk.unterminated = !closed;
        }
        else if (c == '"' || (c == '`' && R->backtick_ident) || (c == '[' && R->bracket_ident)) {
            char close = c == '[' ? ']' : c;
            int j = i + 1; bool closed = false;
            while (j < len) { if (t[j] == close) { if (close != ']' && j + 1 < len && t[j + 1] == close) { j += 2; continue; } j++; closed = true; break; } j++; }
            tk.kind = TK_QIDENT; tk.len = j - i; tk.unterminated = !closed;
        }
        else if (isdigit((unsigned char)c) || (c == '.' && i + 1 < len && isdigit((unsigned char)t[i + 1]))) {
            int j = i; while (j < len && (isalnum((unsigned char)t[j]) || t[j] == '.')) j++;
            tk.kind = TK_NUMBER; tk.len = j - i;
        }
        else if (isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80 || (c == '@' && R->at_vars) || (c == ':' && R->colon_binds && i + 1 < len && isalpha((unsigned char)t[i + 1]))) {
            int j = i + 1; while (j < len && (isalnum((unsigned char)t[j]) || t[j] == '_' || t[j] == '$' || t[j] == '#' || (unsigned char)t[j] >= 0x80)) j++;
            tk.kind = TK_WORD; tk.len = j - i;
            /* SQL Server: GO alone on its line is the client's batch separator, never sent */
            if (R->go_batches && was_line_start && tk.len == 2 && !strncasecmp(t + i, "GO", 2)) {
                int k = j; while (k < len && (t[k] == ' ' || t[k] == '\t')) k++;
                if (k >= len || t[k] == '\n' || t[k] == '\r') tk.kind = TK_BATCH;
            }
        }
        else if (c == '(') tk.kind = TK_LPAREN;
        else if (c == ')') tk.kind = TK_RPAREN;
        else if (c == ',') tk.kind = TK_COMMA;
        else if (c == ';') tk.kind = TK_SEMI;
        else if (c == '.') tk.kind = TK_DOT;
        else if (strchr("+-*/%=<>!|&^~:?@", c)) {
            int j = i + 1; while (j < len && strchr("=<>!|&:", t[j]) && j - i < 3) j++;
            tk.kind = TK_OP; tk.len = j - i;
        }
        if (tk.off + tk.len > len) tk.len = len - tk.off;
        if (tk.len <= 0) tk.len = 1;
        out[n++] = tk;
        i = tk.off + tk.len;
    }
    return n;
}
