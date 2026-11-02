/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Dialect-aware SQL lexer (object source editor, OBJ-011 / 012 / 022 / 077)
 * ==========================================================================
 *
 * Splits SQL text into tokens without understanding grammar: keywords and identifiers, quoted
 * identifiers, strings, numbers, operators, punctuation, comments and whitespace. Each dialect
 * adds its own quoting so code inside it is never read as plain SQL:
 *
 *   PostgreSQL  E'...' escapes, $tag$ ... $tag$ bodies (PL/pgSQL), "ident"
 *   Oracle      q'[...]' alternative quoting, "ident"
 *   SQL Server  [ident], N'...', GO batch separators (a line holding only GO)
 *   MySQL       `ident`, "..." strings, # comments, backslash escapes
 *   SQLite      "ident", [ident], `ident`
 *
 * Tokens point into the caller's text (offset + length); nothing is copied. An unterminated
 * string, comment or body is still one token, flagged so diagnostics can report it.
 * Pure C, no SDL: usable from workers and tests.
 */
#include <stdbool.h>
#include <stddef.h>

/* Built-in dialect ids; engine adapters can register more (ids DIALECT_COUNT .. DIALECT_MAX-1). */
enum { DIALECT_GENERIC, DIALECT_SQLITE, DIALECT_PG, DIALECT_ORACLE, DIALECT_MYSQL, DIALECT_MARIADB, DIALECT_MSSQL, DIALECT_COUNT };
#define DIALECT_MAX 32

/* The lexical rules of a dialect: what quotes code so it is never read as plain SQL. A new
 * engine describes its own instead of changing the lexer. */
typedef struct SqlLexRules {
    bool hash_comments;      /* # to end of line (MySQL) */
    bool nested_comments;    /* block comments nest (PostgreSQL) */
    bool dollar_bodies;      /* $tag$ ... $tag$ (PostgreSQL) */
    bool q_quote;            /* q'[...]' (Oracle) */
    bool dq_strings;         /* "..." is a string, not an identifier (MySQL) */
    bool backslash_escapes;  /* \' inside strings (MySQL) */
    bool backtick_ident;     /* `ident` */
    bool bracket_ident;      /* [ident] */
    bool at_vars;            /* @name is one word (SQL Server) */
    bool colon_binds;        /* :name is one word (Oracle binds) */
    bool go_batches;         /* GO alone on a line separates batches (SQL Server clients) */
} SqlLexRules;
typedef struct SqlDialect { const char *name; const char *driver_id; SqlLexRules rules; } SqlDialect;
/* Registers a dialect (or returns the existing id for driver_id); -1 when the table is full.
 * name / driver_id must outlive the app. */
int dialect_register(const char *name, const char *driver_id, const SqlLexRules *rules);
const SqlLexRules *sql_lex_rules(int dialect);
int dialect_count(void);

enum {
    TK_WS, TK_COMMENT, TK_WORD, TK_QIDENT, TK_STRING, TK_BODY /* $tag$...$tag$ */, TK_NUMBER,
    TK_OP, TK_LPAREN, TK_RPAREN, TK_COMMA, TK_SEMI, TK_DOT, TK_BATCH /* GO */, TK_OTHER
};

typedef struct SqlTok { int kind, off, len; bool unterminated; } SqlTok;

const char *dialect_name(int d);
int dialect_for_driver(const char *driver_id);   /* "postgres" -> DIALECT_PG ... */

/* Tokenizes text[0..len) into out (at most max tokens); returns how many were written. A count
 * equal to max means the text was cut (callers treat that as "partial analysis"). */
int sql_lex(const char *text, int len, int dialect, SqlTok *out, int max);
/* Same, with explicit rules (a language service lexing a dialect it brings). */
int sql_lex_with(const char *text, int len, const SqlLexRules *rules, SqlTok *out, int max);
/* True when word token t (case-insensitive) equals kw. */
bool tok_is(const char *text, const SqlTok *t, const char *kw);
/* True for SQL keywords the formatter may re-case (a fixed list shared by the dialects). */
bool sql_is_keyword(const char *s, int len);
