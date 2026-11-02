/* SPDX-License-Identifier: AGPL-3.0-only */
/* Semantic catalog and the deterministic query compiler: see sem.h. */
#include "sem.h"
#include "jw.h"
#include "sqllex.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char *const AGG[] = { "sum", "count", "count_distinct", "avg", "min", "max" };
static const char *const UNIT[] = { "number", "currency", "percent", "duration" };
static const char *const GRAIN[] = { "day", "week", "month", "quarter", "year" };
static const char *const CERT[] = { "draft", "certified", "deprecated" };
static const char *const ROLE[] = { "dimension", "measure", "time", "key", "other" };

const char *sem_agg_name(int a) { return a >= 0 && a < SA_N ? AGG[a] : "?"; }
const char *sem_unit_name(int u) { return u >= 0 && u < SU_N ? UNIT[u] : "?"; }
const char *sem_grain_name(int g) { return g >= 0 && g < SG_N ? GRAIN[g] : "?"; }
const char *sem_cert_name(int c) { return c >= 0 && c <= SC_DEPRECATED ? CERT[c] : "?"; }
static int idx_of(const char *const *l, int n, const char *s) { for (int i = 0; i < n; i++) if (!strcasecmp(l[i], s)) return i; return -1; }
int sem_grain_by_name(const char *s) { return s ? idx_of(GRAIN, SG_N, s) : -1; }

void sem_init(SemCatalog *c) { memset(c, 0, sizeof *c); c->format_version = 1; }

static void cp(char *d, size_t n, const char *s) { snprintf(d, n, "%s", s ? s : ""); }
#define CP(d, s) cp(d, sizeof(d), s)

/* ---- identifiers ---- */
static bool ident_ok(const char *s) {
    if (!s || !*s || strlen(s) > 63) return false;
    for (const char *p = s; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '$' || *p == '-' || *p == '.' || *p == ' ')) return false;
    return true;
}
static bool id_ok(const char *s) {
    if (!s || !*s || strlen(s) > 47) return false;
    for (const char *p = s; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    return true;
}

/* ---- JSON ---- */
static int list_bits(const Json *j, int arr, const char *const *names, int n) {
    unsigned bits = 0;
    for (int i = 0;; i++) { int c = jr_child(j, arr, i); if (c < 0) break; char b[24]; jr_node_str(j, c, b, sizeof b); int k = idx_of(names, n, b); if (k >= 0) bits |= 1u << k; }
    return (int)bits;
}
static bool parse_def(const Json *j, int o, SemMetricDef *d, char *err, int en) {
    memset(d, 0, sizeof *d);
    long long v = 1; jr_int(j, o, "version", &v); d->version = (int)v;
    jr_str(j, o, "name", d->name, sizeof d->name);
    jr_str(j, o, "description", d->description, sizeof d->description);
    jr_str(j, o, "owner", d->owner, sizeof d->owner);
    jr_str(j, o, "dataset", d->dataset, sizeof d->dataset);
    char b[48];
    jr_str(j, o, "aggregation", b, sizeof b); d->aggregation = idx_of(AGG, SA_N, b);
    if (d->aggregation < 0) { snprintf(err, (size_t)en, "metric %s: unknown aggregation '%s'", d->name, b); return false; }
    jr_str(j, o, "column", d->column, sizeof d->column);
    int fa = jr_get(j, o, "filters");
    for (int i = 0; d->nfilters < SEM_MAX_FILTERS; i++) {
        int f = jr_child(j, fa, i); if (f < 0) break;
        SemFilter *x = &d->filters[d->nfilters++];
        jr_str(j, f, "column", x->column, sizeof x->column); jr_str(j, f, "op", x->op, sizeof x->op); jr_str(j, f, "value", x->value, sizeof x->value);
    }
    jr_str(j, o, "unit", b, sizeof b); d->unit = b[0] ? idx_of(UNIT, SU_N, b) : SU_NUMBER;
    if (d->unit < 0) { snprintf(err, (size_t)en, "metric %s: unknown unit '%s'", d->name, b); return false; }
    jr_str(j, o, "currency", d->currency, sizeof d->currency);
    long long dec = -1; jr_int(j, o, "decimals", &dec); d->decimals = (int)dec;
    int ga = jr_get(j, o, "grains"); d->grains = ga >= 0 ? (unsigned)list_bits(j, ga, GRAIN, SG_N) : (1u << SG_DAY) | (1u << SG_MONTH) | (1u << SG_QUARTER) | (1u << SG_YEAR);
    jr_str(j, o, "certification", b, sizeof b); d->certification = b[0] ? idx_of(CERT, 3, b) : SC_DRAFT; if (d->certification < 0) d->certification = SC_DRAFT;
    long long dir = 0; jr_int(j, o, "direction", &dir); d->direction = dir > 0 ? 1 : dir < 0 ? -1 : 0;
    jr_str(j, o, "changedAt", d->changed_at, sizeof d->changed_at);
    jr_str(j, o, "changeNote", d->change_note, sizeof d->change_note);
    return true;
}
bool sem_parse(SemCatalog *c, const char *text, size_t len) {
    char path[1024]; CP(path, c->path);
    sem_init(c); CP(c->path, path);
    Json j; memset(&j, 0, sizeof j);
    if (!json_parse(&j, text, len)) { snprintf(c->err, sizeof c->err, "semantic catalog: invalid JSON at byte %zu (%s)", j.err_off, j.err); json_free(&j); return false; }
    long long fv = 0;
    if (!jr_int(&j, 0, "formatVersion", &fv) || fv < 1) { snprintf(c->err, sizeof c->err, "semantic catalog: formatVersion missing"); json_free(&j); return false; }
    if (fv > 1) { snprintf(c->err, sizeof c->err, "semantic catalog: formatVersion %lld is newer than this Lume (1): opened read-only, nothing changed", fv); json_free(&j); return false; }
    c->format_version = (int)fv;
    int da = jr_get(&j, 0, "datasets");
    for (int i = 0; c->nds < SEM_MAX_DATASETS; i++) {
        int o = jr_child(&j, da, i); if (o < 0) break;
        SemDataset *d = &c->ds[c->nds++];
        jr_str(&j, o, "id", d->id, sizeof d->id); jr_str(&j, o, "name", d->name, sizeof d->name);
        jr_str(&j, o, "connection", d->conn, sizeof d->conn); jr_str(&j, o, "schema", d->schema, sizeof d->schema);
        jr_str(&j, o, "table", d->table, sizeof d->table); jr_str(&j, o, "timeColumn", d->time_column, sizeof d->time_column);
        jr_str(&j, o, "timezone", d->timezone, sizeof d->timezone);
        int ca = jr_get(&j, o, "columns");
        for (int k = 0; d->ncols < SEM_MAX_COLS; k++) {
            int co = jr_child(&j, ca, k); if (co < 0) break;
            SemColumn *x = &d->cols[d->ncols++];
            jr_str(&j, co, "name", x->name, sizeof x->name); jr_str(&j, co, "label", x->label, sizeof x->label);
            jr_str(&j, co, "type", x->type, sizeof x->type); jr_str(&j, co, "description", x->description, sizeof x->description);
            char r[16]; jr_str(&j, co, "role", r, sizeof r); x->role = r[0] ? idx_of(ROLE, 5, r) : SR_OTHER; if (x->role < 0) x->role = SR_OTHER;
            bool rs = false; jr_bool(&j, co, "restricted", &rs); x->restricted = rs;
        }
    }
    int ma = jr_get(&j, 0, "metrics");
    for (int i = 0; c->nm < SEM_MAX_METRICS; i++) {
        int o = jr_child(&j, ma, i); if (o < 0) break;
        SemMetric *m = &c->m[c->nm++];
        jr_str(&j, o, "id", m->id, sizeof m->id);
        int sa = jr_get(&j, o, "synonyms");
        for (int k = 0; m->nsyn < SEM_MAX_SYN; k++) { int s = jr_child(&j, sa, k); if (s < 0) break; jr_node_str(&j, s, m->synonyms[m->nsyn++], sizeof m->synonyms[0]); }
        int va = jr_get(&j, o, "versions");
        for (int k = 0; m->nv < SEM_MAX_VERSIONS; k++) {
            int vo = jr_child(&j, va, k); if (vo < 0) break;
            if (!parse_def(&j, vo, &m->v[m->nv], c->err, sizeof c->err)) { json_free(&j); return false; }
            m->nv++;
        }
    }
    json_free(&j);
    char why[300];
    if (!sem_validate(c, why, sizeof why)) { snprintf(c->err, sizeof c->err, "semantic catalog: %s", why); return false; }
    return true;
}
bool sem_load(SemCatalog *c, const char *path) {
    sem_init(c); CP(c->path, path);
    FILE *f = fopen(path, "rb");
    if (!f) return true;   /* no catalog yet */
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 8 * 1024 * 1024) { fclose(f); snprintf(c->err, sizeof c->err, "semantic catalog too large"); return false; }
    char *b = malloc((size_t)n + 1); size_t r = fread(b, 1, (size_t)n, f); fclose(f); b[r] = 0;
    bool ok = sem_parse(c, b, r);
    CP(c->path, path);
    free(b);
    return ok;
}
char *sem_to_json(const SemCatalog *c) {
    Jw j; jw_init(&j);
    jw_obj(&j);
    jw_kint(&j, "formatVersion", 1);
    jw_key(&j, "datasets"); jw_arr(&j);
    for (int i = 0; i < c->nds; i++) {
        const SemDataset *d = &c->ds[i];
        jw_obj(&j);
        jw_kstr(&j, "id", d->id); jw_kstr(&j, "name", d->name); jw_kstr(&j, "connection", d->conn);
        jw_kstr(&j, "schema", d->schema); jw_kstr(&j, "table", d->table); jw_kstr(&j, "timeColumn", d->time_column); jw_kstr(&j, "timezone", d->timezone);
        jw_key(&j, "columns"); jw_arr(&j);
        for (int k = 0; k < d->ncols; k++) {
            const SemColumn *x = &d->cols[k];
            jw_obj(&j); jw_kstr(&j, "name", x->name); jw_kstr(&j, "label", x->label); jw_kstr(&j, "type", x->type);
            jw_kstr(&j, "role", ROLE[x->role]); if (x->restricted) jw_kbool(&j, "restricted", true);
            if (x->description[0]) jw_kstr(&j, "description", x->description);
            jw_end_obj(&j);
        }
        jw_end_arr(&j);
        jw_end_obj(&j);
    }
    jw_end_arr(&j);
    jw_key(&j, "metrics"); jw_arr(&j);
    for (int i = 0; i < c->nm; i++) {
        const SemMetric *m = &c->m[i];
        jw_obj(&j); jw_kstr(&j, "id", m->id);
        jw_key(&j, "synonyms"); jw_arr(&j); for (int k = 0; k < m->nsyn; k++) jw_str(&j, m->synonyms[k]); jw_end_arr(&j);
        jw_key(&j, "versions"); jw_arr(&j);
        for (int k = 0; k < m->nv; k++) {
            const SemMetricDef *d = &m->v[k];
            jw_obj(&j);
            jw_kint(&j, "version", d->version); jw_kstr(&j, "name", d->name); jw_kstr(&j, "description", d->description); jw_kstr(&j, "owner", d->owner);
            jw_kstr(&j, "dataset", d->dataset); jw_kstr(&j, "aggregation", AGG[d->aggregation]); jw_kstr(&j, "column", d->column);
            jw_key(&j, "filters"); jw_arr(&j);
            for (int f = 0; f < d->nfilters; f++) { jw_obj(&j); jw_kstr(&j, "column", d->filters[f].column); jw_kstr(&j, "op", d->filters[f].op); jw_kstr(&j, "value", d->filters[f].value); jw_end_obj(&j); }
            jw_end_arr(&j);
            jw_kstr(&j, "unit", UNIT[d->unit]); if (d->currency[0]) jw_kstr(&j, "currency", d->currency);
            if (d->decimals >= 0) jw_kint(&j, "decimals", d->decimals);
            jw_key(&j, "grains"); jw_arr(&j); for (int g = 0; g < SG_N; g++) if (d->grains & (1u << g)) jw_str(&j, GRAIN[g]); jw_end_arr(&j);
            jw_kstr(&j, "certification", CERT[d->certification]);
            if (d->direction) jw_kint(&j, "direction", d->direction);
            if (d->changed_at[0]) jw_kstr(&j, "changedAt", d->changed_at);
            if (d->change_note[0]) jw_kstr(&j, "changeNote", d->change_note);
            jw_end_obj(&j);
        }
        jw_end_arr(&j);
        jw_end_obj(&j);
    }
    jw_end_arr(&j);
    jw_end_obj(&j);
    return jw_take(&j);
}
bool sem_save(SemCatalog *c, const char *path) {
    char *s = sem_to_json(c);
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(s); snprintf(c->err, sizeof c->err, "cannot write %s", tmp); return false; }
    size_t L = strlen(s); bool ok = fwrite(s, 1, L, f) == L;
    ok = (fflush(f) == 0) && ok; ok = (fsync(fileno(f)) == 0) && ok; ok = (fclose(f) == 0) && ok;
    free(s);
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); snprintf(c->err, sizeof c->err, "cannot save the semantic catalog"); return false; }
    CP(c->path, path);
    return true;
}

/* ---- lookups ---- */
SemDataset *sem_dataset(SemCatalog *c, const char *id) { for (int i = 0; id && i < c->nds; i++) if (!strcmp(c->ds[i].id, id)) return &c->ds[i]; return NULL; }
const SemDataset *sem_dataset_c(const SemCatalog *c, const char *id) { return sem_dataset((SemCatalog *)c, id); }
SemMetric *sem_metric(SemCatalog *c, const char *id) { for (int i = 0; id && i < c->nm; i++) if (!strcmp(c->m[i].id, id)) return &c->m[i]; return NULL; }
const SemMetric *sem_metric_c(const SemCatalog *c, const char *id) { return sem_metric((SemCatalog *)c, id); }
const SemMetricDef *sem_cur(const SemMetric *m) { return m && m->nv ? &m->v[m->nv - 1] : NULL; }
const SemMetricDef *sem_version(const SemMetric *m, int version) { for (int i = 0; m && i < m->nv; i++) if (m->v[i].version == version) return &m->v[i]; return NULL; }
const SemColumn *sem_column(const SemDataset *d, const char *name) { for (int i = 0; d && name && i < d->ncols; i++) if (!strcmp(d->cols[i].name, name)) return &d->cols[i]; return NULL; }
bool sem_is_dimension(const SemDataset *d, const char *name) {
    const SemColumn *x = sem_column(d, name);
    return x && !x->restricted && (x->role == SR_DIMENSION || x->role == SR_TIME);
}

static bool def_valid(const SemCatalog *c, const SemMetricDef *d, char *why, int n) {
    const SemDataset *ds = sem_dataset_c(c, d->dataset);
    if (!d->name[0]) { snprintf(why, (size_t)n, "a metric needs a name"); return false; }
    if (!ds) { snprintf(why, (size_t)n, "metric %s: dataset '%s' is not in the catalog", d->name, d->dataset); return false; }
    if (d->aggregation < 0 || d->aggregation >= SA_N) { snprintf(why, (size_t)n, "metric %s: invalid aggregation", d->name); return false; }
    if (d->aggregation != SA_COUNT && !d->column[0]) { snprintf(why, (size_t)n, "metric %s: %s needs a column", d->name, AGG[d->aggregation]); return false; }
    if (d->column[0]) {
        const SemColumn *x = sem_column(ds, d->column);
        if (!x) { snprintf(why, (size_t)n, "metric %s: column '%s' is not declared in dataset %s", d->name, d->column, ds->id); return false; }
        if ((d->aggregation == SA_SUM || d->aggregation == SA_AVG) && strcmp(x->type, "number")) { snprintf(why, (size_t)n, "metric %s: %s of a %s column", d->name, AGG[d->aggregation], x->type[0] ? x->type : "non-numeric"); return false; }
    }
    for (int i = 0; i < d->nfilters; i++) {
        const SemFilter *f = &d->filters[i];
        if (!sem_column(ds, f->column)) { snprintf(why, (size_t)n, "metric %s: filter column '%s' is not declared", d->name, f->column); return false; }
        static const char *const ops[] = { "=", "<>", "!=", ">", ">=", "<", "<=", NULL };
        bool ok = false; for (int k = 0; ops[k]; k++) if (!strcmp(f->op, ops[k])) ok = true;
        if (!ok) { snprintf(why, (size_t)n, "metric %s: filter operator '%s' is not allowed", d->name, f->op); return false; }
    }
    if (d->unit == SU_CURRENCY && strlen(d->currency) != 3) { snprintf(why, (size_t)n, "metric %s: a currency metric needs an ISO currency code", d->name); return false; }
    if (!d->grains) { snprintf(why, (size_t)n, "metric %s: no granularity allowed", d->name); return false; }
    return true;
}
bool sem_validate(const SemCatalog *c, char *why, int n) {
    for (int i = 0; i < c->nds; i++) {
        const SemDataset *d = &c->ds[i];
        if (!id_ok(d->id)) { snprintf(why, (size_t)n, "dataset id '%s' is invalid", d->id); return false; }
        for (int k = 0; k < i; k++) if (!strcmp(c->ds[k].id, d->id)) { snprintf(why, (size_t)n, "dataset id '%s' is duplicated", d->id); return false; }
        if (!d->conn[0] || !ident_ok(d->table)) { snprintf(why, (size_t)n, "dataset %s needs a connection and a table", d->id); return false; }
        if (d->schema[0] && !ident_ok(d->schema)) { snprintf(why, (size_t)n, "dataset %s: invalid schema name", d->id); return false; }
        for (int k = 0; k < d->ncols; k++) if (!ident_ok(d->cols[k].name)) { snprintf(why, (size_t)n, "dataset %s: invalid column name", d->id); return false; }
        if (d->time_column[0] && !sem_column(d, d->time_column)) { snprintf(why, (size_t)n, "dataset %s: time column '%s' is not declared", d->id, d->time_column); return false; }
    }
    for (int i = 0; i < c->nm; i++) {
        const SemMetric *m = &c->m[i];
        if (!id_ok(m->id)) { snprintf(why, (size_t)n, "metric id '%s' is invalid", m->id); return false; }
        for (int k = 0; k < i; k++) if (!strcmp(c->m[k].id, m->id)) { snprintf(why, (size_t)n, "metric id '%s' is duplicated", m->id); return false; }
        if (!m->nv) { snprintf(why, (size_t)n, "metric %s has no version", m->id); return false; }
        for (int k = 0; k < m->nv; k++) {
            if (k && m->v[k].version <= m->v[k - 1].version) { snprintf(why, (size_t)n, "metric %s: versions must increase", m->id); return false; }
            if (!def_valid(c, &m->v[k], why, n)) return false;
        }
    }
    return true;
}

bool sem_metric_edit(SemCatalog *c, const char *id, const SemMetricDef *def, char *why, int n) {
    SemMetric *m = sem_metric(c, id);
    if (!m) {
        if (!id_ok(id)) { snprintf(why, (size_t)n, "invalid metric id"); return false; }
        if (c->nm >= SEM_MAX_METRICS) { snprintf(why, (size_t)n, "the catalog is full"); return false; }
        m = &c->m[c->nm]; memset(m, 0, sizeof *m); CP(m->id, id);
        if (!def_valid(c, def, why, n)) return false;
        c->nm++;
    } else if (!def_valid(c, def, why, n)) return false;
    int next = m->nv ? m->v[m->nv - 1].version + 1 : 1;
    if (m->nv == SEM_MAX_VERSIONS) { memmove(&m->v[0], &m->v[1], sizeof m->v[0] * (SEM_MAX_VERSIONS - 1)); m->nv--; }
    m->v[m->nv] = *def; m->v[m->nv].version = next; m->nv++;
    return true;
}

/* ---- search ---- */
void sem_fold(const char *in, char *out, int n) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && w < n - 1; p++) {
        if (*p == 0xC3 && p[1]) {
            unsigned c = p[1] | 0x20;   /* lower half of Latin-1 letters */
            char m = 0;
            if (c >= 0xA0 && c <= 0xA5) m = 'a'; else if (c == 0xA7) m = 'c'; else if (c >= 0xA8 && c <= 0xAB) m = 'e';
            else if (c >= 0xAC && c <= 0xAF) m = 'i'; else if (c == 0xB1) m = 'n'; else if ((c >= 0xB2 && c <= 0xB6) || c == 0xB8) m = 'o';
            else if (c >= 0xB9 && c <= 0xBC) m = 'u'; else if (c == 0xBD || c == 0xBF) m = 'y';
            if (m) { out[w++] = m; p++; continue; }
        }
        out[w++] = (char)(isalnum(*p) ? tolower(*p) : *p >= 0x80 ? *p : ' ');
    }
    out[w] = 0;
}
typedef struct Occ { int metric, start, end; char term[48]; } Occ;
static int find_word(const char *hay, const char *needle, int from) {
    int nl = (int)strlen(needle); if (!nl) return -1;
    for (const char *p = strstr(hay + from, needle); p; p = strstr(p + 1, needle)) {
        int s = (int)(p - hay);
        bool lb = s == 0 || hay[s - 1] == ' ';
        char after = hay[s + nl];
        bool rb = after == 0 || after == ' ' || (after == 's' && (hay[s + nl + 1] == 0 || hay[s + nl + 1] == ' '));   /* plural */
        if (lb && rb) return s;
    }
    return -1;
}
static int occurrences(const SemCatalog *c, const char *text, Occ *occ, int max) {
    char t[1024]; sem_fold(text, t, sizeof t);
    int n = 0;
    for (int i = 0; i < c->nm; i++) {
        const SemMetricDef *d = sem_cur(&c->m[i]);
        if (!d || d->certification == SC_DEPRECATED) continue;
        char terms[SEM_MAX_SYN + 2][96]; int nt = 0;
        sem_fold(d->name, terms[nt++], 96);
        sem_fold(c->m[i].id, terms[nt], 96); for (char *x = terms[nt]; *x; x++) if (*x == '_' || *x == '-') *x = ' '; nt++;
        for (int k = 0; k < c->m[i].nsyn; k++) sem_fold(c->m[i].synonyms[k], terms[nt++], 96);
        for (int k = 0; k < nt; k++) {
            if (!terms[k][0]) continue;
            for (int s = find_word(t, terms[k], 0); s >= 0 && n < max; s = find_word(t, terms[k], s + 1)) {
                occ[n].metric = i; occ[n].start = s; occ[n].end = s + (int)strlen(terms[k]); CP(occ[n].term, terms[k]); n++;
            }
        }
    }
    /* a match inside a longer match of another metric does not count ("faturamento" inside "faturamento liquido") */
    int w = 0;
    for (int i = 0; i < n; i++) {
        bool inside = false;
        for (int k = 0; k < n && !inside; k++) if (k != i && occ[k].start <= occ[i].start && occ[k].end >= occ[i].end && (occ[k].end - occ[k].start) > (occ[i].end - occ[i].start)) inside = true;
        if (!inside) occ[w++] = occ[i];
    }
    return w;
}
int sem_search(const SemCatalog *c, const char *text, SemMatch *out, int max) {
    Occ occ[256]; int n = occurrences(c, text, occ, 256);
    int m = 0;
    for (int i = 0; i < n; i++) {
        int k; for (k = 0; k < m; k++) if (out[k].metric == occ[i].metric) break;
        int score = occ[i].end - occ[i].start;
        if (k < m) { if (score > out[k].score) { out[k].score = score; CP(out[k].term, occ[i].term); } continue; }
        if (m >= max) continue;
        out[m].metric = occ[i].metric; out[m].score = score; CP(out[m].term, occ[i].term); m++;
    }
    for (int i = 1; i < m; i++) for (int k = i; k > 0 && out[k].score > out[k - 1].score; k--) { SemMatch x = out[k]; out[k] = out[k - 1]; out[k - 1] = x; }
    return m;
}
bool sem_ambiguous(const SemCatalog *c, const char *text, char options[][48], int max, int *nopt, char *term, int tn) {
    Occ occ[256]; int n = occurrences(c, text, occ, 256);
    *nopt = 0; if (tn) term[0] = 0;
    for (int i = 0; i < n; i++) {
        int same = 0;
        for (int k = 0; k < n; k++) if (occ[k].start == occ[i].start && occ[k].end == occ[i].end && occ[k].metric != occ[i].metric) same++;
        if (!same) continue;
        /* every metric matched on exactly this span */
        for (int k = 0; k < n && *nopt < max; k++) {
            if (occ[k].start != occ[i].start || occ[k].end != occ[i].end) continue;
            bool dup = false; for (int q = 0; q < *nopt; q++) if (!strcmp(options[q], c->m[occ[k].metric].id)) dup = true;
            if (!dup) CP(options[(*nopt)++], c->m[occ[k].metric].id);
        }
        snprintf(term, (size_t)tn, "%s", occ[i].term);
        return true;
    }
    return false;
}

bool sem_compatible(const SemCatalog *c, const char *const *ids, int n, char *why, int wn) {
    const SemMetricDef *first = NULL;
    for (int i = 0; i < n; i++) {
        const SemMetricDef *d = sem_cur(sem_metric_c(c, ids[i]));
        if (!d) { snprintf(why, (size_t)wn, "metric '%s' is not in the catalog", ids[i]); return false; }
        if (!first) { first = d; continue; }
        if (d->unit != first->unit) { snprintf(why, (size_t)wn, "%s (%s) and %s (%s) have different units: one axis cannot show both", first->name, UNIT[first->unit], d->name, UNIT[d->unit]); return false; }
        if (d->unit == SU_CURRENCY && strcasecmp(d->currency, first->currency)) { snprintf(why, (size_t)wn, "%s is in %s and %s in %s: convert first (no implicit exchange rate)", first->name, first->currency, d->name, d->currency); return false; }
    }
    return true;
}

/* ---- SQL ---- */
typedef struct Sb { char *b; int n, w; bool over; } Sb;
static void sb(Sb *s, const char *t) { int l = (int)strlen(t); if (s->w + l >= s->n) { s->over = true; return; } memcpy(s->b + s->w, t, (size_t)l); s->w += l; s->b[s->w] = 0; }

void sem_quote_ident(int dialect, const char *id, char *out, int n) {
    char q0 = '"', q1 = '"';
    if (dialect == DIALECT_MYSQL || dialect == DIALECT_MARIADB) q0 = q1 = '`';
    else if (dialect == DIALECT_MSSQL) { q0 = '['; q1 = ']'; }
    int w = 0; if (w < n - 1) out[w++] = q0;
    for (const char *p = id; *p && w < n - 3; p++) { if (*p == q1) out[w++] = q1; out[w++] = *p; }
    out[w++] = q1; out[w] = 0;
}
bool sem_iso_date_ok(const char *s) {
    if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i = 0; i < 10; i++) if (i != 4 && i != 7 && !isdigit((unsigned char)s[i])) return false;
    int m = atoi(s + 5), d = atoi(s + 8);
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}
void sem_date_literal(int dialect, const char *iso, char *out, int n) {
    if (dialect == DIALECT_MSSQL) snprintf(out, (size_t)n, "CAST('%s' AS date)", iso);
    else if (dialect == DIALECT_SQLITE) snprintf(out, (size_t)n, "'%s'", iso);
    else snprintf(out, (size_t)n, "DATE '%s'", iso);
}
static void str_literal(const char *v, char *out, int n) {
    int w = 0; if (n < 3) { out[0] = 0; return; }
    out[w++] = '\'';
    for (const char *p = v; *p && w < n - 3; p++) { if (*p == '\'') out[w++] = '\''; out[w++] = *p; }
    out[w++] = '\''; out[w] = 0;
}
static bool is_number(const char *s) {
    if (!*s) return false;
    char *e; strtod(s, &e);
    if (*e) return false;
    for (const char *p = s; *p; p++) if (!(isdigit((unsigned char)*p) || *p == '.' || *p == '-' || *p == '+')) return false;
    return true;
}
static void grain_expr(int dialect, int g, const char *col, char *out, int n) {
    Sb s = { out, n, 0, false }; out[0] = 0;
    switch (dialect) {
    case DIALECT_PG: {
        static const char *const u[] = { "day", "week", "month", "quarter", "year" };
        sb(&s, "to_char(date_trunc('"); sb(&s, u[g]); sb(&s, "', "); sb(&s, col); sb(&s, "), 'YYYY-MM-DD')");
        break; }
    case DIALECT_SQLITE:
        if (g == SG_DAY) { sb(&s, "date("); sb(&s, col); sb(&s, ")"); }
        else if (g == SG_WEEK) { sb(&s, "date("); sb(&s, col); sb(&s, ", '-' || ((CAST(strftime('%w', "); sb(&s, col); sb(&s, ") AS INTEGER) + 6) % 7) || ' days')"); }
        else if (g == SG_MONTH) { sb(&s, "strftime('%Y-%m-01', "); sb(&s, col); sb(&s, ")"); }
        else if (g == SG_QUARTER) { sb(&s, "printf('%s-%02d-01', strftime('%Y', "); sb(&s, col); sb(&s, "), ((CAST(strftime('%m', "); sb(&s, col); sb(&s, ") AS INTEGER) - 1) / 3) * 3 + 1)"); }
        else { sb(&s, "strftime('%Y-01-01', "); sb(&s, col); sb(&s, ")"); }
        break;
    case DIALECT_MYSQL: case DIALECT_MARIADB:
        if (g == SG_DAY) { sb(&s, "DATE_FORMAT("); sb(&s, col); sb(&s, ", '%Y-%m-%d')"); }
        else if (g == SG_WEEK) { sb(&s, "DATE_FORMAT(DATE_SUB(DATE("); sb(&s, col); sb(&s, "), INTERVAL WEEKDAY("); sb(&s, col); sb(&s, ") DAY), '%Y-%m-%d')"); }
        else if (g == SG_MONTH) { sb(&s, "DATE_FORMAT("); sb(&s, col); sb(&s, ", '%Y-%m-01')"); }
        else if (g == SG_QUARTER) { sb(&s, "DATE_FORMAT(MAKEDATE(YEAR("); sb(&s, col); sb(&s, "), 1) + INTERVAL QUARTER("); sb(&s, col); sb(&s, ") - 1 QUARTER, '%Y-%m-%d')"); }
        else { sb(&s, "DATE_FORMAT("); sb(&s, col); sb(&s, ", '%Y-01-01')"); }
        break;
    case DIALECT_MSSQL:
        sb(&s, "CONVERT(char(10), ");
        if (g == SG_DAY) { sb(&s, "CAST("); sb(&s, col); sb(&s, " AS date)"); }
        else if (g == SG_WEEK) { sb(&s, "DATEADD(day, -((DATEPART(weekday, "); sb(&s, col); sb(&s, ") + @@DATEFIRST - 2) % 7), CAST("); sb(&s, col); sb(&s, " AS date))"); }
        else if (g == SG_MONTH) { sb(&s, "DATEFROMPARTS(YEAR("); sb(&s, col); sb(&s, "), MONTH("); sb(&s, col); sb(&s, "), 1)"); }
        else if (g == SG_QUARTER) { sb(&s, "DATEFROMPARTS(YEAR("); sb(&s, col); sb(&s, "), (DATEPART(quarter, "); sb(&s, col); sb(&s, ") - 1) * 3 + 1, 1)"); }
        else { sb(&s, "DATEFROMPARTS(YEAR("); sb(&s, col); sb(&s, "), 1, 1)"); }
        sb(&s, ", 23)");
        break;
    case DIALECT_ORACLE: {
        static const char *const u[] = { "DD", "IW", "MM", "Q", "YYYY" };
        sb(&s, "TO_CHAR(TRUNC("); sb(&s, col); sb(&s, ", '"); sb(&s, u[g]); sb(&s, "'), 'YYYY-MM-DD')");
        break; }
    default: sb(&s, col);
    }
}

bool sem_compile(const SemCatalog *c, const SemQuery *q, int dialect, char *out, int n, char *why, int wn) {
    out[0] = 0;
    const SemMetric *m = sem_metric_c(c, q->metric);
    if (!m) { snprintf(why, (size_t)wn, "metric '%s' is not in the catalog", q->metric ? q->metric : ""); return false; }
    const SemMetricDef *d = q->version ? sem_version(m, q->version) : sem_cur(m);
    if (!d) { snprintf(why, (size_t)wn, "metric %s version %d is no longer kept in the catalog", m->id, q->version); return false; }
    const SemDataset *ds = sem_dataset_c(c, d->dataset);
    if (!ds) { snprintf(why, (size_t)wn, "dataset %s is not in the catalog", d->dataset); return false; }
    bool has_dim = q->dim && q->dim[0];
    if (has_dim && !sem_is_dimension(ds, q->dim)) { snprintf(why, (size_t)wn, "'%s' is not an authorized dimension of %s", q->dim, ds->name[0] ? ds->name : ds->id); return false; }
    if (q->grain >= 0) {
        if (q->grain >= SG_N) { snprintf(why, (size_t)wn, "unknown granularity"); return false; }
        if (!ds->time_column[0]) { snprintf(why, (size_t)wn, "dataset %s has no time column: no time series", ds->id); return false; }
        if (!(d->grains & (1u << q->grain))) { snprintf(why, (size_t)wn, "%s is not defined per %s (allowed by the metric's owner only)", d->name, GRAIN[q->grain]); return false; }
        if (q->grain == SG_WEEK && dialect == DIALECT_GENERIC) { snprintf(why, (size_t)wn, "weekly grain is not available for this engine"); return false; }
    }
    bool has_period = q->period.from[0] || q->period.to[0];
    if (has_period && !ds->time_column[0]) { snprintf(why, (size_t)wn, "dataset %s has no time column: a period filter cannot apply", ds->id); return false; }
    if ((q->period.from[0] && !sem_iso_date_ok(q->period.from)) || (q->period.to[0] && !sem_iso_date_ok(q->period.to))) { snprintf(why, (size_t)wn, "invalid period date"); return false; }
    bool has_filter = q->filter_col && q->filter_col[0];
    if (has_filter && !sem_is_dimension(ds, q->filter_col)) { snprintf(why, (size_t)wn, "filter field '%s' is not an authorized dimension of %s", q->filter_col, ds->id); return false; }

    char tcol[160] = "", dcol[160] = "", acol[160] = "", from[400], b[600];
    if (ds->time_column[0]) sem_quote_ident(dialect, ds->time_column, tcol, sizeof tcol);
    if (has_dim) sem_quote_ident(dialect, q->dim, dcol, sizeof dcol);
    if (d->column[0]) sem_quote_ident(dialect, d->column, acol, sizeof acol);
    if (ds->schema[0]) { char a[160], t[200]; sem_quote_ident(dialect, ds->schema, a, sizeof a); sem_quote_ident(dialect, ds->table, t, sizeof t); snprintf(from, sizeof from, "%s.%s", a, t); }
    else sem_quote_ident(dialect, ds->table, from, sizeof from);

    char agg[300];
    switch (d->aggregation) {
    case SA_SUM: snprintf(agg, sizeof agg, "SUM(%s)", acol); break;
    case SA_COUNT: snprintf(agg, sizeof agg, d->column[0] ? "COUNT(%s)" : "COUNT(*)", acol); break;
    case SA_COUNT_DISTINCT: snprintf(agg, sizeof agg, "COUNT(DISTINCT %s)", acol); break;
    case SA_AVG: snprintf(agg, sizeof agg, dialect == DIALECT_MSSQL ? "AVG(%s * 1.0)" : "AVG(%s)", acol); break;   /* SQL Server's AVG(int) truncates */
    case SA_MIN: snprintf(agg, sizeof agg, "MIN(%s)", acol); break;
    default: snprintf(agg, sizeof agg, "MAX(%s)", acol); break;
    }
    char gexpr[600] = "";
    if (q->grain >= 0) grain_expr(dialect, q->grain, tcol, gexpr, sizeof gexpr);

    Sb s = { out, n, 0, false };
    sb(&s, "SELECT ");
    if (q->limit > 0 && dialect == DIALECT_MSSQL) { snprintf(b, sizeof b, "TOP (%d) ", q->limit); sb(&s, b); }
    if (gexpr[0]) { sb(&s, gexpr); sb(&s, " AS p, "); }
    if (has_dim) { sb(&s, dcol); sb(&s, " AS d, "); }
    sb(&s, agg); sb(&s, " AS v FROM "); sb(&s, from);
    int nw = 0;
    #define AND() sb(&s, nw++ ? " AND " : " WHERE ")
    if (q->period.from[0]) { AND(); sem_date_literal(dialect, q->period.from, b, sizeof b); sb(&s, tcol); sb(&s, " >= "); sb(&s, b); }
    if (q->period.to[0]) { AND(); sem_date_literal(dialect, q->period.to, b, sizeof b); sb(&s, tcol); sb(&s, " < "); sb(&s, b); }
    for (int i = 0; i < d->nfilters; i++) {
        const SemFilter *f = &d->filters[i];
        const SemColumn *x = sem_column(ds, f->column);
        char col[160], lit[220];
        sem_quote_ident(dialect, f->column, col, sizeof col);
        if (x && !strcmp(x->type, "number") && is_number(f->value)) snprintf(lit, sizeof lit, "%s", f->value); else str_literal(f->value, lit, sizeof lit);
        AND(); sb(&s, col); sb(&s, " "); sb(&s, !strcmp(f->op, "!=") ? "<>" : f->op); sb(&s, " "); sb(&s, lit);
    }
    if (has_filter) { char col[160], lit[300]; sem_quote_ident(dialect, q->filter_col, col, sizeof col); str_literal(q->filter_val ? q->filter_val : "", lit, sizeof lit); AND(); sb(&s, col); sb(&s, " = "); sb(&s, lit); }
    #undef AND
    if (gexpr[0] || has_dim) {
        sb(&s, " GROUP BY ");
        if (gexpr[0]) sb(&s, gexpr);
        if (has_dim) { if (gexpr[0]) sb(&s, ", "); sb(&s, dcol); }
        sb(&s, " ORDER BY ");
        if (gexpr[0]) { sb(&s, "1"); if (has_dim) sb(&s, ", 2"); }
        else sb(&s, dialect == DIALECT_MSSQL || dialect == DIALECT_ORACLE ? "v DESC" : "v DESC");
    }
    if (q->limit > 0 && dialect != DIALECT_MSSQL) {
        if (dialect == DIALECT_ORACLE) snprintf(b, sizeof b, " FETCH FIRST %d ROWS ONLY", q->limit);
        else snprintf(b, sizeof b, " LIMIT %d", q->limit);
        sb(&s, b);
    }
    if (s.over) { snprintf(why, (size_t)wn, "query too long"); out[0] = 0; return false; }
    return true;
}

char *sem_context(const SemCatalog *c, bool names) {
    Jw j; jw_init(&j);
    jw_obj(&j);
    jw_key(&j, "metrics"); jw_arr(&j);
    for (int i = 0; i < c->nm; i++) {
        const SemMetricDef *d = sem_cur(&c->m[i]);
        if (!d || d->certification == SC_DEPRECATED) continue;
        const SemDataset *ds = sem_dataset_c(c, d->dataset);
        jw_obj(&j);
        jw_kstr(&j, "id", c->m[i].id); jw_kint(&j, "version", d->version); jw_kstr(&j, "name", d->name);
        jw_kstr(&j, "description", d->description); jw_kstr(&j, "unit", UNIT[d->unit]); if (d->currency[0]) jw_kstr(&j, "currency", d->currency);
        jw_kstr(&j, "aggregation", AGG[d->aggregation]); jw_kstr(&j, "certification", CERT[d->certification]);
        jw_key(&j, "synonyms"); jw_arr(&j); for (int k = 0; k < c->m[i].nsyn; k++) jw_str(&j, c->m[i].synonyms[k]); jw_end_arr(&j);
        jw_key(&j, "grains"); jw_arr(&j); for (int g = 0; g < SG_N; g++) if (d->grains & (1u << g)) jw_str(&j, GRAIN[g]); jw_end_arr(&j);
        jw_kbool(&j, "timeSeries", ds && ds->time_column[0]);
        jw_key(&j, "dimensions"); jw_arr(&j);
        for (int k = 0; ds && k < ds->ncols; k++) if (sem_is_dimension(ds, ds->cols[k].name) && ds->cols[k].role == SR_DIMENSION) {
            jw_obj(&j); jw_kstr(&j, "id", ds->cols[k].name); jw_kstr(&j, "label", ds->cols[k].label[0] ? ds->cols[k].label : ds->cols[k].name); jw_end_obj(&j);
        }
        jw_end_arr(&j);
        if (names && ds) { jw_kstr(&j, "dataset", ds->id); jw_kstr(&j, "table", ds->table); }
        jw_end_obj(&j);
    }
    jw_end_arr(&j);
    jw_end_obj(&j);
    return jw_take(&j);
}
