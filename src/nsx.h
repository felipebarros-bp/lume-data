/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Namespace explorer model: "Schemas" + "Others" (Schema Explorer v2, DBX-077..126)
 * =================================================================================
 *
 * What the Explorer shows under a connection's "Schemas" (MySQL / MariaDB: "Databases") node,
 * without ever listing the whole catalog:
 *
 *   featured   the current schema, pinned schemas, recent ones and a capped number of automatic
 *              highlights (owners already known to have visible objects); sorted by name
 *   Others     everything else: names are read one page at a time (keyset on the name), only
 *              when Others is expanded; "Others (N)" only when N is exact for this scope
 *   search     server-side, prefix by default (substring on request), across pages never loaded
 *
 * Visibility of a schema (NSV_*) is internal: it never creates a folder. Others means "not
 * highlighted", never "empty" (DBX-078 / 092).
 *
 * The model does not run SQL. It produces requests (one catalog read each) that the caller
 * executes one at a time per connection (nsx_next_request / nsx_complete), and it never builds
 * an N+1 per schema: a probe is one schema, on demand. Everything here is UI-thread data; tests
 * drive it headless with a simulated catalog (tests/nsx_check.c).
 *
 * Adapters (one per driver) only write SQL text; every statement is a plain read of the catalog
 * (checked by the caller with the write classifier before it runs).
 */
#include <stdbool.h>
#include <stdint.h>
#include "rs.h"

enum { NSV_UNKNOWN, NSV_LOADING, NSV_HAS, NSV_NONE, NSV_ERROR };
enum { NSM_COMPACT, NSM_ALL, NSM_PINNED };
/* why a schema is featured (bits) */
enum { NSF_CURRENT = 1, NSF_PINNED = 2, NSF_RECENT = 4, NSF_AUTO = 8, NSF_SMALL = 16 };
/* request kinds */
enum { NSR_SCOPE, NSR_NAMES, NSR_COUNT, NSR_PAGE, NSR_SEARCH, NSR_PROBE, NSR_TABLES, NSR_OBJSEARCH, NSR_DISCOVER, NSR_NKINDS };
/* object kinds that make a schema relevant (DBX-089) */
enum { NSK_TABLES = 1, NSK_VIEWS = 2, NSK_ROUTINES = 4, NSK_OTHER = 8, NSK_ALL = 15 };

/* DBX-103: what a driver's catalog can do. */
typedef struct NsCaps {
    bool schemas;             /* the tree has a Schemas / Databases node (false: SQLite keeps its tree) */
    bool server_search;       /* names can be filtered by the server */
    bool paged_names;         /* keyset pages ordered by name */
    bool visibility_probe;    /* "is any object visible in this schema" for one schema */
    bool cheap_count;         /* an exact count of names is cheap (small catalog table) */
    bool object_search;       /* objects by name across schemas */
    bool discovery;           /* bounded "owners with visible objects" (opt-in) */
    bool system_flag;         /* the catalog says which namespaces belong to the system */
    bool conn_tables_all;     /* the connection's table list (list_tables) covers every schema */
} NsCaps;

typedef struct NsPageReq {
    const char *after;        /* keyset cursor (exclusive), "" = from the start */
    const char *pattern;      /* "" none; prefix unless substring */
    bool substring;
    int limit;                /* rows to return (callers ask page + 1 to know has_more) */
    bool include_system;
    bool no_sysflag;          /* Oracle fallback when ORACLE_MAINTAINED does not exist */
} NsPageReq;

typedef struct NsAdapter {
    const char *driver;       /* DriverInfo id */
    const char *label;        /* "Schemas" / "Databases" */
    NsCaps caps;
    void (*scope_sql)(char *o, int n);                         /* 1 row: current namespace, user, container */
    void (*page_sql)(const NsPageReq *r, char *o, int n);      /* name, system (0 / 1), ordered by name */
    void (*names_in_sql)(const char *const *names, int k, bool no_sysflag, char *o, int n);   /* name, system: those that exist */
    void (*count_sql)(bool include_system, char *o, int n);    /* 1 row: count */
    void (*probe_sql)(const char *ns, int kinds, char *o, int n);   /* a row when an object is visible */
    void (*tables_sql)(const char *ns, int limit, char *o, int n);  /* name as the driver addresses it, is_view */
    void (*objsearch_sql)(const char *ns, const char *pattern, bool substring, int limit, char *o, int n);   /* schema, type, name */
    void (*discover_sql)(int limit, char *o, int n);           /* owner (opt-in, bounded) */
} NsAdapter;

const NsAdapter *nsx_adapter(const char *driver);   /* NULL when the driver has none */

/* A schema the model knows something about (only names the user touched: featured, loaded,
 * found, probed). */
typedef struct NsTable { char *name; bool is_view; } NsTable;
typedef struct NsInfo {
    char name[128];
    int vis;                  /* NSV_* */
    char note[160];           /* why UNKNOWN / ERROR ("not verified", "ORA-01031 ...") */
    uint64_t checked_ms;      /* when vis was decided (0 never) */
    bool system, exists_known, exists;
    /* tables of a schema not covered by the connection's list (DBX-090) */
    NsTable *tables; int ntables; bool tables_loaded, tables_loading, tables_more; char tables_err[200];
    bool tables_cancel;       /* the user asked to stop the running listing */
    int tables_secs;          /* seconds the running listing has taken so far (shown while loading) */
    uint32_t tables_serial;   /* unique per load: the Explorer's table cache follows it */
} NsInfo;

typedef struct NsItem { char name[128]; int flags; } NsItem;   /* a featured schema */

typedef struct NsPager {
    char **names; uint8_t *sys; int n, cap;
    char cursor[128];
    bool started, has_more, loading, failed;
    char error[200];
    uint32_t gen;
    int pages, want;          /* pages loaded; Refresh Others reloads as many as were listed */
} NsPager;

typedef struct NsHit { char schema[128], type[32], name[128]; bool system; } NsHit;
typedef struct NsSearch {
    char text[128]; bool substring, objects; char ns[128];   /* objects: Find Objects (ns "" = every schema) */
    uint32_t gen;
    uint64_t due;             /* debounce: the request starts at this time */
    bool pending, loading, done, partial, failed;
    char error[200];
    NsHit *hits; int nhits;
} NsSearch;

typedef struct NsReq {
    int kind; uint32_t gen;
    char key[128];            /* schema of a probe / tables request */
    char *sql;                /* owned */
    char *aux;                /* owned: NSR_NAMES, the names asked (newline separated) */
    int limit;                /* rows asked */
    int timeout_ms;
    uint64_t started;
} NsReq;

typedef struct NsPrefs {
    int mode, auto_cap, page_size, timeout_ms, kinds;
    bool show_system, auto_recent, auto_known, discover;
    char **pins; int npins;
    char **suppressed; int nsup;   /* "Move to Others": never promoted automatically */
    char **recent; int nrecent;    /* newest first */
} NsPrefs;

typedef struct NsMetrics {
    int queries, cancels, timeouts, failures, pages, names_loaded;
    double useful_ms;         /* connect -> featured list ready */
    double last_page_ms, last_search_ms;
    int skipped_stale;        /* responses dropped because a newer request replaced them */
} NsMetrics;

#define NSX_QUEUE 16
typedef struct NsState {
    const NsAdapter *ad;
    char conn_id[32];
    NsPrefs prefs;
    /* scope (DBX-095 / 110): who / where; a change drops every classification */
    bool scope_known, scope_loading, scope_failed;
    char current[128], user[128], container[128], scope_error[200];
    uint64_t scope_hash;
    double connected_at;      /* ms, for useful_ms */
    NsInfo *info; int ninfo, capinfo;
    NsItem *featured; int nfeatured, capfeat;
    NsPager pager;            /* Others (compact / pinned) or the inline list (all) */
    char **anchors; int nanchors;   /* names revealed / unpinned beyond the loaded pages (DBX-106) */
    bool total_known; long long total;   /* exact number of names in this scope and filter */
    bool count_loading, count_failed;
    NsSearch search;
    NsReq queue[NSX_QUEUE]; int nq;
    NsReq running; bool has_running;
    bool sysflag_unavailable;
    char **known; int nknown;  /* schemas with tables in the connection's own list */
    bool discover_done, discover_loading; char discover_note[160];
    uint32_t count_gen;       /* bumps when the count's scope / filter changed */
    NsMetrics m;
    uint32_t rev;             /* bumps when what the tree shows changed */
} NsState;

/* ---- registry (per connection id) ---- */
void nsx_set_dir(const char *prefs_dir);           /* where schemas.txt lives */
NsState *nsx_get(const char *conn_id, const char *driver);   /* NULL: driver without an adapter */
NsState *nsx_find(const char *conn_id);
void nsx_disconnected(const char *conn_id);        /* drops session data, keeps prefs */
void nsx_shutdown(void);

/* ---- lifecycle ---- */
/* A session opened: reads the scope (current schema, user, container) and checks the pinned /
 * recent names, nothing else (DBX-083). session_schema: Lume's own default schema, "" none. */
void nsx_connected(NsState *s, const char *session_schema, double now_ms);
/* Schemas that own tables in the connection's table list (no extra query). */
void nsx_note_known(NsState *s, const char *const *names, int n);
void nsx_refresh(NsState *s);                       /* Schemas > Refresh: scope, pins, count; pages restart */
void nsx_refresh_others(NsState *s);                /* Others > Refresh: the loaded pages only */
void nsx_refresh_schema(NsState *s, const char *name);   /* its tables and visibility */

/* ---- what the tree shows ---- */
int nsx_featured(NsState *s);                       /* recomputes s->featured; returns the count */
bool nsx_is_featured(NsState *s, const char *name);
int nsx_featured_flags(NsState *s, const char *name);
bool nsx_others_shown(NsState *s);
/* "Others" or "Others (12,200)" */
void nsx_others_label(NsState *s, char *out, int n);
long long nsx_others_exact(NsState *s);             /* -1 unknown */
/* names Others lists now: anchors then loaded pages, minus featured; returns count, fills up to max */
int nsx_others_names(NsState *s, const char **out, int max);
NsInfo *nsx_info(NsState *s, const char *name, bool create);
const char *nsx_vis_text(int vis);                  /* "has visible objects", "not verified", ... */

/* ---- actions ---- */
void nsx_expand_others(NsState *s);                 /* first page (only names) */
void nsx_load_more(NsState *s);
void nsx_pin(NsState *s, const char *name);
void nsx_unpin(NsState *s, const char *name);       /* Move to Others; refused for the current schema */
bool nsx_can_unpin(NsState *s, const char *name);
void nsx_note_recent(NsState *s, const char *name);
void nsx_reveal(NsState *s, const char *name);      /* anchors a searched name in Others */
void nsx_set_mode(NsState *s, int mode);
void nsx_set_system(NsState *s, bool on);
void nsx_set_kinds(NsState *s, int kinds);
void nsx_set_option(NsState *s, const char *key, int value);   /* auto_cap, page_size, timeout_ms, auto_recent, auto_known, discover */
void nsx_reset_prefs(NsState *s);                   /* Reset for this connection */
void nsx_search(NsState *s, const char *text, bool substring, bool objects, const char *ns, uint64_t now_ms);
void nsx_search_clear(NsState *s);
void nsx_probe(NsState *s, const char *name);       /* Verify Visible Objects */
void nsx_load_tables(NsState *s, const char *name);
/* Stops listing a schema's tables: drops it from the queue, or asks the running read to stop
 * (ns_pump cancels it). The schema then shows "Listing cancelled" with Retry. */
void nsx_cancel_tables(NsState *s, const char *name);
/* A schema's table listing is opened by the user and waits for large catalogs (all_tables on a
 * schema with many grants can take minutes); it still has a ceiling so a stuck read ends. */
#define NSX_TABLES_BUDGET_MS (10 * 60 * 1000)   /* default; Settings > Schema Explorer changes it */
/* Settings > Schema Explorer: default catalog read timeout and the table-listing budget, in ms
 * (values below 500 / 1000 are ignored). A connection's own timeout_ms (> 0) wins for reads. */
void nsx_set_timeouts(int read_ms, int tables_ms);
void nsx_discover(NsState *s);                      /* opt-in bounded discovery */
/* loaded names, featured first, newline separated (Copy Loaded Names); returns the count */
int nsx_copy_loaded(NsState *s, char *out, int n);

/* ---- execution ---- */
/* The next request to run (moves it to running), or NULL. Search debounce uses now_ms. */
const NsReq *nsx_next_request(NsState *s, uint64_t now_ms);
/* The running request is obsolete (a newer search / scope replaced it): cancel it. */
bool nsx_running_stale(NsState *s);
/* Result of the running request: rs NULL with err on failure; timed_out when the caller
 * cancelled it at its budget. */
void nsx_complete(NsState *s, const ResultSet *rs, const char *err, bool timed_out, bool cancelled, double ms, uint64_t now_ms);
bool nsx_busy(NsState *s);                          /* a request is queued or running */
uint64_t nsx_next_due(NsState *s);                  /* debounce deadline, 0 none */

/* ---- persistence / diagnostics ---- */
void nsx_save(void);
void nsx_load(void);
/* one line for the harness / Properties (no names unless with_names) */
int nsx_dump(NsState *s, char *out, int n, bool with_names);
/* error text with a specific hint (ORA-00942 / 01031, timeout, connection lost); no secrets */
void nsx_error_hint(const char *err, char *out, int n);
