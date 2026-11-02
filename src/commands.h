/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Commands and context (toolbar spec, section 8: Commands + Context + UI)
 * =======================================================================
 *
 * One registry of commands for every entry point: the main toolbar, the menu bar, the command
 * palette and keyboard shortcuts all go through it. Each command has a stable id ("sql.run"),
 * a label, an icon name, a group, search synonyms, a policy, and a rule that says whether it can
 * run now and, when not, why. Core commands map to an ACT_* code that main.c / panels.c
 * execute (do_action). do_action calls commands_gate first, so a disabled command does nothing
 * from any entry point and explains why (TBR-048).
 *
 * The rules read a snapshot of the application context (LumeCtx), rebuilt every frame from the
 * active query tab: its own connection (never the Explorer's selection), that connection's
 * state and session, whether it is running, its transaction, the driver capabilities. The
 * snapshot holds ids and copied values, never pointers, so a stale snapshot cannot reach freed
 * memory. Its `generation` changes whenever any field changes (TBR-049).
 *
 * Invoking a command takes a fresh snapshot at that instant (TBR-025: if the focused editor
 * changed before the click, the new one is the target), and execution commands invoked twice
 * within a short interval for the same tab and session are dropped (no double run from a
 * double click or a repeated key).
 *
 * Extensions (TBR-050) register commands with their own invoke callback. They receive the
 * snapshot only (no Db handle, no credentials) and cannot declare the CMD_RUNS_SQL policy:
 * a toolbar button does not grant a module access to databases.
 */
#include "lume.h"
#include "caps.h"

typedef struct LumeCtx {
    unsigned long long generation;   /* changes when any field below changes */
    /* active query tab */
    int editor_id;                   /* 0 = no query tab */
    char tab_title[48];
    bool sql_editor_active, has_text, has_selection;
    int statements;                  /* statements in the tab (for "Run All in File (n)") */
    /* its connection */
    bool has_conn;                   /* the tab is bound to a saved connection */
    char conn_id[16], conn_name[64], driver[16];
    int conn_state;                  /* CONN_* */
    unsigned conn_generation;
    bool connected;
    int env; unsigned env_color;     /* CONN_ENV_* and its colour */
    bool read_only; int ro_level;    /* "read-only in Lume" and the level obtained (RO_*) */
    char schema[64];                 /* session default schema chosen in Lume, "" = server default (unknown) */
    char error[160];                 /* last connect failure */
    /* session and execution */
    bool running, cancelling;        /* on this tab's session */
    bool running_here;               /* ... and started from this very tab (the session may be shared by tabs) */
    int exec_id; double elapsed_ms;
    int running_total;               /* executions running in every session */
    int tx_state;                    /* -1 unknown, 0 none, 1 open: as the driver reports it */
    bool manual_tx;
    bool has_last_run;               /* something to re-run */
    const DriverCaps *caps;          /* NULL when the tab has no connection */
} LumeCtx;

/* Command policies. */
enum {
    CMD_RUNS_SQL = 1,        /* sends SQL to a database: core only, gated by the connection state */
    CMD_DESTRUCTIVE = 2,     /* can lose data (Commit / Rollback / Disconnect with a transaction) */
    CMD_SAFETY = 4,          /* a safety command the toolbar never lets the user remove (Stop) */
};

typedef struct Command {
    const char *id;          /* stable, unique: "sql.run" */
    const char *label;       /* "Run" */
    const char *icon;        /* icon name for the toolbar / palette ("run", "stop", ...) */
    const char *group;       /* "Execution", "Datasource", ... */
    const char *synonyms;    /* extra words for the palette search ("execute go") */
    int act;                 /* ACT_* executed by the core; 0 for extension commands */
    unsigned policy;         /* CMD_* */
    /* true when it can run now; otherwise why in `why` (short, no sensitive data) */
    bool (*enabled)(const LumeCtx *c, char *why, size_t n);
    /* extension commands only: called with the snapshot (no App, no Db) */
    void (*invoke)(void *ud, const LumeCtx *c);
    void *ud;
    const char *module;      /* NULL for core commands */
} Command;

void commands_init(void);
int commands_count(void);
const Command *commands_at(int i);
const Command *commands_find(const char *id);
const Command *commands_by_act(int act);

/* The snapshot of this frame (rebuilt by commands_frame). */
const LumeCtx *commands_ctx(void);
void commands_snapshot(App *a, LumeCtx *out);   /* a fresh one, now */
void commands_frame(App *a);                    /* once per frame, before any widget reads the context */

/* Whether `id` can run now; why = the reason when not (may be NULL). */
bool commands_enabled(const char *id, char *why, size_t n);
/* Runs a command from any entry point: fresh snapshot, rule, duplicate guard, then dispatch.
 * Returns false (with the reason in the status bar) when it did not run. */
bool commands_invoke(App *a, const char *id);
/* do_action's gate for ACT_* codes that belong to a command: false (reason shown) blocks it. */
bool commands_gate(App *a, int act);
bool commands_act_enabled(int act);             /* this frame's rule for an ACT_* (true when no command owns it) */
/* The core dispatcher (main.c's do_action), installed once. */
void commands_set_dispatch(void (*dispatch)(App *a, int act));
void commands_dispatch(App *a, int act);        /* runs an ACT_* through the core dispatcher (its gate included) */

/* Shortcut text of a command for this platform ("⌘↩" on macOS, "Ctrl+Enter" elsewhere), "" if none. */
const char *commands_shortcut(const char *id);
/* TBR-048: commands whose shortcuts collide (same key for two ids); returns how many pairs
 * were found and writes the first one into a / b. */
int commands_collisions(const char **a, const char **b);

/* Extensions (TBR-050). register: false for a duplicate id, a core ACT, or CMD_RUNS_SQL.
 * unregister_module removes every command of the module and returns how many. */
bool commands_register(const Command *c);
int commands_unregister_module(const char *module);
/* +1 whenever the registry changes: views holding command pointers rebuild (no dangling button). */
unsigned commands_version(void);
