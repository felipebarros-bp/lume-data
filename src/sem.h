/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Semantic catalog (LI-018..027): authorized datasets, versioned business metrics, synonyms and the
 * deterministic SQL compiler behind every business number in the Report Studio.
 *
 *   The model never writes business SQL. It names catalog ids (metric, dimension, grain, period);
 *   Lume validates them and compiles the query here from the curated definition, so "Receita
 *   líquida" is computed the same way in every card, report and engine (spec 2, "Decisões").
 *
 * Storage: <prefs>/semantic.json (formatVersion 1). A metric edit creates a new version and keeps
 * the previous ones (LI-019.2 / 026): reports store the version they were validated with.
 * Datasets declare the only tables and columns a business query may touch (LI-018 / 078);
 * columns marked "restricted" never go to the model and are not offered as dimensions (LI-086).
 *
 * Headless: no UI, no connection. Identifiers are quoted per dialect; literals come only from the
 * catalog (curated) or from Lume (ISO dates it formats itself), never from the model.
 */
#include <stdbool.h>
#include <stddef.h>

#define SEM_MAX_DATASETS 32
#define SEM_MAX_COLS 64
#define SEM_MAX_METRICS 64
#define SEM_MAX_VERSIONS 8
#define SEM_MAX_SYN 12
#define SEM_MAX_FILTERS 4

enum { SA_SUM, SA_COUNT, SA_COUNT_DISTINCT, SA_AVG, SA_MIN, SA_MAX, SA_N };
enum { SU_NUMBER, SU_CURRENCY, SU_PERCENT, SU_DURATION, SU_N };
enum { SC_DRAFT, SC_CERTIFIED, SC_DEPRECATED };
enum { SG_DAY, SG_WEEK, SG_MONTH, SG_QUARTER, SG_YEAR, SG_N };
enum { SR_DIMENSION, SR_MEASURE, SR_TIME, SR_KEY, SR_OTHER };

typedef struct SemColumn {
    char name[64];            /* physical column */
    char label[64];           /* business label */
    char type[16];            /* text, number, date, timestamp, bool */
    int role;                 /* SR_* */
    bool restricted;          /* PII / confidential: never in the AI context, never a dimension */
    char description[200];    /* curated; database comments are NOT copied here (untrusted) */
} SemColumn;

typedef struct SemDataset {
    char id[48];
    char name[80];
    char conn[16];            /* connection id (connections.txt), never a DSN or credential */
    char schema[64], table[128];
    char time_column[64];
    char timezone[40];        /* how timestamps are stored ("UTC"); shown with every period */
    SemColumn cols[SEM_MAX_COLS]; int ncols;
} SemDataset;

typedef struct SemFilter { char column[64]; char op[8]; char value[96]; } SemFilter;   /* = <> > >= < <= ; value is a curated literal */

typedef struct SemMetricDef {
    int version;
    char name[80];
    char description[300];
    char owner[48];
    char dataset[48];
    int aggregation;          /* SA_* */
    char column[64];          /* "" for COUNT(*) */
    SemFilter filters[SEM_MAX_FILTERS]; int nfilters;
    int unit;                 /* SU_* */
    char currency[4];         /* ISO 4217 when unit == currency */
    int decimals;             /* -1 = automatic */
    unsigned grains;          /* bit SG_* allowed */
    int certification;        /* SC_* */
    int direction;            /* +1 higher is better, -1 lower is better, 0 unknown (no green/red) */
    char changed_at[24];      /* ISO date of this version */
    char change_note[160];
} SemMetricDef;

typedef struct SemMetric {
    char id[48];
    char synonyms[SEM_MAX_SYN][48]; int nsyn;   /* search aids, not rules (LI-020.2) */
    SemMetricDef v[SEM_MAX_VERSIONS]; int nv;    /* v[nv-1] is current */
} SemMetric;

typedef struct SemCatalog {
    int format_version;
    SemDataset ds[SEM_MAX_DATASETS]; int nds;
    SemMetric m[SEM_MAX_METRICS]; int nm;
    char path[1024];
    char err[300];
} SemCatalog;

const char *sem_agg_name(int a);          /* "sum" */
const char *sem_unit_name(int u);         /* "currency" */
const char *sem_grain_name(int g);        /* "month" */
const char *sem_cert_name(int c);
int sem_grain_by_name(const char *s);     /* -1 unknown */

void sem_init(SemCatalog *c);
bool sem_load(SemCatalog *c, const char *path);             /* missing file = empty catalog, true */
bool sem_parse(SemCatalog *c, const char *text, size_t len); /* false: c->err */
bool sem_save(SemCatalog *c, const char *path);             /* atomic (tmp + rename) */
char *sem_to_json(const SemCatalog *c);                     /* malloc'd */
/* structural validation: ids, datasets, columns, aggregations, units (LI-019 / 023) */
bool sem_validate(const SemCatalog *c, char *why, int n);

SemDataset *sem_dataset(SemCatalog *c, const char *id);
const SemDataset *sem_dataset_c(const SemCatalog *c, const char *id);
SemMetric *sem_metric(SemCatalog *c, const char *id);
const SemMetric *sem_metric_c(const SemCatalog *c, const char *id);
const SemMetricDef *sem_cur(const SemMetric *m);
const SemMetricDef *sem_version(const SemMetric *m, int version);   /* NULL when not kept */
const SemColumn *sem_column(const SemDataset *d, const char *name);
/* a dimension usable for grouping: declared, not restricted, role dimension or time (LI-025 lite) */
bool sem_is_dimension(const SemDataset *d, const char *name);

/* new version of a metric (LI-019.2): the def is copied, version = previous + 1, the oldest
 * version is dropped beyond SEM_MAX_VERSIONS; false with why when invalid */
bool sem_metric_edit(SemCatalog *c, const char *id, const SemMetricDef *def, char *why, int n);

/* ---- search and ambiguity (LI-020 / 021) ---- */
typedef struct SemMatch { int metric; int score; char term[48]; } SemMatch;
/* metrics whose name / synonyms appear in the text (accent- and case-insensitive); best first */
int sem_search(const SemCatalog *c, const char *text, SemMatch *out, int max);
/* ambiguous when two or more different metrics match the same best term; options get their ids */
bool sem_ambiguous(const SemCatalog *c, const char *text, char options[][48], int max, int *nopt, char *term, int tn);
void sem_fold(const char *in, char *out, int n);   /* lowercase ASCII, Latin-1 accents removed */

/* ---- compatibility (LI-023) ---- */
/* the metrics may share one chart axis: same unit, same currency; why otherwise */
bool sem_compatible(const SemCatalog *c, const char *const *ids, int n, char *why, int wn);

/* ---- query compiler (LI-028 / 040 / 041 / 042) ---- */
typedef struct SemPeriod { char from[11], to[11]; } SemPeriod;   /* [from, to) ISO dates; "" = open */
typedef struct SemQuery {
    const char *metric; int version;   /* version 0 = current */
    const char *dim;          /* "" none */
    int grain;                /* -1 none, else SG_* over the dataset's time column */
    SemPeriod period;
    const char *filter_col, *filter_val;   /* one page / report filter (dimension = value), optional */
    int limit;                /* categories / rows cap, 0 = none */
} SemQuery;
/* SELECT [period AS p,] [dim AS d,] AGG(...) AS v FROM ... WHERE ... GROUP BY ... ORDER BY ...
 * dialect: sqllex DIALECT_*. false with why when the request is not valid for the catalog. */
bool sem_compile(const SemCatalog *c, const SemQuery *q, int dialect, char *out, int n, char *why, int wn);
/* identifier quoting and an ISO date literal per dialect */
void sem_quote_ident(int dialect, const char *id, char *out, int n);
void sem_date_literal(int dialect, const char *iso, char *out, int n);
bool sem_iso_date_ok(const char *s);

/* ---- context for the model (LI-010 / 032 / 079 / 084) ---- */
/* metric ids, names, descriptions, units, grains and non-restricted dimension names; no rows, no
 * connection, no table names unless names is true. malloc'd. */
char *sem_context(const SemCatalog *c, bool names);
