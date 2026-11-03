/* SPDX-License-Identifier: AGPL-3.0-only */
/* complete.c: SQL autocompletion for the editor (see docs/autocomplete-rules.md).
 *
 * The editor (editor.c) calls complete_query() with the statement around the caret; this module
 * lexes it, works out the context (after "alias." -> that table's columns; after FROM / JOIN /
 * INTO / UPDATE -> tables; otherwise columns in scope, aliases, tables, keywords and functions)
 * and returns ranked candidates. It reads only the in-memory schema cache handed over by
 * complete_set_schema() (DbTable array owned by the caller); columns of a table are loaded lazily
 * through the load_cols callback the first time they are needed. It never runs SQL itself.
 *
 * Ranking: text match quality first (exact-case prefix > any-case prefix > word-boundary match >
 * substring > subsequence), then a learned usage score. Usage lives in a small hash table of
 * string keys with exponentially decaying counts (half-life one week):
 *   t:TABLE            a table was used
 *   c:TABLE.COLUMN     a column was used
 *   j:A+B              two tables appeared in the same statement (names sorted)
 *   n:TOK TOK [TOK]    2- and 3-gram of normalised tokens ending in an object / keyword
 *   KEYWORD            a keyword chosen from the popup
 * It is learnt from executed statements (complete_learn_statement) and accepted proposals
 * (complete_learn_choice), and persisted per connection location in
 * <pref dir>/usage-<hash>.txt (only names, never data), saved with a debounce from complete_tick.
 *
 * Threading: main thread only; g_c and g_has_prefix are file-static helpers for qsort and the
 * lazy column loader. */
#include "complete.h"
#include <ctype.h>
#include <math.h>
#include <strings.h>
#include <time.h>

/* ---- usage store ------------------------------------------------------- */

/* Usage decay half-life (seconds), delay before writing the usage file after a change (ms), and
 * how many of the highest-scoring entries are kept when saving. */
#define HALF_LIFE_S (7.0 * 86400.0)
#define SAVE_DEBOUNCE_MS 2000
#define PRUNE_KEEP 3000

/* One usage entry: heap-owned key, count as of `last` (unix seconds); decays from there. */
typedef struct { char *key; float count; double last; } Entry;

/* Completer state: borrowed schema (tables / ntables, plus the lazy column loader and its
 * context), the usage hash table (cap is a power of two, kept at most half full), the usage file
 * path ("" = not persisted), dirty flag + SDL tick of the first unsaved change, and the n-gram
 * context of the last query so an accepted choice can be learnt as an n-gram. */
struct Completer {
    DbTable *tables; int ntables;
    bool (*load_cols)(void *ctx, DbTable *t); void *load_ctx;
    bool (*load_fks)(void *ctx, const char *table, DbFk **out, int *n);
    DbFk **fks; int *nfks; unsigned char *fks_state;   /* per table: 0 not read, 1 read */
    Entry *ent; int nent, cap;      /* open addressing, linear probe */
    char path[1024];
    bool dirty;
    Uint64 dirty_since;
    char prev1[128], prev2[128];    /* normalised tokens before the last query, for learn_choice */
};

/* 32-bit FNV-1a hash of a NUL-terminated string (hash table and usage file name). */
static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

/* Current wall-clock time in unix seconds (usage timestamps survive restarts). */
static double now_s(void) { return (double)time(NULL); }

/* Linear-probe lookup: the slot holding `key`, or the empty slot where it would go.
 * `cap` must be a power of two and the table must have a free slot. */
static Entry *find_slot(Entry *ent, int cap, const char *key) {
    uint32_t i = fnv1a(key) & (cap - 1);
    for (;;) {
        if (!ent[i].key || strcmp(ent[i].key, key) == 0) return &ent[i];
        i = (i + 1) & (cap - 1);
    }
}

/* Double the hash table (initially 256 slots) and rehash every entry; keys are moved, not copied. */
static void grow(Completer *c) {
    int ncap = c->cap ? c->cap * 2 : 256;
    Entry *n = calloc(ncap, sizeof *n);
    for (int i = 0; i < c->cap; i++) if (c->ent[i].key) *find_slot(n, ncap, c->ent[i].key) = c->ent[i];
    free(c->ent);
    c->ent = n; c->cap = ncap;
}

/* Entry for `key`, or NULL when it was never recorded. */
static Entry *lookup(Completer *c, const char *key) {
    if (!c->cap) return NULL;
    Entry *e = find_slot(c->ent, c->cap, key);
    return e->key ? e : NULL;
}

/* Decayed usage value of an entry at time `now` (0 for NULL). */
static float value_of(Entry *e, double now) {
    if (!e) return 0;
    return e->count * (float)pow(0.5, (now - e->last) / HALF_LIFE_S);
}

/* Current decayed usage of `key` (0 when unknown). */
static float usage_of(Completer *c, const char *key) { return value_of(lookup(c, key), now_s()); }

/* Record one use of `key`: decay the old count to now, add 1, mark the store dirty (starting the
 * save debounce). Empty keys are ignored; the table grows to stay at most half full. */
static void bump(Completer *c, const char *key) {
    if (!key[0]) return;
    if ((c->nent + 1) * 2 > c->cap) grow(c);
    Entry *e = find_slot(c->ent, c->cap, key);
    double now = now_s();
    if (!e->key) { e->key = SDL_strdup(key); e->count = 0; e->last = now; c->nent++; }
    e->count = value_of(e, now) + 1;
    e->last = now;
    if (!c->dirty) { c->dirty = true; c->dirty_since = SDL_GetTicks(); }
}

/* Free every key and the table itself; leaves an empty, clean store. */
static void store_clear(Completer *c) {
    for (int i = 0; i < c->cap; i++) free(c->ent[i].key);
    free(c->ent);
    c->ent = NULL; c->nent = c->cap = 0;
    c->dirty = false;
}

/* qsort comparator: highest decayed value first. */
static int cmp_entry_desc(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    double now = now_s();
    float vx = value_of((Entry *)x, now), vy = value_of((Entry *)y, now);
    return vx < vy ? 1 : vx > vy ? -1 : 0;
}

/* Write the usage file (tab-separated key, count, last), best PRUNE_KEEP entries only, and clear
 * the dirty flag. Does nothing when no path is set; write errors are silently ignored. */
static void store_save(Completer *c) {
    c->dirty = false;
    if (!c->path[0]) return;
    Entry *all = malloc(sizeof *all * (c->nent ? c->nent : 1));
    int n = 0;
    for (int i = 0; i < c->cap; i++) if (c->ent[i].key) all[n++] = c->ent[i];
    qsort(all, n, sizeof *all, cmp_entry_desc);
    if (n > PRUNE_KEEP) n = PRUNE_KEEP;
    FILE *f = fopen(c->path, "w");
    if (f) {
        fprintf(f, "# lume usage v1: key, count, last-used (unix seconds). Names only.\n");
        for (int i = 0; i < n; i++) fprintf(f, "%s\t%.4f\t%.0f\n", all[i].key, all[i].count, all[i].last);
        fclose(f);
    }
    free(all);
}

/* Merge the usage file at c->path into the table (lines starting with '#' are comments).
 * A missing file is not an error. */
static void store_load(Completer *c) {
    FILE *f = fopen(c->path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1++ = 0;
        char *t2 = strchr(t1, '\t');
        if (!t2) continue;
        *t2++ = 0;
        if ((c->nent + 1) * 2 > c->cap) grow(c);
        Entry *e = find_slot(c->ent, c->cap, line);
        if (!e->key) { e->key = SDL_strdup(line); c->nent++; }
        e->count = (float)atof(t1);
        e->last = atof(t2);
    }
    fclose(f);
    c->dirty = false;
}

/* ---- lexer ------------------------------------------------------------- */

/* Token kinds of the completion lexer (comments are recognised but not emitted). */
enum { TK_IDENT, TK_KW, TK_NUM, TK_STR, TK_COMMENT, TK_DOT, TK_COMMA, TK_LPAREN, TK_RPAREN, TK_SEMI, TK_OP };

/* A token: kind and byte range [start, end) in the source text. */
typedef struct { int type, start, end; } Tok;

/* SQL keywords offered as completions and recognised by the lexer (upper case). */
static const char *const keywords[] = {
    "SELECT", "FROM", "WHERE", "AND", "OR", "NOT", "NULL", "IS", "IN", "LIKE", "BETWEEN", "JOIN", "LEFT",
    "RIGHT", "INNER", "OUTER", "CROSS", "ON", "AS", "GROUP", "BY", "ORDER", "HAVING", "LIMIT", "OFFSET",
    "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE", "CREATE", "TABLE", "VIEW", "INDEX", "DROP",
    "ALTER", "DISTINCT", "UNION", "ALL", "CASE", "WHEN", "THEN", "ELSE", "END", "EXISTS", "ASC", "DESC",
    "PRIMARY", "KEY", "DEFAULT", "WITH", "EXPLAIN", "PRAGMA", "BEGIN", "COMMIT", "ROLLBACK", "TRANSACTION",
    "USING", "NATURAL", "IF", "REPLACE", "RETURNING", "TRUE", "FALSE", "COLLATE", "GLOB", "ESCAPE",
};
/* Built-in (SQLite) function names offered as "name(" completions. */
static const char *const functions[] = {
    "count", "sum", "avg", "min", "max", "coalesce", "ifnull", "nullif", "length", "upper", "lower",
    "substr", "trim", "ltrim", "rtrim", "replace", "round", "abs", "date", "time", "datetime", "strftime",
    "julianday", "typeof", "group_concat", "total", "cast", "printf", "instr", "random", "iif", "hex",
    "quote", "unicode", "zeroblob", "last_insert_rowid", "changes", "sqlite_version",
};
#define NKEYWORDS (int)(sizeof keywords / sizeof *keywords)
#define NFUNCTIONS (int)(sizeof functions / sizeof *functions)

/* Identifier character: ASCII letter, digit, '_' or any non-ASCII byte (UTF-8 names). */
static bool is_word(unsigned char ch) { return isalnum(ch) || ch == '_' || ch >= 0x80; }

/* Case-insensitive (ASCII) equality between the alen bytes of a and the whole string b. */
static bool ieq(const char *a, int alen, const char *b) {
    for (int i = 0; i < alen; i++) if (!b[i] || tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return b[alen] == 0;
}

/* True when the n bytes at s spell one of `keywords`. */
static bool is_keyword(const char *s, int n) {
    for (int i = 0; i < NKEYWORDS; i++) if (ieq(s, n, keywords[i])) return true;
    return false;
}

/* Tokenises s[0..len). Returns the token count; *open_literal is true when the text ends inside
 * a string or comment. Quoted identifiers keep their quotes stripped (start/end exclude them). */
static int lex(const char *s, int len, Tok *out, int max, bool *open_literal) {
    int n = 0, i = 0;
    *open_literal = false;
    while (i < len && n < max) {
        unsigned char ch = (unsigned char)s[i];
        if (isspace(ch)) { i++; continue; }
        Tok t = { TK_OP, i, i + 1 };
        if (ch == '-' && i + 1 < len && s[i + 1] == '-') {
            while (i < len && s[i] != '\n') i++;
            t.type = TK_COMMENT; t.end = i;
            if (i >= len) *open_literal = true;
        } else if (ch == '/' && i + 1 < len && s[i + 1] == '*') {
            i += 2;
            bool closed = false;
            while (i < len) { if (s[i] == '*' && i + 1 < len && s[i + 1] == '/') { i += 2; closed = true; break; } i++; }
            t.type = TK_COMMENT; t.end = i;
            if (!closed) *open_literal = true;
        } else if (ch == '\'' || ch == '"' || ch == '`' || ch == '[') {
            char q = ch == '[' ? ']' : (char)ch;
            i++;
            int st = i;
            bool closed = false;
            while (i < len) {
                if (s[i] == q) { if (q == '\'' && i + 1 < len && s[i + 1] == '\'') { i += 2; continue; } closed = true; break; }
                i++;
            }
            t.type = ch == '\'' ? TK_STR : TK_IDENT; t.start = st; t.end = i;
            if (closed) i++; else *open_literal = true;
        } else if (isdigit(ch)) {
            while (i < len && (isalnum((unsigned char)s[i]) || s[i] == '.')) i++;
            t.type = TK_NUM; t.end = i;
        } else if (is_word(ch)) {
            while (i < len && is_word((unsigned char)s[i])) i++;
            t.end = i;
            t.type = is_keyword(s + t.start, i - t.start) ? TK_KW : TK_IDENT;
        } else {
            i++;
            t.type = ch == '.' ? TK_DOT : ch == ',' ? TK_COMMA : ch == '(' ? TK_LPAREN : ch == ')' ? TK_RPAREN : ch == ';' ? TK_SEMI : TK_OP;
        }
        if (t.type != TK_COMMENT) out[n++] = t;
    }
    return n;
}

/* ---- scope analysis ---------------------------------------------------- */

/* Limits: tables / aliases tracked per statement, tokens lexed per call (longer text is cut). */
#define MAX_SCOPE 32
#define MAX_TOKS 2048

/* An alias declared in the statement (e.g. "FROM orders o") and the table index it stands for.
 * Scope: the tables and aliases a statement references. */
typedef struct { char alias[64]; int table; } Alias;
typedef struct {
    int tables[MAX_SCOPE]; int ntables;   /* indices into schema, in order of appearance */
    Alias aliases[MAX_SCOPE]; int naliases;
} Scope;

/* Matches "name", "schema.name" against cached names that may or may not carry a schema. */
/* Index in the schema cache of the table named by the n bytes at s, or -1. Exact name first, then
 * the last dotted part of s, or (for an unqualified s) a cached "schema.name" whose name matches. */
static int find_table(Completer *c, const char *s, int n) {
    for (int i = 0; i < c->ntables; i++) if (ieq(s, n, c->tables[i].name)) return i;
    const char *last = s; int ln = n;
    for (const char *p; (p = memchr(last, '.', ln)); ) { ln -= (int)(p + 1 - last); last = p + 1; }
    if (last != s) {
        for (int i = 0; i < c->ntables; i++) if (ieq(last, ln, c->tables[i].name)) return i;
    } else {
        for (int i = 0; i < c->ntables; i++) {
            const char *d = strrchr(c->tables[i].name, '.');
            if (d && ieq(s, n, d + 1)) return i;
        }
    }
    return -1;
}

static Completer *g_c;   /* for ensure_cols from find_column (single-threaded) */
/* Make sure t's columns are in the cache, asking the owner through load_cols if not. */
static void ensure_cols(Completer *c, DbTable *t) {
    if (!t->cols_loaded && c && c->load_cols) c->load_cols(c->load_ctx, t);
}

/* Index of the column named by the n bytes at s in table t (columns loaded on demand), or -1. */
static int find_column(DbTable *t, const char *s, int n) {
    ensure_cols(g_c, t);
    for (int i = 0; i < t->ncols; i++) if (ieq(s, n, t->cols[i].name)) return i;
    return -1;
}

/* True when token t is the keyword kw (case-insensitive). */
static bool kw_is(const char *s, Tok *t, const char *kw) { return t->type == TK_KW && ieq(s + t->start, t->end - t->start, kw); }

/* True when a table name follows token t (FROM, JOIN, INTO, UPDATE, TABLE). */
static bool expects_table(const char *s, Tok *t) {
    return kw_is(s, t, "FROM") || kw_is(s, t, "JOIN") || kw_is(s, t, "INTO") || kw_is(s, t, "UPDATE") || kw_is(s, t, "TABLE");
}

/* True when token t ends a FROM-style list of tables (a clause keyword, ';' or a parenthesis). */
static bool ends_from_list(const char *s, Tok *t) {
    if (t->type == TK_SEMI || t->type == TK_RPAREN || t->type == TK_LPAREN) return true;
    if (t->type != TK_KW) return false;
    static const char *const stop[] = { "WHERE", "ON", "GROUP", "ORDER", "LIMIT", "SET", "VALUES", "SELECT", "HAVING", "UNION", "USING", "RETURNING" };
    for (size_t i = 0; i < sizeof stop / sizeof *stop; i++) if (kw_is(s, t, stop[i])) return true;
    return false;
}

/* True when, with nothing typed yet, a column name is the natural next token after t. */
static bool expects_column(const char *s, Tok *t) {
    if (t->type == TK_LPAREN) return true;
    static const char *const kws[] = { "SET", "WHERE", "ON", "BY", "SELECT", "AND", "OR", "HAVING" };
    for (size_t i = 0; i < sizeof kws / sizeof *kws; i++) if (kw_is(s, t, kws[i])) return true;
    return false;
}

/* Add a table index to the scope once (silently ignored past MAX_SCOPE). */
static void scope_add(Scope *sc, int table) {
    for (int i = 0; i < sc->ntables; i++) if (sc->tables[i] == table) return;
    if (sc->ntables < MAX_SCOPE) sc->tables[sc->ntables++] = table;
}

/* Walks tokens [0, ntok) collecting tables and aliases. When `upto` >= 0, also reports whether
 * the position right after token `upto` expects a table name (FROM list state). */
static void analyse(Completer *c, const char *s, Tok *tk, int ntok, Scope *sc, int upto, bool *table_ctx) {
    memset(sc, 0, sizeof *sc);
    bool from_list = false;
    if (table_ctx) *table_ctx = false;
    for (int i = 0; i < ntok; i++) {
        bool want = expects_table(s, &tk[i]) || (from_list && tk[i].type == TK_COMMA);
        if (expects_table(s, &tk[i])) from_list = true;
        else if (ends_from_list(s, &tk[i])) from_list = false;
        if (table_ctx && i == upto) *table_ctx = want;
        if (!want || i + 1 >= ntok || tk[i + 1].type != TK_IDENT) continue;
        int j = i + 1;
        /* schema.table: match the dotted name as a whole, then fall back to the last part */
        while (j + 2 < ntok && tk[j + 1].type == TK_DOT && tk[j + 2].type == TK_IDENT) j += 2;
        int table = find_table(c, s + tk[i + 1].start, tk[j].end - tk[i + 1].start);
        if (table < 0) continue;
        scope_add(sc, table);
        int k = j + 1;
        if (k < ntok && kw_is(s, &tk[k], "AS")) k++;
        if (k < ntok && tk[k].type == TK_IDENT && sc->naliases < MAX_SCOPE) {
            Alias *al = &sc->aliases[sc->naliases++];
            int n = tk[k].end - tk[k].start;
            if (n > 63) n = 63;
            memcpy(al->alias, s + tk[k].start, n);
            al->alias[n] = 0;
            al->table = table;
        }
    }
}

/* Table index for a qualifier before '.': an alias of this statement, else a table name; -1. */
static int resolve_qualifier(Completer *c, Scope *sc, const char *s, int n) {
    for (int i = 0; i < sc->naliases; i++) if (ieq(s, n, sc->aliases[i].alias)) return sc->aliases[i].table;
    return find_table(c, s, n);
}

/* Normalised token text for n-grams: keywords upper-cased, objects as usage keys, others literal. */
static void norm_token(Completer *c, Scope *sc, const char *s, Tok *tk, int i, char *out, size_t n) {
    Tok *t = &tk[i];
    int len = t->end - t->start;
    if (t->type == TK_KW) {
        int k = 0;
        for (; k < len && k < (int)n - 1; k++) out[k] = (char)toupper((unsigned char)s[t->start + k]);
        out[k] = 0;
    } else if (t->type == TK_IDENT) {
        int table = -1;
        if (i >= 2 && tk[i - 1].type == TK_DOT && tk[i - 2].type == TK_IDENT)
            table = resolve_qualifier(c, sc, s + tk[i - 2].start, tk[i - 2].end - tk[i - 2].start);
        if (table >= 0) {
            int col = find_column(&c->tables[table], s + t->start, len);
            if (col >= 0) { snprintf(out, n, "c:%s.%s", c->tables[table].name, c->tables[table].cols[col].name); return; }
        }
        int tb = find_table(c, s + t->start, len);
        if (tb >= 0) { snprintf(out, n, "t:%s", c->tables[tb].name); return; }
        for (int j = 0; j < sc->ntables; j++) {
            int col = find_column(&c->tables[sc->tables[j]], s + t->start, len);
            if (col >= 0) { snprintf(out, n, "c:%s.%s", c->tables[sc->tables[j]].name, c->tables[sc->tables[j]].cols[col].name); return; }
        }
        snprintf(out, n, "id");
    } else if (t->type == TK_STR) snprintf(out, n, "str");
    else if (t->type == TK_NUM) snprintf(out, n, "num");
    else snprintf(out, n, "%.*s", len < (int)n - 1 ? len : (int)n - 1, s + t->start);
}

/* ---- public: lifecycle ------------------------------------------------- */

/* New completer with no schema and an empty, unpersisted usage store. Free with complete_destroy. */
Completer *complete_create(void) { return calloc(1, sizeof(Completer)); }

/* Save pending usage, free the usage store and the completer (the schema is not owned). */
void complete_destroy(Completer *c) {
    if (c->dirty) store_save(c);
    store_clear(c);
    for (int i = 0; c->fks && i < c->ntables; i++) if (c->fks[i]) db_fks_free(c->fks[i], c->nfks[i]);
    free(c->fks); free(c->nfks); free(c->fks_state);
    free(c);
}

/* Switch to another schema / connection: save and drop the old usage, borrow the new table array,
 * and load the usage file for `location` from LUME_PREF_DIR or SDL's pref path. */
void complete_set_schema(Completer *c, DbTable *tables, int ntables, const char *location,
                         bool (*load_cols)(void *ctx, DbTable *t), void *ctx) {
    if (c->dirty) store_save(c);
    store_clear(c);
    for (int i = 0; c->fks && i < c->ntables; i++) if (c->fks[i]) db_fks_free(c->fks[i], c->nfks[i]);
    free(c->fks); free(c->nfks); free(c->fks_state); c->fks = NULL; c->nfks = NULL; c->fks_state = NULL;
    c->tables = tables; c->ntables = ntables;
    if (ntables > 0) { c->fks = calloc((size_t)ntables, sizeof *c->fks); c->nfks = calloc((size_t)ntables, sizeof *c->nfks); c->fks_state = calloc((size_t)ntables, 1); }
    c->load_cols = load_cols; c->load_ctx = ctx;
    g_c = c;
    c->path[0] = 0;
    if (!location) return;
    const char *envdir = SDL_getenv("LUME_PREF_DIR");
    char *pref = envdir ? NULL : SDL_GetPrefPath("lume", "Lume");
    const char *dir = envdir ? envdir : pref;
    if (dir) {
        snprintf(c->path, sizeof c->path, "%s%susage-%08x.txt", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", fnv1a(location));
        store_load(c);
    }
    SDL_free(pref);
}

void complete_set_fk_loader(Completer *c, bool (*load_fks)(void *ctx, const char *table, DbFk **out, int *n)) { c->load_fks = load_fks; }
static void ensure_fks(Completer *c, int t) {
    if (!c->fks || t < 0 || t >= c->ntables || c->fks_state[t]) return;
    c->fks_state[t] = 1;
    if (c->load_fks && !c->tables[t].is_view && !c->load_fks(c->load_ctx, c->tables[t].name, &c->fks[t], &c->nfks[t])) { c->fks[t] = NULL; c->nfks[t] = 0; c->fks_state[t] = 0; }   /* busy or failed: asked again next time */
}

/* Save the usage file once SAVE_DEBOUNCE_MS passed since the first unsaved change. Returns true
 * while a save is still pending, so the caller keeps waking up. */
bool complete_tick(Completer *c) {
    if (!c->dirty) return false;
    if (SDL_GetTicks() - c->dirty_since >= SAVE_DEBOUNCE_MS) { store_save(c); return false; }
    return true;
}

/* ---- public: learning -------------------------------------------------- */

/* Learn from one statement's tokens: bump every table in scope, every pair of tables used
 * together, every resolved column, and the 2- and 3-grams of normalised tokens. */
static void learn_one(Completer *c, const char *s, Tok *tk, int ntok) {
    if (ntok == 0) return;
    Scope sc;
    analyse(c, s, tk, ntok, &sc, -1, NULL);
    char key[160], p1[128] = "", p2[128] = "";
    for (int i = 0; i < sc.ntables; i++) {
        snprintf(key, sizeof key, "t:%s", c->tables[sc.tables[i]].name);
        bump(c, key);
        for (int j = i + 1; j < sc.ntables; j++) {
            const char *a = c->tables[sc.tables[i]].name, *b = c->tables[sc.tables[j]].name;
            if (strcmp(a, b) > 0) { const char *t = a; a = b; b = t; }
            snprintf(key, sizeof key, "j:%s+%s", a, b);
            bump(c, key);
        }
    }
    for (int i = 0; i < ntok; i++) {
        char cur[128];
        norm_token(c, &sc, s, tk, i, cur, sizeof cur);
        if (cur[0] == 'c' && cur[1] == ':') bump(c, cur);
        if (p1[0]) { snprintf(key, sizeof key, "n:%s %s", p1, cur); bump(c, key); }
        if (p2[0]) { snprintf(key, sizeof key, "n:%s %s %s", p2, p1, cur); bump(c, key); }
        memcpy(p2, p1, sizeof p2);
        memcpy(p1, cur, sizeof p1);
    }
}

/* Split sql (len < 0: NUL-terminated) on ';' and learn each statement. No-op without a schema. */
void complete_learn_statement(Completer *c, const char *sql, int len) {
    if (!c->tables || !sql) return;
    if (len < 0) len = (int)strlen(sql);
    Tok *tk = malloc(sizeof *tk * MAX_TOKS);
    bool open;
    int ntok = lex(sql, len, tk, MAX_TOKS, &open);
    int start = 0;
    for (int i = 0; i <= ntok; i++) {
        if (i == ntok || tk[i].type == TK_SEMI) { learn_one(c, sql, tk + start, i - start); start = i + 1; }
    }
    free(tk);
}

/* Learn an accepted proposal: bump its key, plus n-grams with the tokens that preceded the caret
 * in the last complete_query call. */
void complete_learn_choice(Completer *c, const Cand *cand) {
    if (!cand->key[0]) return;
    bump(c, cand->key);
    char key[400];
    if (c->prev1[0]) { snprintf(key, sizeof key, "n:%s %s", c->prev1, cand->key); bump(c, key); }
    if (c->prev2[0]) { snprintf(key, sizeof key, "n:%s %s %s", c->prev2, c->prev1, cand->key); bump(c, key); }
}

/* ---- public: query ----------------------------------------------------- */

/* Text match score of candidate `name` against the typed prefix: 500 exact-case prefix,
 * 400 case-insensitive prefix, 300 match at a word boundary (after '_' or a camelCase hump),
 * 200 substring, 100 subsequence, 0 no match. An empty prefix matches everything with 1. */
static int match_score(const char *name, const char *prefix, int plen) {
    if (plen == 0) return 1;
    int nlen = (int)strlen(name);
    if (nlen < plen) return 0;
    if (strncmp(name, prefix, plen) == 0) return 500;
    if (ieq(prefix, plen, name) || strncasecmp(name, prefix, plen) == 0) return 400;
    for (int i = 1; i + plen <= nlen; i++) {
        bool boundary = name[i - 1] == '_' || (islower((unsigned char)name[i - 1]) && isupper((unsigned char)name[i]));
        if (boundary && strncasecmp(name + i, prefix, plen) == 0) return 300;
    }
    const char *p = name;
    for (; *p; p++) if (strncasecmp(p, prefix, plen) == 0) return 200;
    int k = 0;
    for (const char *q = name; *q && k < plen; q++) if (tolower((unsigned char)*q) == tolower((unsigned char)prefix[k])) k++;
    return k == plen ? 100 : 0;
}

/* Candidate generator state for one query: output array and capacity, the typed prefix, the
 * statement's scope and the two normalised tokens before the prefix (n-gram context). */
typedef struct {
    Completer *c; Cand *out; int n, max;
    const char *prefix; int plen;
    Scope *sc;
    char p1[128], p2[128];
} Gen;

/* Usage of `key` following the previous one / two tokens; trigrams weigh twice. */
static float ngram_usage(Gen *g, const char *key) {
    char k[400];
    float u = 0;
    if (g->p1[0]) { snprintf(k, sizeof k, "n:%s %s", g->p1, key); u += usage_of(g->c, k); }
    if (g->p2[0]) { snprintf(k, sizeof k, "n:%s %s %s", g->p2, g->p1, key); u += 2 * usage_of(g->c, k); }
    return u;
}

/* Append a candidate when it matches the prefix (and there is room): copies text / detail / key,
 * scores the match and adds n-gram usage to `usage`. `key` NULL = not learnable. */
static void add(Gen *g, const char *text, const char *detail, const char *key, int kind, float usage) {
    if (g->n >= g->max) return;
    int m = match_score(text, g->prefix, g->plen);
    if (!m) return;
    Cand *cd = &g->out[g->n++];
    snprintf(cd->text, sizeof cd->text, "%s", text);
    snprintf(cd->detail, sizeof cd->detail, "%s", detail);
    snprintf(cd->key, sizeof cd->key, "%s", key ? key : "");
    cd->kind = kind;
    cd->match = m;
    cd->usage = usage + (key ? ngram_usage(g, key) : 0);
}

/* Add every column of a table (type and PK flag in the detail). show_table is currently unused. */
static void add_columns(Gen *g, int table, bool show_table) {
    DbTable *t = &g->c->tables[table];
    ensure_cols(g->c, t);
    char key[160], detail[96];
    for (int i = 0; i < t->ncols; i++) {
        snprintf(key, sizeof key, "c:%s.%s", t->name, t->cols[i].name);
        (void)show_table;
        snprintf(detail, sizeof detail, "%s \xC2\xB7 %s%s", t->name, t->cols[i].type, t->cols[i].pk ? " \xC2\xB7 PK" : "");
        add(g, t->cols[i].name, detail, key, CAND_COLUMN, usage_of(g->c, key));
    }
}

/* Add every table and view; usage includes a bonus for tables often joined with those in scope. */
/* How a table is named in this statement: its alias, else its name without the schema. */
static const char *qual_of(Scope *sc, Completer *c, int t) {
    for (int i = 0; i < sc->naliases; i++) if (sc->aliases[i].table == t) return sc->aliases[i].alias;
    const char *n = c->tables[t].name, *dot = strrchr(n, '.');
    return dot ? dot + 1 : n;
}
static bool same_table(const char *fk_target, const char *name) {
    const char *a = strrchr(fk_target, '.'), *b = strrchr(name, '.');
    return !strcasecmp(fk_target, name) || !strcasecmp(a ? a + 1 : fk_target, b ? b + 1 : name);
}
/* "JOIN b ON |": the conditions linking b to the tables before it. Real foreign keys first (every
 * pair of a composite key, either direction); then columns with exactly the same name that are a
 * primary key on at least one side (no naming convention assumed). */
static void add_join_conditions(Gen *g, Scope *sc, int joined) {
    Completer *c = g->c;
    char text[240], detail[96], done[24][240]; int nd = 0;
    const char *qb = qual_of(sc, c, joined);
    ensure_fks(c, joined);
    for (int si = 0; si < sc->ntables; si++) {
        int t = sc->tables[si]; if (t == joined) continue;
        const char *qa = qual_of(sc, c, t);
        ensure_fks(c, t);
        for (int dir = 0; dir < 2; dir++) {   /* 0: joined -> t, 1: t -> joined */
            int from = dir ? t : joined, to = dir ? joined : t;
            const char *qf = dir ? qa : qb, *qt = dir ? qb : qa;
            DbFk *f = c->fks ? c->fks[from] : NULL; int nf = c->nfks ? c->nfks[from] : 0;
            for (int i = 0; i < nf; i++) {
                if (!same_table(f[i].to_table, c->tables[to].name)) continue;
                bool first = true; for (int q = 0; q < i; q++) if (f[q].name && f[i].name && !strcmp(f[q].name, f[i].name) && same_table(f[q].to_table, f[i].to_table)) first = false;
                if (!first) continue;
                size_t w = 0; int pairs = 0;
                for (int q = i; q < nf && w < sizeof text - 8; q++) {
                    bool same = q == i || (f[q].name && f[i].name && !strcmp(f[q].name, f[i].name));
                    if (!same || !same_table(f[q].to_table, f[i].to_table)) continue;
                    /* written with the joined table first: "b.x = a.y" */
                    if (dir == 0) w += (size_t)snprintf(text + w, sizeof text - w, "%s%s.%s = %s.%s", pairs ? " AND " : "", qf, f[q].from_col, qt, f[q].to_col ? f[q].to_col : f[q].from_col);
                    else w += (size_t)snprintf(text + w, sizeof text - w, "%s%s.%s = %s.%s", pairs ? " AND " : "", qt, f[q].to_col ? f[q].to_col : f[q].from_col, qf, f[q].from_col);
                    pairs++;
                }
                snprintf(detail, sizeof detail, "foreign key%s%s%s", f[i].name && f[i].name[0] ? " " : "", f[i].name ? f[i].name : "", pairs > 1 ? " (composite)" : "");
                if (nd < 24) snprintf(done[nd++], sizeof done[0], "%s", text);
                add(g, text, detail, NULL, CAND_COLUMN, 3.0f);
            }
        }
        /* same column name, a primary key on at least one side */
        ensure_cols(c, &c->tables[joined]); ensure_cols(c, &c->tables[t]);
        DbTable *B = &c->tables[joined], *A = &c->tables[t];
        for (int i = 0; i < B->ncols; i++) for (int j = 0; j < A->ncols; j++) {
            if (strcasecmp(B->cols[i].name, A->cols[j].name) || !(B->cols[i].pk || A->cols[j].pk)) continue;
            if (B->cols[i].pk && A->cols[j].pk && !strcasecmp(B->cols[i].name, "id")) continue;   /* each table's own "id": not a relation */
            snprintf(text, sizeof text, "%s.%s = %s.%s", qb, B->cols[i].name, qa, A->cols[j].name);
            bool dup = false; for (int q = 0; q < nd; q++) if (strstr(done[q], text)) dup = true;
            if (dup) continue;
            snprintf(detail, sizeof detail, "same column name (key in %s)", A->cols[j].pk ? qa : qb);
            add(g, text, detail, NULL, CAND_COLUMN, 1.0f);
        }
    }
}

static void add_tables(Gen *g) {
    char key[160];
    for (int i = 0; i < g->c->ntables; i++) {
        DbTable *t = &g->c->tables[i];
        snprintf(key, sizeof key, "t:%s", t->name);
        float u = usage_of(g->c, key);
        /* tables used together with the ones already in the statement rank first */
        for (int j = 0; j < g->sc->ntables; j++) {
            const char *a = t->name, *b = g->c->tables[g->sc->tables[j]].name;
            if (g->sc->tables[j] == i) continue;
            if (strcmp(a, b) > 0) { const char *x = a; a = b; b = x; }
            char jk[200];
            snprintf(jk, sizeof jk, "j:%s+%s", a, b);
            u += 3 * usage_of(g->c, jk);
        }
        add(g, t->name, t->is_view ? "view" : "table", key, t->is_view ? CAND_VIEW : CAND_TABLE, u);
    }
}

/* Add keywords (lower-cased when `lower_kw`, to follow the user's style) and functions as "name(". */
static void add_words(Gen *g, bool lower_kw) {
    char buf[64];
    for (int i = 0; i < NKEYWORDS; i++) {
        int k = 0;
        for (; keywords[i][k]; k++) buf[k] = lower_kw ? (char)tolower((unsigned char)keywords[i][k]) : keywords[i][k];
        buf[k] = 0;
        add(g, buf, "keyword", keywords[i], CAND_KEYWORD, usage_of(g->c, keywords[i]) * 0.01f);
    }
    for (int i = 0; i < NFUNCTIONS; i++) {
        snprintf(buf, sizeof buf, "%s(", functions[i]);
        /* match on the bare name so "cou" finds count( */
        int m = match_score(functions[i], g->prefix, g->plen);
        if (!m || g->n >= g->max) continue;
        Cand *cd = &g->out[g->n++];
        snprintf(cd->text, sizeof cd->text, "%s", buf);
        snprintf(cd->detail, sizeof cd->detail, "function");
        cd->key[0] = 0;
        cd->kind = CAND_FUNC;
        cd->match = m;
        cd->usage = 0;
    }
}

/* qsort comparator for candidates: match score, then (with a prefix) kind before usage, or
 * (without a prefix) usage before kind, then alphabetical. g_has_prefix carries the mode. */
static int g_has_prefix;
static int cmp_cand(const void *a, const void *b) {
    const Cand *x = a, *y = b;
    if (x->match != y->match) return y->match - x->match;
    if (g_has_prefix && x->kind != y->kind) return x->kind - y->kind;
    if (x->usage != y->usage) return x->usage < y->usage ? 1 : -1;
    if (!g_has_prefix && x->kind != y->kind) return x->kind - y->kind;
    return strcasecmp(x->text, y->text);
}

/* See complete.h. Returns the number of candidates written to out (sorted best first), 0 inside
 * a string / comment, after a digit, or when the only proposal is what is already typed. */
static int (*extra_fn)(void *ud, CompleteExtra *out, int max);
static void *extra_ud;
void complete_set_extra(int (*fn)(void *ud, CompleteExtra *out, int max), void *ud) { extra_fn = fn; extra_ud = ud; }
static void add_extra(Gen *g) {
    if (!extra_fn) return;
    CompleteExtra ex[32]; int n = extra_fn(extra_ud, ex, 32);
    for (int i = 0; i < n; i++) add(g, ex[i].text, ex[i].detail, NULL, CAND_COLUMN, 0.5f);
}

int complete_query(Completer *c, const char *s, int len, int cursor, bool explicit, Cand *out, int max, int *prefix_len) {
    *prefix_len = 0;
    c->prev1[0] = c->prev2[0] = 0;
    if (cursor < 0 || cursor > len) return 0;

    /* identifier prefix before the cursor */
    int ps = cursor;
    while (ps > 0 && is_word((unsigned char)s[ps - 1])) ps--;
    if (ps > 0 && isdigit((unsigned char)s[ps]) && ps < cursor) return 0;
    int plen = cursor - ps;
    *prefix_len = plen;

    Tok *tk = malloc(sizeof *tk * MAX_TOKS);
    bool open;
    int ntok = lex(s, ps, tk, MAX_TOKS, &open);
    if (open) { free(tk); return 0; }
    /* previous significant token index (before the prefix) */
    int prev = ntok - 1;
    Scope sc;
    bool table_ctx = false;
    /* analyse the whole statement so aliases declared after the cursor still resolve */
    Tok *all = malloc(sizeof *all * MAX_TOKS);
    bool open2;
    int nall = lex(s, len, all, MAX_TOKS, &open2);
    /* drop the word under the cursor: it is what we are completing, not an alias */
    for (int i = 0; i < nall; i++) {
        if (all[i].start < cursor && all[i].end > ps) { memmove(all + i, all + i + 1, sizeof *all * (nall - i - 1)); nall--; i--; }
    }
    analyse(c, s, all, nall, &sc, -1, NULL);
    if (prev >= 0) {
        Scope tmp;
        analyse(c, s, tk, ntok, &tmp, prev, &table_ctx);
    }

    Gen g = { c, out, 0, max, s + ps, plen, &sc, "", "" };
    if (prev >= 0) norm_token(c, &sc, s, tk, prev, g.p1, sizeof g.p1);
    if (prev >= 1) norm_token(c, &sc, s, tk, prev - 1, g.p2, sizeof g.p2);
    memcpy(c->prev1, g.p1, sizeof c->prev1);
    memcpy(c->prev2, g.p2, sizeof c->prev2);

    /* keyword case: lower when the prefix has no capitals; with no prefix, like the last keyword */
    bool lower_kw = plen > 0;
    for (int i = 0; i < plen; i++) if (isupper((unsigned char)s[ps + i])) lower_kw = false;
    if (plen == 0) {
        /* nothing typed: follow the case of the last keyword in the statement */
        for (int i = ntok - 1; i >= 0; i--) if (tk[i].type == TK_KW) { lower_kw = islower((unsigned char)s[tk[i].start]); break; }
    }

    /* pick the candidate set from the context before the prefix */
    if (prev >= 1 && tk[prev].type == TK_DOT && tk[prev - 1].type == TK_IDENT) {
        int table = resolve_qualifier(c, &sc, s + tk[prev - 1].start, tk[prev - 1].end - tk[prev - 1].start);
        if (table >= 0) add_columns(&g, table, false);
    } else if (prev >= 0 && table_ctx) {
        add_tables(&g);
    } else if (prev >= 0 && kw_is(s, &tk[prev], "ON") && sc.ntables >= 2) {
        /* the table joined by this ON: the identifier after the closest JOIN before it */
        int joined = -1;
        for (int i = prev - 1; i >= 0 && joined < 0; i--) if (kw_is(s, &tk[i], "JOIN") && i + 1 < ntok && tk[i + 1].type == TK_IDENT) {
            int j = i + 1; while (j + 2 < ntok && tk[j + 1].type == TK_DOT && tk[j + 2].type == TK_IDENT) j += 2;
            joined = find_table(c, s + tk[i + 1].start, tk[j].end - tk[i + 1].start);
        }
        if (joined >= 0) add_join_conditions(&g, &sc, joined);
        if (plen > 0 || explicit || !g.n) for (int i = 0; i < sc.ntables; i++) add_columns(&g, sc.tables[i], sc.ntables > 1);
    } else if (plen == 0 && !explicit && prev >= 0 && sc.ntables > 0 && expects_column(s, &tk[prev])) {
        /* "UPDATE t SET ", "WHERE ", "INSERT INTO t (": columns of the tables in scope */
        for (int i = 0; i < sc.ntables; i++) add_columns(&g, sc.tables[i], sc.ntables > 1);
        add_extra(&g);
    } else if (plen > 0 || explicit) {
        add_extra(&g);
        for (int i = 0; i < sc.ntables; i++) add_columns(&g, sc.tables[i], sc.ntables > 1);
        char key[160];
        for (int i = 0; i < sc.naliases; i++) {
            if ((int)strlen(sc.aliases[i].alias) == plen && strncasecmp(sc.aliases[i].alias, s + ps, plen) == 0) continue;
            snprintf(key, sizeof key, "alias of %s", c->tables[sc.aliases[i].table].name);
            add(&g, sc.aliases[i].alias, key, NULL, CAND_ALIAS, 0);
        }
        add_tables(&g);
        add_words(&g, lower_kw);
    }
    free(tk);
    free(all);

    /* rank */
    g_has_prefix = plen > 0;
    qsort(out, g.n, sizeof *out, cmp_cand);
    /* a single proposal that already equals the prefix is noise */
    if (g.n == 1 && plen > 0 && (int)strlen(out[0].text) == plen && strncmp(out[0].text, g.prefix, plen) == 0) return 0;
    return g.n;
}
