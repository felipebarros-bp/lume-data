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
    int (*load_cols)(void *ctx, DbTable *t); void *load_ctx;   /* 1 filled, 0 asked in the background, -1 refused / failed */
    int (*load_fks)(void *ctx, const char *table, DbFk **out, int *n);   /* 1 filled, 0 asked in the background, -1 refused / failed */
    DbFk **fks; int *nfks; unsigned char *fks_state;   /* per table of the schema cache: 0 not read, 1 read, 2 asked in the background */
    DbFk **xfk; int *nxfk; unsigned char *xfst;        /* the same per catalog table (parallel to xt) */
    Entry *ent; int nent, cap;      /* open addressing, linear probe */
    char path[1024];
    bool dirty;
    Uint64 dirty_since;
    char prev1[128], prev2[128];    /* normalised tokens before the last query, for learn_choice */
    const ObjCat *cat; char home[128];   /* every visible object (borrowed) and the schema proposed unqualified */
    CompleteTableFn table_fn; void *table_ctx;   /* catalog objects as tables, columns in the background */
    DbTable **xt; int *xst; int nxt, capxt;   /* catalog tables met in statements: index ntables + k (pointers owned by objcat), their OCS_* state */
    bool pending; char pending_name[200]; char pending_what[24];   /* the last query waits for these ("columns" / "foreign keys") of this table */
    DbTable *dt; int ndt, capdt;   /* derived tables and CTEs of the statement being analysed (index DT_BASE + k; rebuilt per query) */
    int *tset; int tset_cap; DbTable *tset_tables; int tset_n;   /* names of `tables` (index + 1, open addressing): catalog entries already proposed */
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
    "MERGE", "MATCHED", "APPLY", "LATERAL", "FULL", "FETCH", "FIRST", "NEXT", "ROWS", "ONLY", "START", "CONNECT",
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

/* Table i of the statement's world: the schema cache, then catalog tables (index ntables + k). */
#define DT_BASE (1 << 20)   /* indices of derived tables / CTEs, above every real table */
static DbTable *TB(Completer *c, int i) { return i >= DT_BASE ? &c->dt[i - DT_BASE] : i < c->ntables ? &c->tables[i] : c->xt[i - c->ntables]; }
static void dt_clear(Completer *c) {
    for (int k = 0; k < c->ndt; k++) { DbTable *t = &c->dt[k]; for (int i = 0; i < t->ncols; i++) { free(t->cols[i].name); free(t->cols[i].type); } free(t->cols); free(t->name); }
    c->ndt = 0;
}
/* the matching ')' of the '(' at lp, or `end` when it never closes */
static int match_paren(Tok *tk, int end, int lp) {
    int d = 0;
    for (int i = lp; i < end; i++) { if (tk[i].type == TK_LPAREN) d++; else if (tk[i].type == TK_RPAREN && --d == 0) return i; }
    return end;
}
static bool is_relation(int kind) { return kind == OC_TABLE || kind == OC_VIEW || kind == OC_MVIEW || kind == OC_SYNONYM; }
/* The catalog object named by the n bytes at s ("SCHEMA.NAME", or NAME in the current schema,
 * then a PUBLIC synonym) as a table index, its columns asked for in the background; -1. */
static int catalog_table(Completer *c, const char *s, int n) {
    if (!c->cat || !c->table_fn || n <= 0) return -1;
    const char *dot = NULL;
    for (int i = n - 1; i >= 0; i--) if (s[i] == '.') { dot = s + i; break; }
    char nm[160]; const OcSchema *sch[2] = { NULL, NULL };
    if (dot) { snprintf(nm, sizeof nm, "%.*s", (int)(s + n - dot - 1), dot + 1); sch[0] = objcat_schema(c->cat, s, (int)(dot - s)); }
    else { snprintf(nm, sizeof nm, "%.*s", n, s); sch[0] = objcat_schema(c->cat, c->home, (int)strlen(c->home)); sch[1] = objcat_schema(c->cat, "PUBLIC", 6); }
    for (int q = 0; q < 2; q++) {
        const OcSchema *h = sch[q]; if (!h) continue;
        const OcObj *hit = NULL;
        for (int i = h->first; i < h->first + h->n && !hit; i++) if (is_relation(c->cat->o[i].kind) && !strcmp(c->cat->o[i].name, nm)) hit = &c->cat->o[i];
        for (int i = h->first; i < h->first + h->n && !hit; i++) if (is_relation(c->cat->o[i].kind) && !strcasecmp(c->cat->o[i].name, nm)) hit = &c->cat->o[i];
        if (!hit) continue;
        int st; DbTable *t = c->table_fn(c->table_ctx, hit->schema, hit->name, &st);
        if (!t) return -1;
        for (int k = 0; k < c->nxt; k++) if (c->xt[k] == t) { c->xst[k] = st; return c->ntables + k; }
        if (c->nxt == c->capxt) {
            int nc = c->capxt ? c->capxt * 2 : 16;
            DbTable **nx = realloc(c->xt, sizeof *nx * (size_t)nc); if (!nx) return -1; c->xt = nx;
            int *ns = realloc(c->xst, sizeof *ns * (size_t)nc); if (!ns) return -1; c->xst = ns;
            DbFk **nf = realloc(c->xfk, sizeof *nf * (size_t)nc); if (!nf) return -1; c->xfk = nf;
            int *nn = realloc(c->nxfk, sizeof *nn * (size_t)nc); if (!nn) return -1; c->nxfk = nn;
            unsigned char *nst = realloc(c->xfst, (size_t)nc); if (!nst) return -1; c->xfst = nst;
            c->capxt = nc;
        }
        c->xst[c->nxt] = st; c->xfk[c->nxt] = NULL; c->nxfk[c->nxt] = 0; c->xfst[c->nxt] = 0; c->xt[c->nxt++] = t;
        return c->ntables + c->nxt - 1;
    }
    return -1;
}

/* Matches "name", "schema.name" against cached names that may or may not carry a schema. */
/* Index in the schema cache of the table named by the n bytes at s, or -1. Exact name first, then
 * the last dotted part of s, or (for an unqualified s) a cached "schema.name" whose name matches. */
static int find_table_ex(Completer *c, const char *s, int n, bool ask_catalog) {
    for (int k = 0; k < c->ndt; k++) if (ieq(s, n, c->dt[k].name)) return DT_BASE + k;   /* a CTE or derived table shadows a real one */
    for (int i = 0; i < c->ntables; i++) if (ieq(s, n, c->tables[i].name)) return i;
    if (ask_catalog) { int ct = catalog_table(c, s, n); if (ct >= 0) return ct; }   /* another schema's object, a synonym: before guessing by the last part */
    else for (int k = 0; k < c->nxt; k++) if (ieq(s, n, c->xt[k]->name)) return c->ntables + k;   /* only what this statement already met: no column request as a side effect */
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
static int find_table(Completer *c, const char *s, int n) { return find_table_ex(c, s, n, true); }

static Completer *g_c;   /* for ensure_cols from find_column (single-threaded) */
/* Make sure t's columns are in the cache, asking the owner through load_cols if not. */
static void ensure_cols(Completer *c, DbTable *t) {
    if (!c || t->cols_loaded) return;
    bool own = c->tables && t >= c->tables && t < c->tables + c->ntables;
    bool loading = false;
    if (own) { if (c->load_cols) loading = c->load_cols(c->load_ctx, t) == 0 && !t->cols_loaded; }   /* 0: asked in the background; a refusal or failure is not waited for */
    else for (int k = 0; k < c->nxt; k++) if (c->xt[k] == t) loading = c->xst[k] == OCS_LOADING;   /* catalog tables: asked for by catalog_table */
    if (loading && !t->cols_loaded && !c->pending) { c->pending = true; snprintf(c->pending_what, sizeof c->pending_what, "columns"); snprintf(c->pending_name, sizeof c->pending_name, "%s", t->name); }
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
    return kw_is(s, t, "FROM") || kw_is(s, t, "JOIN") || kw_is(s, t, "INTO") || kw_is(s, t, "UPDATE") || kw_is(s, t, "TABLE")
        || kw_is(s, t, "USING") || kw_is(s, t, "APPLY");   /* MERGE ... USING u b; CROSS APPLY f(x) c (a JOIN ... USING ( is followed by a parenthesis, not a name) */
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

static int dt_from_query(Completer *c, const char *s, Tok *tk, int L, int R, int nametok, int depth);
static int resolve_qualifier(Completer *c, Scope *sc, const char *s, int n);
static void ensure_cols(Completer *c, DbTable *t);
static void scope_alias(Scope *sc, const char *s, Tok *t, int table) {
    if (sc->naliases >= MAX_SCOPE) return;
    Alias *al = &sc->aliases[sc->naliases++];
    int n = t->end - t->start; if (n > 63) n = 63;
    memcpy(al->alias, s + t->start, n); al->alias[n] = 0; al->table = table;
}
/* Walks tokens [L, R) of one query level collecting its tables and aliases: real tables, CTEs
 * (WITH name AS (...)) and derived tables ((...) alias) — each defined by its own select list —
 * while nested parentheses (subqueries, calls) are skipped. When `upto` >= 0, also reports whether
 * the position right after token `upto` expects a table name (FROM list state). */
static void analyse_range(Completer *c, const char *s, Tok *tk, int L, int R, Scope *sc, int upto, bool *table_ctx, int depth) {
    memset(sc, 0, sizeof *sc);
    bool from_list = false;
    if (table_ctx) *table_ctx = false;
    int i = L;
    if (i < R && kw_is(s, &tk[i], "WITH")) {   /* the CTEs: each one a table of this level (and of the levels below) */
        i++;
        if (i < R && tk[i].type == TK_IDENT && ieq(s + tk[i].start, tk[i].end - tk[i].start, "RECURSIVE")) i++;
        while (i < R && tk[i].type == TK_IDENT) {
            int name = i++;
            if (i < R && tk[i].type == TK_LPAREN) i = match_paren(tk, R, i) + 1;   /* (column list): the select list names them anyway */
            if (!(i < R && kw_is(s, &tk[i], "AS"))) break;
            i++;
            if (!(i < R && tk[i].type == TK_LPAREN)) break;
            int M = match_paren(tk, R, i);
            if (depth < 8) dt_from_query(c, s, tk, i + 1, M, name, depth + 1);
            i = M + 1;
            if (i < R && tk[i].type == TK_COMMA) i++; else break;
        }
    }
    for (; i < R; i++) {
        if (tk[i].type == TK_LPAREN) {
            int M = match_paren(tk, R, i);
            bool want_here = i > L && (expects_table(s, &tk[i - 1]) || (from_list && tk[i - 1].type == TK_COMMA));
            if (want_here && depth < 8) {   /* a derived table: (select ...) alias */
                int k = M + 1;
                if (k < R && kw_is(s, &tk[k], "AS")) k++;
                if (k < R && tk[k].type == TK_IDENT) {
                    int t = dt_from_query(c, s, tk, i + 1, M, k, depth + 1);
                    if (t >= 0) { scope_add(sc, t); scope_alias(sc, s, &tk[k], t); }
                }
            }
            if (table_ctx && upto > i && upto <= M) *table_ctx = false;
            i = M;   /* the inside is another level */
            continue;
        }
        bool want = expects_table(s, &tk[i]) || (from_list && tk[i].type == TK_COMMA);
        if (expects_table(s, &tk[i])) from_list = true;
        else if (ends_from_list(s, &tk[i])) from_list = false;
        if (table_ctx && i == upto) *table_ctx = want;
        if (!want || i + 1 >= R || tk[i + 1].type != TK_IDENT) continue;
        int j = i + 1;
        /* schema.table: match the dotted name as a whole, then fall back to the last part */
        while (j + 2 < R && tk[j + 1].type == TK_DOT && tk[j + 2].type == TK_IDENT) j += 2;
        int table = find_table(c, s + tk[i + 1].start, tk[j].end - tk[i + 1].start);
        if (table < 0) continue;
        scope_add(sc, table);
        int k = j + 1;
        if (k < R && kw_is(s, &tk[k], "AS")) k++;
        if (k < R && tk[k].type == TK_IDENT) scope_alias(sc, s, &tk[k], table);
    }
}
static void analyse(Completer *c, const char *s, Tok *tk, int ntok, Scope *sc, int upto, bool *table_ctx) { analyse_range(c, s, tk, 0, ntok, sc, upto, table_ctx, 0); }

/* copies a column (name, type, pk) into the derived table t */
static void dt_col(DbTable *t, const char *name, int nlen, const char *type, bool pk) {
    DbColumn *nc = realloc(t->cols, sizeof *nc * (size_t)(t->ncols + 1)); if (!nc) return;
    t->cols = nc; DbColumn *col = &t->cols[t->ncols++]; memset(col, 0, sizeof *col);
    col->name = malloc((size_t)nlen + 1); memcpy(col->name, name, (size_t)nlen); col->name[nlen] = 0;
    col->type = strdup(type ? type : "expression"); col->pk = pk;
}
/* The query at tokens [L, R) ("select ... from ...") as a table named by token nametok: its select
 * list is its columns (a plain column keeps its type, `expr AS name` and `expr name` their name,
 * `*` and `t.*` expand what the query's own FROM knows). Returns the DT_BASE index, or -1. */
static int dt_from_query(Completer *c, const char *s, Tok *tk, int L, int R, int nametok, int depth) {
    if (c->ndt == c->capdt) { int nc = c->capdt ? c->capdt * 2 : 8; DbTable *nd = realloc(c->dt, sizeof *nd * (size_t)nc); if (!nd) return -1; c->dt = nd; c->capdt = nc; }
    int idx = c->ndt++;
    DbTable *t = &c->dt[idx]; memset(t, 0, sizeof *t);
    { int n = tk[nametok].end - tk[nametok].start; t->name = malloc((size_t)n + 1); memcpy(t->name, s + tk[nametok].start, (size_t)n); t->name[n] = 0; }
    t->cols_loaded = true; t->is_view = true;
    Scope inner; analyse_range(c, s, tk, L, R, &inner, -1, NULL, depth);   /* may add more derived tables: t's address is refreshed below */
    t = &c->dt[idx];
    int i = L;
    while (i < R && !kw_is(s, &tk[i], "SELECT")) i++;   /* a CTE body may start with parentheses or UNION parts: the first select list */
    if (i >= R) return DT_BASE + idx;
    i++;
    if (i < R && kw_is(s, &tk[i], "DISTINCT")) i++;
    int a = i, d = 0;
    for (int j = i; j <= R; j++) {
        bool end = j == R || (d == 0 && (tk[j].type == TK_COMMA || kw_is(s, &tk[j], "FROM")));
        if (j < R && tk[j].type == TK_LPAREN) d++;
        else if (j < R && tk[j].type == TK_RPAREN) d--;
        if (!end) continue;
        int b = j;   /* item [a, b) */
        if (b > a) {
            if (b - a == 1 && tk[a].type == TK_OP && s[tk[a].start] == '*') {   /* every column of the FROM */
                for (int q = 0; q < inner.ntables; q++) { DbTable *src = TB(c, inner.tables[q]); ensure_cols(c, src); for (int k = 0; k < src->ncols; k++) dt_col(t, src->cols[k].name, (int)strlen(src->cols[k].name), src->cols[k].type, src->cols[k].pk); t = &c->dt[idx]; }
            } else if (b - a == 3 && tk[a].type == TK_IDENT && tk[a + 1].type == TK_DOT && tk[a + 2].type == TK_OP && s[tk[a + 2].start] == '*') {   /* q.* */
                int tb = resolve_qualifier(c, &inner, s + tk[a].start, tk[a].end - tk[a].start);
                if (tb >= 0) { DbTable *src = TB(c, tb); ensure_cols(c, src); for (int k = 0; k < src->ncols; k++) dt_col(t, src->cols[k].name, (int)strlen(src->cols[k].name), src->cols[k].type, src->cols[k].pk); t = &c->dt[idx]; }
            } else if (tk[b - 1].type == TK_IDENT) {
                int last = b - 1, se = b;   /* the name is the last identifier; the expression before it may be a plain column */
                if (b - a >= 3 && kw_is(s, &tk[b - 2], "AS")) se = b - 2;
                else if (b - a >= 2 && tk[b - 2].type != TK_DOT && tk[b - 2].type != TK_OP && tk[b - 2].type != TK_LPAREN && tk[b - 2].type != TK_KW) se = b - 1;   /* expr name */
                const char *type = NULL; bool pk = false;
                int ce = se == b ? b : se;   /* the expression [a, ce) */
                if (ce - a == 1 && tk[a].type == TK_IDENT) {   /* a column of one of the FROM tables */
                    for (int q = 0; q < inner.ntables && !type; q++) { DbTable *src = TB(c, inner.tables[q]); ensure_cols(c, src); for (int k = 0; k < src->ncols; k++) if (ieq(s + tk[a].start, tk[a].end - tk[a].start, src->cols[k].name)) { type = src->cols[k].type; pk = src->cols[k].pk; break; } }
                } else if (ce - a == 3 && tk[a].type == TK_IDENT && tk[a + 1].type == TK_DOT && tk[a + 2].type == TK_IDENT) {   /* q.col */
                    int tb = resolve_qualifier(c, &inner, s + tk[a].start, tk[a].end - tk[a].start);
                    if (tb >= 0) { DbTable *src = TB(c, tb); ensure_cols(c, src); for (int k = 0; k < src->ncols; k++) if (ieq(s + tk[a + 2].start, tk[a + 2].end - tk[a + 2].start, src->cols[k].name)) { type = src->cols[k].type; pk = src->cols[k].pk; break; } }
                }
                dt_col(t, s + tk[last].start, tk[last].end - tk[last].start, type, pk); t = &c->dt[idx];
            }
        }
        if (j < R && kw_is(s, &tk[j], "FROM")) break;
        a = j + 1;
    }
    return DT_BASE + idx;
}

/* true when the position right after token i expects a table name (a comma in a FROM list) */
static bool table_ctx_at(Completer *c, const char *s, Tok *tk, int i) { Scope tmp; bool t = false; analyse(c, s, tk, i + 1, &tmp, i, &t); return t; }

/* Table index for a qualifier before '.': an alias of this statement, else a table name; -1. */
static int resolve_qualifier(Completer *c, Scope *sc, const char *s, int n) {
    for (int i = 0; i < sc->naliases; i++) if (ieq(s, n, sc->aliases[i].alias)) return sc->aliases[i].table;
    /* no alias: a table of this statement named whole or by its last part ("from HUMASTER.X where X.") */
    for (int i = 0; i < sc->ntables; i++) {
        const char *nm = TB(c, sc->tables[i])->name, *d = strrchr(nm, '.');
        if (ieq(s, n, nm) || (d && ieq(s, n, d + 1))) return sc->tables[i];
    }
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
            int col = find_column(TB(c, table), s + t->start, len);
            if (col >= 0) { snprintf(out, n, "c:%s.%s", TB(c, table)->name, TB(c, table)->cols[col].name); return; }
        }
        int tb = find_table_ex(c, s + t->start, len, false);   /* a key for usage: an identifier that happens to name an object must not start a column read */
        if (tb >= 0) { snprintf(out, n, "t:%s", TB(c, tb)->name); return; }
        for (int j = 0; j < sc->ntables; j++) {
            int col = find_column(TB(c, sc->tables[j]), s + t->start, len);
            if (col >= 0) { snprintf(out, n, "c:%s.%s", TB(c, sc->tables[j])->name, TB(c, sc->tables[j])->cols[col].name); return; }
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
    for (int k = 0; k < c->nxt; k++) if (c->xfk[k]) db_fks_free(c->xfk[k], c->nxfk[k]);
    free(c->fks); free(c->nfks); free(c->fks_state);
    free(c->tset); free(c->xt); free(c->xst); free(c->xfk); free(c->nxfk); free(c->xfst);
    dt_clear(c); free(c->dt);
    free(c);
}
bool complete_pending(const Completer *c) { return c->pending; }
void complete_set_catalog(Completer *c, const ObjCat *cat, const char *schema, CompleteTableFn table_fn, void *ctx) {
    for (int k = 0; k < c->nxt; k++) if (c->xfk[k]) db_fks_free(c->xfk[k], c->nxfk[k]);
    c->cat = cat; c->table_fn = table_fn; c->table_ctx = ctx; c->nxt = 0;
    snprintf(c->home, sizeof c->home, "%s", schema && schema[0] ? schema : cat ? cat->home : "");
}

/* Switch to another schema / connection: save and drop the old usage, borrow the new table array,
 * and load the usage file for `location` from LUME_PREF_DIR or SDL's pref path. */
void complete_set_schema(Completer *c, DbTable *tables, int ntables, const char *location,
                         int (*load_cols)(void *ctx, DbTable *t), void *ctx) {
    if (c->dirty) store_save(c);
    store_clear(c);
    for (int i = 0; c->fks && i < c->ntables; i++) if (c->fks[i]) db_fks_free(c->fks[i], c->nfks[i]);
    free(c->fks); free(c->nfks); free(c->fks_state); c->fks = NULL; c->nfks = NULL; c->fks_state = NULL;
    for (int k = 0; k < c->nxt; k++) if (c->xfk[k]) db_fks_free(c->xfk[k], c->nxfk[k]);
    c->tables = tables; c->ntables = ntables; c->nxt = 0;   /* catalog tables are numbered after these */
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

void complete_set_fk_loader(Completer *c, int (*load_fks)(void *ctx, const char *table, DbFk **out, int *n)) { c->load_fks = load_fks; }
/* the foreign keys of table t as known now (NULL / 0 until read) */
static DbFk *fks_of(Completer *c, int t, int *n) {
    if (t < 0 || t >= DT_BASE) { *n = 0; return NULL; }
    if (t < c->ntables) { *n = c->fks && c->fks_state[t] == 1 ? c->nfks[t] : 0; return *n ? c->fks[t] : NULL; }
    int k = t - c->ntables;
    *n = k < c->nxt && c->xfst[k] == 1 ? c->nxfk[k] : 0; return *n ? c->xfk[k] : NULL;
}
/* Reads t's foreign keys the first time (state 0), or collects them when they were asked for in the
 * background (state 2). A refusal leaves state 0: asked again next time. */
static void ensure_fks(Completer *c, int t) {
    if (!c->load_fks || t < 0 || t >= DT_BASE) return;
    DbFk **slot; int *nslot; unsigned char *st; DbTable *tb;
    if (t < c->ntables) { if (!c->fks) return; slot = &c->fks[t]; nslot = &c->nfks[t]; st = &c->fks_state[t]; tb = &c->tables[t]; }
    else { int k = t - c->ntables; if (k >= c->nxt) return; slot = &c->xfk[k]; nslot = &c->nxfk[k]; st = &c->xfst[k]; tb = c->xt[k]; }
    if (*st == 1 || tb->is_view) return;
    int r = c->load_fks(c->load_ctx, tb->name, slot, nslot);
    if (r > 0) *st = 1;
    else {
        *slot = NULL; *nslot = 0; *st = r == 0 ? 2 : 0;
        if (r == 0 && !c->pending) { c->pending = true; snprintf(c->pending_what, sizeof c->pending_what, "foreign keys"); snprintf(c->pending_name, sizeof c->pending_name, "%s", tb->name); }
    }
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
    dt_clear(c);
    analyse(c, s, tk, ntok, &sc, -1, NULL);
    char key[160], p1[128] = "", p2[128] = "";
    for (int i = 0; i < sc.ntables; i++) {
        snprintf(key, sizeof key, "t:%s", TB(c, sc.tables[i])->name);
        bump(c, key);
        for (int j = i + 1; j < sc.ntables; j++) {
            const char *a = TB(c, sc.tables[i])->name, *b = TB(c, sc.tables[j])->name;
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
    int worst;   /* index of the weakest candidate once out[] is full, -1 = to recompute */
    char *compact;   /* the statement without whitespace, lower-case: a condition already written is not proposed again */
} Gen;
/* true when the statement already contains cond (spaces and case aside) */
static bool stmt_has(Gen *g, const char *cond) {
    if (!g->compact) return false;
    char k[300]; int w = 0;
    for (const char *p = cond; *p && w + 1 < (int)sizeof k; p++) if (!isspace((unsigned char)*p)) k[w++] = (char)tolower((unsigned char)*p);
    k[w] = 0;
    return w > 0 && strstr(g->compact, k) != NULL;
}

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
/* The slot for a candidate scored (m, usage): a free one, else the weakest one when this is
 * better (a large catalog must not lose its best matches to whatever came first), else NULL. */
/* x ranks above y: match, then usage, then kind (a table before a synonym when nothing else differs) */
static bool ranks_above(int mx, float ux, int kx, int my, float uy, int ky) { return mx != my ? mx > my : ux != uy ? ux > uy : kx < ky; }
static void find_worst(Gen *g) {
    if (g->worst >= 0 && g->worst < g->n) return;
    int w = 0;
    for (int i = 1; i < g->n; i++) {
        const Cand *x = &g->out[w], *y = &g->out[i];
        if (ranks_above(x->match, x->usage, x->kind, y->match, y->usage, y->kind)) w = i;
        else if (x->match == y->match && x->usage == y->usage && x->kind == y->kind && strcasecmp(y->text, x->text) > 0) w = i;   /* a full tie: the one that would sort last */
    }
    g->worst = w;
}
/* would (m, usage, kind) get a place now */
static bool admits(Gen *g, int m, float usage, int kind) {
    if (g->n < g->max) return true;
    find_worst(g);
    const Cand *w = &g->out[g->worst];
    return ranks_above(m, usage, kind, w->match, w->usage, w->kind);
}
static Cand *slot(Gen *g, int m, float usage, int kind) {
    Cand *cd = NULL;
    if (g->n < g->max) { g->worst = -1; cd = &g->out[g->n++]; }
    else if (admits(g, m, usage, kind)) { cd = &g->out[g->worst]; g->worst = -1; }   /* replaced: recomputed next time */
    if (cd) { cd->label[0] = 0; cd->info = false; }   /* the array is reused between queries */
    return cd;
}
static void add_m(Gen *g, const char *text, const char *detail, const char *key, int kind, float usage, int m);
static void add(Gen *g, const char *text, const char *detail, const char *key, int kind, float usage) { add_m(g, text, detail, key, kind, usage, match_score(text, g->prefix, g->plen)); }
/* add with the match score already computed (a qualified column matched on its own name) */
static void add_m(Gen *g, const char *text, const char *detail, const char *key, int kind, float usage, int m) {
    if (!m) return;
    if (!admits(g, m, usage + 1e6f, kind)) return;   /* cheap pre-check before the n-gram lookups (n-grams only add usage) */
    usage += key ? ngram_usage(g, key) : 0;
    Cand *cd = slot(g, m, usage, kind); if (!cd) return;
    snprintf(cd->text, sizeof cd->text, "%s", text);
    snprintf(cd->detail, sizeof cd->detail, "%s", detail);
    snprintf(cd->key, sizeof cd->key, "%s", key ? key : "");
    cd->kind = kind;
    cd->match = m;
    cd->usage = usage;
}

/* Add every column of a table (type and PK flag in the detail). show_table is currently unused. */
static const char *qual_of(Scope *sc, Completer *c, int t);
/* Add every column of a table (type and PK flag in the detail). qualify: several tables in the
 * statement, so each column is written with its alias / table ("a.ID"), matched on its own name too. */
static void add_columns(Gen *g, int table, bool qualify) {
    DbTable *t = TB(g->c, table);
    ensure_cols(g->c, t);
    char key[300], detail[96], text[300];
    const char *q = qualify ? qual_of(g->sc, g->c, table) : NULL;
    int nf; DbFk *f = q ? fks_of(g->c, table, &nf) : NULL;   /* several tables: a key column ranks first among equals, and says what it points at */
    for (int i = 0; i < t->ncols; i++) {
        snprintf(key, sizeof key, "c:%s.%s", t->name, t->cols[i].name);
        const DbFk *fk = NULL; for (int k = 0; f && k < nf; k++) if (!strcasecmp(f[k].from_col, t->cols[i].name)) { fk = &f[k]; break; }
        char fkd[120] = ""; if (fk) { const char *tt = strrchr(fk->to_table, '.'); snprintf(fkd, sizeof fkd, " \xC2\xB7 FK \xE2\x86\x92 %s.%s", tt ? tt + 1 : fk->to_table, fk->to_col ? fk->to_col : ""); }
        if (q) snprintf(detail, sizeof detail, "%s \xC2\xB7 %s%s%s", q, t->cols[i].type, t->cols[i].pk ? " \xC2\xB7 PK" : "", fkd);   /* several tables: which one */
        else snprintf(detail, sizeof detail, "%s%s", t->cols[i].type, t->cols[i].pk ? " \xC2\xB7 PK" : "");
        float u = usage_of(g->c, key) + (q ? (t->cols[i].pk ? 0.5f : 0) + (fk ? 0.3f : 0) : 0);
        if (!q) { add(g, t->cols[i].name, detail, key, CAND_COLUMN, u); continue; }
        snprintf(text, sizeof text, "%s.%s", q, t->cols[i].name);
        int m = match_score(t->cols[i].name, g->prefix, g->plen), m2 = match_score(text, g->prefix, g->plen);
        add_m(g, text, detail, key, CAND_COLUMN, u, m > m2 ? m : m2);
    }
}
/* INSERT INTO t: every writable column at once, first in the list. paren: "(" is typed already. */
static void add_all_columns(Gen *g, int table, bool paren) {
    DbTable *t = TB(g->c, table);
    ensure_cols(g->c, t);
    if (!t->cols_loaded || t->ncols == 0 || g->n >= g->max) return;
    Cand *cd = &g->out[g->n++];
    memset(cd, 0, sizeof *cd);
    size_t w = 0; int k = 0, skipped = 0;
    if (!paren) w += (size_t)snprintf(cd->text + w, sizeof cd->text - w, "(");
    for (int i = 0; i < t->ncols; i++) {
        if (t->cols[i].identity_always || t->cols[i].generated) { skipped++; continue; }   /* the database refuses a value there */
        int need = snprintf(cd->text + w, sizeof cd->text - w, "%s%s", k ? ", " : "", t->cols[i].name);
        if (need < 0 || w + (size_t)need + 16 > sizeof cd->text) { g->n--; return; }   /* a list that does not fit is not offered as "every column" */
        w += (size_t)need; k++;
    }
    if (!k) { g->n--; return; }
    snprintf(cd->text + w, sizeof cd->text - w, paren ? ")" : ") VALUES (");
    snprintf(cd->label, sizeof cd->label, paren ? "all %d columns)" : "all %d columns, then VALUES (", k);
    { size_t d = 0; int shown = 0;   /* a preview of the list, then what was left out */
      for (int i = 0; i < t->ncols && shown < 4 && d < 60; i++) {
          if (t->cols[i].identity_always || t->cols[i].generated) continue;
          d += (size_t)snprintf(cd->detail + d, sizeof cd->detail - d, "%s%s", shown++ ? ", " : "", t->cols[i].name);
      }
      if (shown < k) d += (size_t)snprintf(cd->detail + d, sizeof cd->detail - d, ", \xE2\x80\xA6");
      if (skipped) snprintf(cd->detail + d, sizeof cd->detail - d, " \xC2\xB7 %d generated / identity left out", skipped); }
    cd->kind = CAND_COLUMN; cd->match = 100000; cd->usage = 1e9f;   /* always first */
}

/* Add every table and view; usage includes a bonus for tables often joined with those in scope. */
/* How a table is named in this statement: its alias, else its name without the schema. */
static const char *qual_of(Scope *sc, Completer *c, int t) {
    for (int i = 0; i < sc->naliases; i++) if (sc->aliases[i].table == t) return sc->aliases[i].alias;
    const char *n = TB(c, t)->name, *dot = strrchr(n, '.');
    return dot ? dot + 1 : n;
}
static bool same_table(const char *fk_target, const char *name) {
    const char *a = strrchr(fk_target, '.'), *b = strrchr(name, '.');
    return !strcasecmp(fk_target, name) || !strcasecmp(a ? a + 1 : fk_target, b ? b + 1 : name);
}
/* "JOIN b ON |": the conditions linking b to the tables before it. Real foreign keys first (every
 * pair of a composite key, either direction); then columns with exactly the same name that are a
 * primary key on at least one side (no naming convention assumed). */
static void add_join_conditions_ex(Gen *g, Scope *sc, int joined, int before_only) {
    Completer *c = g->c;
    char text[240], detail[96], done[24][240]; int nd = 0;
    const char *qb = qual_of(sc, c, joined);
    ensure_fks(c, joined);
    for (int si = 0; si < sc->ntables; si++) {
        int t = sc->tables[si]; if (t == joined) continue;
        if (before_only >= 0 && si >= before_only) continue;   /* a WHERE join: each table against the ones listed before it */
        const char *qa = qual_of(sc, c, t);
        ensure_fks(c, t);
        for (int dir = 0; dir < 2; dir++) {   /* 0: joined -> t, 1: t -> joined */
            int from = dir ? t : joined, to = dir ? joined : t;
            int nf; DbFk *f = fks_of(c, from, &nf);
            for (int i = 0; i < nf; i++) {
                if (!same_table(f[i].to_table, TB(c, to)->name)) continue;
                bool first = true; for (int q = 0; q < i; q++) if (f[q].name && f[i].name && !strcmp(f[q].name, f[i].name) && same_table(f[q].to_table, f[i].to_table)) first = false;
                if (!first) continue;
                size_t w = 0; int pairs = 0;
                for (int q = i; q < nf && w < sizeof text - 8; q++) {
                    bool same = q == i || (f[q].name && f[i].name && !strcmp(f[q].name, f[i].name));
                    if (!same || !same_table(f[q].to_table, f[i].to_table)) continue;
                    /* ON: written with the joined table first ("b.x = a.y"); WHERE: in FROM order (the earlier table first) */
                    bool joined_first = before_only < 0;
                    const char *lq = joined_first ? qb : qa, *rq = joined_first ? qa : qb;
                    const char *jc = f[q].from_col, *tc = f[q].to_col ? f[q].to_col : f[q].from_col;   /* from = dir ? t : joined */
                    const char *jcol = dir == 0 ? jc : tc, *tcol = dir == 0 ? tc : jc;   /* the joined table's column, the other's */
                    w += (size_t)snprintf(text + w, sizeof text - w, "%s%s.%s = %s.%s", pairs ? " AND " : "", lq, joined_first ? jcol : tcol, rq, joined_first ? tcol : jcol);
                    pairs++;
                }
                snprintf(detail, sizeof detail, "foreign key%s%s%s", f[i].name && f[i].name[0] ? " " : "", f[i].name ? f[i].name : "", pairs > 1 ? " (composite)" : "");
                if (nd < 24) snprintf(done[nd++], sizeof done[0], "%s", text);
                if (stmt_has(g, text)) continue;   /* written already */
                add(g, text, detail, NULL, CAND_COLUMN, 3.0f);
            }
        }
        /* same column name, a primary key on at least one side */
        ensure_cols(c, TB(c, joined)); ensure_cols(c, TB(c, t));
        DbTable *B = TB(c, joined), *A = TB(c, t);
        for (int i = 0; i < B->ncols; i++) for (int j = 0; j < A->ncols; j++) {
            if (strcasecmp(B->cols[i].name, A->cols[j].name) || !(B->cols[i].pk || A->cols[j].pk)) continue;
            if (B->cols[i].pk && A->cols[j].pk && !strcasecmp(B->cols[i].name, "id")) continue;   /* each table's own "id": not a relation */
            if (before_only < 0) snprintf(text, sizeof text, "%s.%s = %s.%s", qb, B->cols[i].name, qa, A->cols[j].name);
            else snprintf(text, sizeof text, "%s.%s = %s.%s", qa, A->cols[j].name, qb, B->cols[i].name);
            bool dup = false; for (int q = 0; q < nd; q++) if (strstr(done[q], text)) dup = true;
            if (dup || stmt_has(g, text)) continue;
            { char rev[240]; snprintf(rev, sizeof rev, "%s.%s = %s.%s", qa, A->cols[j].name, qb, B->cols[i].name); if (stmt_has(g, rev)) continue; }
            snprintf(detail, sizeof detail, "same column name (key in %s)", A->cols[j].pk ? qa : qb);
            add(g, text, detail, NULL, CAND_COLUMN, 1.0f);
        }
    }
}
static void add_join_conditions(Gen *g, Scope *sc, int joined) { add_join_conditions_ex(g, sc, joined, -1); }
/* "FROM a, b WHERE |": the conditions joining each table to the ones before it (the implicit join) */
static void add_where_joins(Gen *g, Scope *sc) {
    for (int si = 1; si < sc->ntables; si++) add_join_conditions_ex(g, sc, sc->tables[si], si);
}
/* "WHERE a.id = |": the columns of the other tables that pair with a.id: by foreign key first
 * (either direction), then same-name key columns. */
static void add_pair_columns(Gen *g, Scope *sc, int table, const char *col, int clen) {
    Completer *c = g->c;
    char text[240], detail[96];
    ensure_fks(c, table);
    for (int si = 0; si < sc->ntables; si++) {
        int t = sc->tables[si]; if (t == table) continue;
        const char *qt = qual_of(sc, c, t);
        ensure_fks(c, t);
        int nf; DbFk *f = fks_of(c, table, &nf);   /* table -> t */
        for (int i = 0; i < nf; i++) if (ieq(col, clen, f[i].from_col) && same_table(f[i].to_table, TB(c, t)->name)) {
            snprintf(text, sizeof text, "%s.%s", qt, f[i].to_col ? f[i].to_col : f[i].from_col);
            snprintf(detail, sizeof detail, "foreign key%s%s", f[i].name && f[i].name[0] ? " " : "", f[i].name ? f[i].name : "");
            add(g, text, detail, NULL, CAND_COLUMN, 3.0f);
        }
        f = fks_of(c, t, &nf);   /* t -> table */
        for (int i = 0; i < nf; i++) if (same_table(f[i].to_table, TB(c, table)->name) && ieq(col, clen, f[i].to_col ? f[i].to_col : f[i].from_col)) {
            snprintf(text, sizeof text, "%s.%s", qt, f[i].from_col);
            snprintf(detail, sizeof detail, "foreign key%s%s", f[i].name && f[i].name[0] ? " " : "", f[i].name ? f[i].name : "");
            add(g, text, detail, NULL, CAND_COLUMN, 3.0f);
        }
        DbTable *A = TB(c, table), *B = TB(c, t); ensure_cols(c, A); ensure_cols(c, B);
        int ca = -1; for (int i = 0; i < A->ncols; i++) if (ieq(col, clen, A->cols[i].name)) ca = i;
        for (int j = 0; ca >= 0 && j < B->ncols; j++) {
            if (strcasecmp(A->cols[ca].name, B->cols[j].name) || !(A->cols[ca].pk || B->cols[j].pk)) continue;
            if (A->cols[ca].pk && B->cols[j].pk && !strcasecmp(B->cols[j].name, "id")) continue;
            snprintf(text, sizeof text, "%s.%s", qt, B->cols[j].name);
            snprintf(detail, sizeof detail, "same column name (key in %s)", B->cols[j].pk ? qt : qual_of(sc, c, table));
            add(g, text, detail, NULL, CAND_COLUMN, 1.0f);
        }
    }
}
/* "JOIN b USING (|": the columns both sides have, by name */
static void add_using_columns(Gen *g, Scope *sc, int joined) {
    Completer *c = g->c;
    DbTable *B = TB(c, joined); ensure_cols(c, B);
    char detail[96];
    for (int si = 0; si < sc->ntables; si++) {
        int t = sc->tables[si]; if (t == joined) continue;
        DbTable *A = TB(c, t); ensure_cols(c, A);
        for (int i = 0; i < B->ncols; i++) for (int j = 0; j < A->ncols; j++) {
            if (strcasecmp(B->cols[i].name, A->cols[j].name)) continue;
            snprintf(detail, sizeof detail, "in %s and %s", qual_of(sc, c, joined), qual_of(sc, c, t));
            add(g, B->cols[i].name, detail, NULL, CAND_COLUMN, B->cols[i].pk || A->cols[j].pk ? 2.0f : 1.0f);
        }
    }
}
/* true when position i (a token index) is in a select list: the nearest clause keyword before it is SELECT */
static bool in_select_list(const char *s, Tok *tk, int i) {
    int depth = 0;
    for (int k = i - 1; k >= 0; k--) {
        if (tk[k].type == TK_RPAREN) depth++;
        else if (tk[k].type == TK_LPAREN) { if (depth == 0) return false; depth--; }
        else if (depth == 0 && tk[k].type == TK_KW) {
            if (kw_is(s, &tk[k], "SELECT") || kw_is(s, &tk[k], "DISTINCT")) return true;
            if (kw_is(s, &tk[k], "FROM") || kw_is(s, &tk[k], "WHERE") || kw_is(s, &tk[k], "ON") || kw_is(s, &tk[k], "BY") || kw_is(s, &tk[k], "SET") || kw_is(s, &tk[k], "HAVING") || kw_is(s, &tk[k], "VALUES") || kw_is(s, &tk[k], "INTO")) return false;
        }
    }
    return false;
}

static void add_tables(Gen *g) {
    char key[160];
    for (int i = 0; i < g->c->ntables; i++) {
        DbTable *t = &g->c->tables[i];
        snprintf(key, sizeof key, "t:%s", t->name);
        float u = usage_of(g->c, key);
        /* tables used together with the ones already in the statement rank first */
        for (int j = 0; j < g->sc->ntables; j++) {
            const char *a = t->name, *b = TB(g->c, g->sc->tables[j])->name;
            if (g->sc->tables[j] == i) continue;
            if (strcmp(a, b) > 0) { const char *x = a; a = b; b = x; }
            char jk[200];
            snprintf(jk, sizeof jk, "j:%s+%s", a, b);
            u += 3 * usage_of(g->c, jk);
        }
        add(g, t->name, t->is_view ? "view" : "table", key, t->is_view ? CAND_VIEW : CAND_TABLE, u);
    }
}

/* ---- every visible object (objcat) ---- */

static uint32_t lc_hash(const char *s, int n) { uint32_t h = 2166136261u; for (int i = 0; i < n && s[i]; i++) { h ^= (unsigned char)tolower((unsigned char)s[i]); h *= 16777619u; } return h; }
/* the names of `tables` in a hash set (rebuilt when the table array changes) */
static void tset_build(Completer *c) {
    if (c->tset && c->tset_tables == c->tables && c->tset_n == c->ntables) return;
    free(c->tset); c->tset = NULL; c->tset_cap = 0; c->tset_tables = c->tables; c->tset_n = c->ntables;
    if (c->ntables <= 0) return;
    int cap = 16; while (cap < c->ntables * 2) cap *= 2;
    c->tset = calloc((size_t)cap, sizeof *c->tset); if (!c->tset) return;
    c->tset_cap = cap;
    for (int i = 0; i < c->ntables; i++) {
        uint32_t h = lc_hash(c->tables[i].name, 1 << 30) & (uint32_t)(cap - 1);
        while (c->tset[h]) h = (h + 1) & (uint32_t)(cap - 1);
        c->tset[h] = i + 1;
    }
}
/* the n bytes at s name one of `tables` (case-insensitive, as written there) */
static bool tset_has(Completer *c, const char *s, int n) {
    if (!c->tset) return false;
    uint32_t h = lc_hash(s, n) & (uint32_t)(c->tset_cap - 1);
    for (; c->tset[h]; h = (h + 1) & (uint32_t)(c->tset_cap - 1)) if (ieq(s, n, c->tables[c->tset[h] - 1].name)) return true;
    return false;
}
/* usage of "t:SCHEMA.NAME" without building the key (the catalog loop runs it per object) */
static bool key_is(const char *k, const char *schema, const char *name) {
    if (k[0] != 't' || k[1] != ':') return false;
    k += 2; size_t sl = strlen(schema);
    if (strncmp(k, schema, sl) || k[sl] != '.') return false;
    return !strcmp(k + sl + 1, name);
}
static float obj_usage(Completer *c, const char *schema, const char *name, double now) {
    if (!c->nent) return 0;
    uint32_t h = 2166136261u;
    for (const char *p = "t:"; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    for (const char *p = schema; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    h ^= (unsigned char)'.'; h *= 16777619u;
    for (const char *p = name; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    for (uint32_t i = h & (uint32_t)(c->cap - 1);; i = (i + 1) & (uint32_t)(c->cap - 1)) {
        if (!c->ent[i].key) return 0;
        if (key_is(c->ent[i].key, schema, name)) return value_of(&c->ent[i], now);
    }
}
/* tset_has for "SCHEMA.NAME" without building it */
static bool tset_has_q(Completer *c, const char *schema, const char *name) {
    if (!c->tset) return false;
    uint32_t h = 2166136261u;
    for (const char *p = schema; *p; p++) { h ^= (unsigned char)tolower((unsigned char)*p); h *= 16777619u; }
    h ^= (unsigned char)'.'; h *= 16777619u;
    for (const char *p = name; *p; p++) { h ^= (unsigned char)tolower((unsigned char)*p); h *= 16777619u; }
    size_t sl = strlen(schema);
    for (uint32_t i = h & (uint32_t)(c->tset_cap - 1); c->tset[i]; i = (i + 1) & (uint32_t)(c->tset_cap - 1)) {
        const char *t = c->tables[c->tset[i] - 1].name;
        if (!strncasecmp(t, schema, sl) && t[sl] == '.' && !strcasecmp(t + sl + 1, name)) return true;
    }
    return false;
}
static int cand_kind(int ok) { return ok == OC_TABLE ? CAND_TABLE : ok == OC_VIEW || ok == OC_MVIEW ? CAND_VIEW : CAND_OBJECT; }
/* Objects of the catalog, or of one schema (`only`: inserted without the schema, it is typed
 * already). Unqualified: the current schema's and PUBLIC synonyms; others as SCHEMA.NAME, matched
 * on the name and on the whole. Objects the schema cache already proposes are skipped. */
/* "25,728": a count with thousands separators (locale-free, like the grid's fmt_int) */
static void thousands(char *o, size_t n, int v) {
    char raw[24]; int l = snprintf(raw, sizeof raw, "%d", v), k = 0;
    for (int i = 0; i < l && k + 1 < (int)n; i++) { if (i && (l - i) % 3 == 0 && raw[i - 1] != '-') o[k++] = ','; o[k++] = raw[i]; }
    o[k] = 0;
}
static void add_catalog(Gen *g, const OcSchema *only, bool relations) {
    Completer *c = g->c; const ObjCat *k = c->cat;
    if (!k) return;
    tset_build(c);
    char key[260], detail[96];
    if (!only) for (int i = 0; i < k->ns; i++) {
        const OcSchema *s = &k->s[i];
        if (!strcasecmp(s->name, "PUBLIC") || !s->name[0]) continue;
        int m = match_score(s->name, g->prefix, g->plen); if (!m) continue;
        snprintf(key, sizeof key, "s:%s", s->name);
        float u = usage_of(c, key);
        Cand *cd = slot(g, m, u, CAND_SCHEMA); if (!cd) continue;
        snprintf(cd->text, sizeof cd->text, "%s", s->name);
        char cnt[24]; thousands(cnt, sizeof cnt, s->n);
        snprintf(cd->detail, sizeof cd->detail, "schema \xC2\xB7 %s object%s \xE2\x80\xBA", cnt, s->n == 1 ? "" : "s");   /* › : Enter lists them */
        snprintf(cd->key, sizeof cd->key, "%s", key);
        cd->kind = CAND_SCHEMA; cd->match = m; cd->usage = u;
    }
    int from = only ? only->first : 0, to = only ? only->first + only->n : k->n;
    if (!only && g->plen == 0) {   /* nothing typed (Ctrl+Space): the schemas and this schema's objects, not the whole catalog */
        const OcSchema *h = objcat_schema(k, c->home, (int)strlen(c->home));
        if (!h) return;
        from = h->first; to = h->first + h->n;
    }
    unsigned pmask = objcat_mask(g->prefix, g->plen);
    double now = now_s();
    for (int i = from; i < to; i++) {
        const OcObj *o = &k->o[i];
        if (relations && !is_relation(o->kind)) continue;   /* after FROM / JOIN: what can be queried */
        if ((o->mask & pmask) != pmask) continue;   /* a letter typed is not in the name */
        int m = match_score(o->name, g->prefix, g->plen);
        if (!m) continue;
        int ck = cand_kind(o->kind);
        if (g->n >= g->max) { find_worst(g); if (m < g->out[g->worst].match) continue; }   /* full: a weaker match cannot get in, skip the rest */
        float u = obj_usage(c, o->schema, o->name, now);
        if (!admits(g, m, u, ck)) continue;   /* a full tie does not replace */
        bool unq = only || !strcasecmp(o->schema, c->home) || !strcmp(o->schema, "PUBLIC") || !o->schema[0];
        if (!only && c->tset) {   /* the schema cache proposes its own tables already (as NAME or SCHEMA.NAME) */
            if (unq && tset_has(c, o->name, (int)strlen(o->name))) continue;
            if (o->schema[0] && tset_has_q(c, o->schema, o->name)) continue;
        }
        Cand *cd = slot(g, m, u, ck); if (!cd) continue;
        snprintf(key, sizeof key, "t:%s.%s", o->schema, o->name);
        if (only || unq) snprintf(cd->text, sizeof cd->text, "%s", o->name); else snprintf(cd->text, sizeof cd->text, "%s.%s", o->schema, o->name);
        if (o->kind == OC_SYNONYM && o->target) snprintf(detail, sizeof detail, "synonym \xE2\x86\x92 %s", o->target);
        else snprintf(detail, sizeof detail, "%s", objcat_kind_name(o->kind));
        snprintf(cd->detail, sizeof cd->detail, "%s", detail);
        snprintf(cd->key, sizeof cd->key, "%s", key);
        cd->kind = ck; cd->match = m; cd->usage = u;
    }
}

/* Add keywords (lower-cased when `lower_kw`, to follow the user's style) and functions as "name(". */
static void add_words(Gen *g, bool lower_kw) {
    char buf[64];
    for (int i = 0; i < NKEYWORDS; i++) {
        int k = 0;
        for (; keywords[i][k]; k++) buf[k] = lower_kw ? (char)tolower((unsigned char)keywords[i][k]) : keywords[i][k];
        buf[k] = 0;
        /* a keyword is written in the case the user types: "same case" earns it nothing over a column */
        int m = match_score(buf, g->prefix, g->plen); if (m > 400) m = 400;
        add_m(g, buf, "keyword", keywords[i], CAND_KEYWORD, usage_of(g->c, keywords[i]) * 0.01f, m);
    }
    for (int i = 0; i < NFUNCTIONS; i++) {
        snprintf(buf, sizeof buf, "%s(", functions[i]);
        /* match on the bare name so "cou" finds count( */
        int m = match_score(functions[i], g->prefix, g->plen); if (m > 400) m = 400;   /* like keywords */
        if (!m || g->n >= g->max) continue;
        Cand *cd = &g->out[g->n++];
        cd->label[0] = 0; cd->info = false;
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
    c->pending = false; c->pending_name[0] = 0;
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
    dt_clear(c);
    {   /* the query level the caret is in: its own tables first, then the enclosing levels' */
        int stack[32], ns = 0;
        for (int i = 0; i < nall && all[i].start < cursor; i++) {
            if (all[i].type == TK_LPAREN) { if (ns < 32) stack[ns++] = i; }
            else if (all[i].type == TK_RPAREN && all[i].end <= cursor && ns > 0) ns--;
        }
        int L = ns ? stack[ns - 1] + 1 : 0, R = ns ? match_paren(all, nall, stack[ns - 1]) : nall;
        if (ns) analyse_range(c, s, all, 0, nall, &sc, -1, NULL, 0);   /* the top level first: its CTEs and derived tables get defined */
        analyse_range(c, s, all, L, R, &sc, -1, NULL, 0);
        for (int lv = ns - 1; lv >= 0; lv--) {   /* enclosing levels, nearest first */
            int oL = lv ? stack[lv - 1] + 1 : 0, oR = lv ? match_paren(all, nall, stack[lv - 1]) : nall;
            Scope outer; analyse_range(c, s, all, oL, oR, &outer, -1, NULL, 0);
            for (int i = 0; i < outer.ntables; i++) scope_add(&sc, outer.tables[i]);
            for (int i = 0; i < outer.naliases; i++) { bool dup = false; for (int k = 0; k < sc.naliases; k++) if (!strcasecmp(sc.aliases[k].alias, outer.aliases[i].alias)) dup = true; if (!dup && sc.naliases < MAX_SCOPE) sc.aliases[sc.naliases++] = outer.aliases[i]; }
        }
    }
    if (prev >= 0) {
        Scope tmp;
        analyse(c, s, tk, ntok, &tmp, prev, &table_ctx);
    }

    Gen g = { c, out, 0, max, s + ps, plen, &sc, "", "", -1, NULL };
    { g.compact = malloc((size_t)len + 1); int w = 0; for (int i = 0; g.compact && i < len; i++) if (!isspace((unsigned char)s[i])) g.compact[w++] = (char)tolower((unsigned char)s[i]); if (g.compact) g.compact[w] = 0; }
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
    /* INSERT INTO t | / INSERT INTO t ( |: every column first */
    if (plen == 0 && prev >= 1) {
        int j = prev; bool paren = tk[j].type == TK_LPAREN; if (paren) j--;
        int end = j;
        if (j >= 0 && tk[j].type == TK_IDENT) {
            while (j >= 2 && tk[j - 1].type == TK_DOT && tk[j - 2].type == TK_IDENT) j -= 2;
            if (j >= 1 && kw_is(s, &tk[j - 1], "INTO") && (paren || prev == end)) {
                int t = find_table(c, s + tk[j].start, tk[end].end - tk[j].start);
                if (t >= 0) add_all_columns(&g, t, paren);
            }
        }
    }
    if (prev >= 1 && tk[prev].type == TK_DOT && tk[prev - 1].type == TK_IDENT) {
        int q0 = prev - 1;   /* the whole qualifier: "a.", "X.", "HUMASTER.X." */
        while (q0 >= 2 && tk[q0 - 1].type == TK_DOT && tk[q0 - 2].type == TK_IDENT) q0 -= 2;
        int table = resolve_qualifier(c, &sc, s + tk[q0].start, tk[prev - 1].end - tk[q0].start);
        if (table >= 0) {
            if (sc.ntables > 1 && plen == 0 && in_select_list(s, tk, q0)) {   /* "a.": every column of a, first, when several tables are in play */
                Cand *cd = slot(&g, 100000, 1e9f, CAND_COLUMN);
                if (cd) { snprintf(cd->text, sizeof cd->text, "*"); snprintf(cd->detail, sizeof cd->detail, "every column of %s", TB(c, table)->name); cd->key[0] = 0; cd->kind = CAND_COLUMN; cd->match = 100000; cd->usage = 1e9f; }
            }
            add_columns(&g, table, false);
        }
        else if (q0 == prev - 1) {   /* "SCHEMA.": that schema's objects */
            const OcSchema *sch = objcat_schema(c->cat, s + tk[prev - 1].start, tk[prev - 1].end - tk[prev - 1].start);
            bool rel = q0 >= 1 && (expects_table(s, &tk[q0 - 1]) || (tk[q0 - 1].type == TK_COMMA && table_ctx_at(c, s, tk, q0 - 1)));
            if (sch) add_catalog(&g, sch, rel);
        }
    } else if (prev >= 0 && table_ctx) {
        add_tables(&g);
        add_catalog(&g, NULL, true);
    } else if (prev >= 1 && tk[prev].type == TK_LPAREN && kw_is(s, &tk[prev - 1], "USING") && sc.ntables >= 2) {
        /* "JOIN b USING (": the columns both sides share */
        int joined = -1;
        for (int i = prev - 2; i >= 0 && joined < 0; i--) if (kw_is(s, &tk[i], "JOIN") && i + 1 < ntok && tk[i + 1].type == TK_IDENT) {
            int j = i + 1; while (j + 2 < ntok && tk[j + 1].type == TK_DOT && tk[j + 2].type == TK_IDENT) j += 2;
            joined = find_table(c, s + tk[i + 1].start, tk[j].end - tk[i + 1].start);
        }
        if (joined >= 0) add_using_columns(&g, &sc, joined);
    } else if (prev >= 1 && tk[prev].type == TK_OP && tk[prev].end - tk[prev].start == 1 && s[tk[prev].start] == '=' && tk[prev - 1].type == TK_IDENT && sc.ntables >= 2 && plen == 0) {
        /* "WHERE a.id = ": what pairs with a.id in the other tables (the implicit join) */
        int ci = prev - 1, table = -1;
        if (ci >= 2 && tk[ci - 1].type == TK_DOT && tk[ci - 2].type == TK_IDENT) {
            int q0 = ci - 2; while (q0 >= 2 && tk[q0 - 1].type == TK_DOT && tk[q0 - 2].type == TK_IDENT) q0 -= 2;
            table = resolve_qualifier(c, &sc, s + tk[q0].start, tk[ci - 2].end - tk[q0].start);
        } else for (int i = 0; i < sc.ntables && table < 0; i++) { DbTable *t = TB(c, sc.tables[i]); ensure_cols(c, t); for (int k = 0; k < t->ncols; k++) if (ieq(s + tk[ci].start, tk[ci].end - tk[ci].start, t->cols[k].name)) table = sc.tables[i]; }
        if (table >= 0) add_pair_columns(&g, &sc, table, s + tk[ci].start, tk[ci].end - tk[ci].start);
        if (explicit || !g.n) for (int i = 0; i < sc.ntables; i++) add_columns(&g, sc.tables[i], sc.ntables > 1);
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
        /* "UPDATE t SET ", "WHERE ", "INSERT INTO t (": columns of the tables in scope; with several
         * tables listed, "WHERE " / "AND " offer the join conditions first (the implicit join) */
        if (sc.ntables >= 2 && (kw_is(s, &tk[prev], "WHERE") || kw_is(s, &tk[prev], "AND") || kw_is(s, &tk[prev], "OR"))) add_where_joins(&g, &sc);
        for (int i = 0; i < sc.ntables; i++) add_columns(&g, sc.tables[i], sc.ntables > 1);
        add_extra(&g);
    } else if (plen > 0 || explicit) {
        if (plen == 0 && prev >= 0 && sc.ntables >= 2 && (kw_is(s, &tk[prev], "WHERE") || kw_is(s, &tk[prev], "AND") || kw_is(s, &tk[prev], "OR"))) add_where_joins(&g, &sc);
        add_extra(&g);
        for (int i = 0; i < sc.ntables; i++) add_columns(&g, sc.tables[i], sc.ntables > 1);
        char key[160];
        for (int i = 0; i < sc.naliases; i++) {
            if ((int)strlen(sc.aliases[i].alias) == plen && strncasecmp(sc.aliases[i].alias, s + ps, plen) == 0) continue;
            snprintf(key, sizeof key, "alias of %s", TB(c, sc.aliases[i].table)->name);
            add(&g, sc.aliases[i].alias, key, NULL, CAND_ALIAS, 0);
        }
        add_tables(&g);
        add_catalog(&g, NULL, false);
        add_words(&g, lower_kw);
    }
    free(tk);
    free(all);
    free(g.compact);

    /* rank */
    g_has_prefix = plen > 0;
    qsort(out, g.n, sizeof *out, cmp_cand);
    if (c->pending && max > 0) {   /* columns on their way: a row says so (nothing is inserted from it) */
        if (g.n == max) g.n--;
        memmove(out + 1, out, sizeof *out * (size_t)g.n);
        memset(&out[0], 0, sizeof out[0]);
        snprintf(out[0].label, sizeof out[0].label, "Loading %s of %.100s\xE2\x80\xA6", c->pending_what, c->pending_name);
        out[0].info = true; out[0].kind = CAND_KEYWORD;
        g.n++;
    }
    /* a single proposal that already equals the prefix is noise */
    if (g.n == 1 && plen > 0 && (int)strlen(out[0].text) == plen && strncmp(out[0].text, g.prefix, plen) == 0) return 0;
    return g.n;
}
