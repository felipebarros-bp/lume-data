/* SPDX-License-Identifier: AGPL-3.0-only */
/* Command registry, context snapshot and dispatcher: see commands.h. UI thread only. */
#include "commands.h"
#include "planview.h"
/* the core table leaves the extension fields (invoke, ud, module) zero on purpose */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <strings.h>
#include "conn.h"
#include "editor.h"
#include "exec.h"
#include "log.h"
#include "menu.h"
#include "output.h"
#include "panels.h"

/* ---- rules ---------------------------------------------------------------- */

#define WHY(...) do { if (why && n) snprintf(why, n, __VA_ARGS__); return false; } while (0)

/* A query tab bound to a connection that is open. */
static bool target_ok(const LumeCtx *c, char *why, size_t n) {
    if (!c->sql_editor_active) WHY("No SQL query tab is active");
    if (!c->has_conn) WHY("This tab has no connection: choose one in the toolbar's datasource menu");
    if (c->conn_state == CONN_CONNECTING || c->conn_state == CONN_RECONNECTING) WHY("%s is connecting\xE2\x80\xA6", c->conn_name);
    if (!c->connected) WHY("%s is not connected (datasource menu > Connect)", c->conn_name);
    return true;
}
/* ... and nothing is running on its session (one execution per session). */
static bool idle_target(const LumeCtx *c, char *why, size_t n) {
    if (!target_ok(c, why, n)) return false;
    if (c->running) WHY("A statement is running on %s: Stop it first", c->conn_name);
    return true;
}
static bool r_always(const LumeCtx *c, char *why, size_t n) { (void)c; (void)why; (void)n; return true; }
static bool r_run(const LumeCtx *c, char *why, size_t n) {
    if (!idle_target(c, why, n)) return false;
    if (!c->has_text) WHY("The query tab is empty");
    return true;
}
static bool r_run_selection(const LumeCtx *c, char *why, size_t n) {
    if (!idle_target(c, why, n)) return false;
    if (!c->has_selection) WHY("Select the SQL to run first: Run Selection never runs text outside the selection");
    return true;
}
static bool r_explain(const LumeCtx *c, char *why, size_t n) {
    if (!r_run(c, why, n)) return false;
    if (!planview_supported(c->driver, false)) WHY("%s has no execution plan in Lume", c->caps ? c->caps->name : "This driver");
    return true;
}
static bool r_explain_analyze(const LumeCtx *c, char *why, size_t n) {
    if (!r_run(c, why, n)) return false;
    if (!planview_supported(c->driver, true)) WHY("%s has no actual execution plan (it reports no per-operator runtime)", c->caps ? c->caps->name : "This driver");
    return true;
}
static bool r_params(const LumeCtx *c, char *why, size_t n) {
    if (!r_run(c, why, n)) return false;
    if (!c->caps || !c->caps->parameters) WHY("Parameters are not supported for this driver");
    return true;
}
static bool r_rerun(const LumeCtx *c, char *why, size_t n) {
    if (!idle_target(c, why, n)) return false;
    if (!c->has_last_run) WHY("Nothing has run in this session yet");
    return true;
}
static bool r_cancel(const LumeCtx *c, char *why, size_t n) {
    if (c->running && c->cancelling) WHY("Cancel already requested: waiting for the driver");
    if (!c->running) { if (c->running_total) WHY("Nothing runs on this tab's connection (%d elsewhere: Stop \xE2\x96\xBE lists them)", c->running_total); WHY("Nothing is running"); }
    return true;
}
static bool r_connect(const LumeCtx *c, char *why, size_t n) {
    if (!c->has_conn) WHY("This tab has no connection: choose one first");
    if (c->connected) WHY("%s is already connected", c->conn_name);
    if (c->conn_state == CONN_CONNECTING || c->conn_state == CONN_RECONNECTING) WHY("%s is connecting\xE2\x80\xA6", c->conn_name);
    return true;
}
static bool r_disconnect(const LumeCtx *c, char *why, size_t n) {
    if (!c->has_conn || !c->connected) WHY("Not connected");
    if (c->running) WHY("A statement is running on %s: Stop it first", c->conn_name);
    return true;
}
static bool r_cancel_connect(const LumeCtx *c, char *why, size_t n) {
    if (c->conn_state != CONN_CONNECTING && c->conn_state != CONN_RECONNECTING) WHY("No connect in progress");
    return true;
}
static bool r_reconnect(const LumeCtx *c, char *why, size_t n) {
    if (!c->has_conn) WHY("This tab has no connection");
    if (c->conn_state == CONN_CONNECTING || c->conn_state == CONN_RECONNECTING) WHY("%s is connecting\xE2\x80\xA6", c->conn_name);
    if (c->running) WHY("A statement is running on %s: Stop it before reconnecting", c->conn_name);
    return true;
}
static bool r_tx_mode(const LumeCtx *c, char *why, size_t n) {
    if (!idle_target(c, why, n)) return false;
    if (!c->caps || !c->caps->transactions) WHY("This driver has no manual transactions in Lume");
    return true;
}
static bool r_tx_end(const LumeCtx *c, char *why, size_t n) {
    if (!idle_target(c, why, n)) return false;
    if (!c->caps || !c->caps->transactions) WHY("This driver has no manual transactions in Lume");
    if (c->tx_state < 0) WHY("The transaction state of %s is not known with confidence", c->conn_name);
    if (c->tx_state == 0) WHY("No open transaction on %s", c->conn_name);
    return true;
}
static bool r_session(const LumeCtx *c, char *why, size_t n) {
    if (!c->has_conn) WHY("This tab has no connection");
    return true;
}

/* ---- registry ------------------------------------------------------------- */

#define MAX_COMMANDS 128
static Command cmds[MAX_COMMANDS];
static int ncmds;
static unsigned version = 1;
unsigned commands_version(void) { return version; }
static void (*dispatch_fn)(App *a, int act);

static const Command core[] = {
    /* workspace and files */
    { "workspace.new", "New Workspace\xE2\x80\xA6", "workspace", "Workspace", "project create", ACT_WORKSPACE_NEW, 0, r_always },
    { "workspace.open", "Open Workspace\xE2\x80\xA6", "workspace", "Workspace", "project switch recent", ACT_WORKSPACE_OPEN, 0, r_always },
    { "workspace.settings", "Workspace Settings\xE2\x80\xA6", "workspace", "Workspace", "project preferences", ACT_WORKSPACE_SETTINGS, 0, r_always },
    { "workspace.close", "Close Workspace", "workspace", "Workspace", "project", ACT_WORKSPACE_CLOSE, 0, r_always },
    { "sql.new", "New SQL Query", "plus", "File", "new tab query console", ACT_NEW_EDITOR_TAB, 0, r_always },
    { "nav.quick_open", "Quick Open\xE2\x80\xA6", "search", "Navigation", "go to table object file find", ACT_QUICK_OPEN, 0, r_always },
    { "nav.switch_tab", "Switch Tab\xE2\x80\xA6", "tabs", "Navigation", "recent tabs editors", ACT_SWITCH_TAB, 0, r_always },
    { "nav.palette", "Command Palette\xE2\x80\xA6", "search", "Navigation", "search actions find command", ACT_PALETTE, 0, r_always },
    { "app.about.open", "About Lume Data", "info", "Help", "version build license credits", ACT_ABOUT, 0, r_always },
    /* datasource and session */
    { "ds.connect", "Connect", "plug", "Datasource", "open login", ACT_CONNECT, 0, r_connect },
    { "ds.disconnect", "Disconnect", "unplug", "Datasource", "close", ACT_DISCONNECT, CMD_DESTRUCTIVE, r_disconnect },
    { "ds.reconnect", "Reconnect", "refresh", "Datasource", "retry restore session", ACT_RECONNECT, CMD_DESTRUCTIVE, r_reconnect },
    { "ds.cancel_connect", "Cancel Connect", "stop", "Datasource", "abort", ACT_CANCEL_CONNECT, 0, r_cancel_connect },
    { "ds.settings", "Connection Settings\xE2\x80\xA6", "gear", "Datasource", "edit properties", ACT_EDIT_CONN, 0, r_always },
    { "ds.new", "New Connection\xE2\x80\xA6", "plus", "Datasource", "add database", ACT_NEW_CONN, 0, r_always },
    { "ds.session", "Session Details\xE2\x80\xA6", "info", "Datasource", "transaction session id", ACT_SESSION_DETAILS, 0, r_session },
    /* execution */
    { "sql.run", "Run", "run", "Execution", "execute go statement caret", ACT_RUN, CMD_RUNS_SQL, r_run },
    { "sql.run_selection", "Run Selection", "run", "Execution", "execute selected", ACT_RUN_SELECTION, CMD_RUNS_SQL, r_run_selection },
    { "sql.run_file", "Run All in File", "run", "Execution", "execute script everything", ACT_RUN_ALL, CMD_RUNS_SQL, r_run },
    { "sql.run_new_tab", "Run in New Tab", "run", "Execution", "execute", ACT_RUN_NEW_TAB, CMD_RUNS_SQL, r_run },
    { "sql.explain", "Explain Plan", "plan", "Execution", "query plan performance", ACT_EXPLAIN, CMD_RUNS_SQL, r_explain },
    { "sql.explain_analyze", "Explain Analyze\xE2\x80\xA6", "plan", "Execution", "query plan timing performance", ACT_EXPLAIN_ANALYZE, CMD_RUNS_SQL | CMD_DESTRUCTIVE, r_explain_analyze },
    { "sql.run_params", "Run with Parameters\xE2\x80\xA6", "run", "Execution", "bind variables placeholders", ACT_RUN_PARAMS, CMD_RUNS_SQL, r_params },
    { "sql.run_to_file", "Execute to File\xE2\x80\xA6", "save", "Execution", "export csv output", ACT_RUN_TO_FILE, CMD_RUNS_SQL, r_run },
    { "sql.rerun_last", "Re-run Last\xE2\x80\xA6", "refresh", "Execution", "repeat again previous", ACT_RERUN_LAST, CMD_RUNS_SQL, r_rerun },
    { "sql.exec_settings", "Query Execution Settings\xE2\x80\xA6", "gear", "Execution", "fetch size continue on error", ACT_EXEC_SETTINGS, 0, r_always },
    { "sql.cancel", "Stop", "stop", "Execution", "cancel abort interrupt kill", ACT_CANCEL, CMD_SAFETY, r_cancel },
    /* transactions */
    { "tx.auto", "Auto-commit", "tx", "Transaction", "autocommit mode", ACT_TX_AUTO, CMD_RUNS_SQL, r_tx_mode },
    { "tx.manual", "Manual Transaction", "tx", "Transaction", "begin mode", ACT_TX_MANUAL, CMD_RUNS_SQL, r_tx_mode },
    { "tx.commit", "Commit", "commit", "Transaction", "save persist", ACT_COMMIT, CMD_RUNS_SQL | CMD_DESTRUCTIVE, r_tx_end },
    { "tx.rollback", "Rollback", "rollback", "Transaction", "undo discard revert", ACT_ROLLBACK, CMD_RUNS_SQL | CMD_DESTRUCTIVE, r_tx_end },
    /* panels */
    { "panel.explorer", "Explorer", "db", "Panels", "database tree connections", ACT_PANEL_EXPLORER, 0, r_always },
    { "panel.results", "Results", "grid", "Panels", "grid data rows", ACT_PANEL_RESULTS, 0, r_always },
    { "panel.output", "Output", "log", "Panels", "messages errors log", ACT_PANEL_OUTPUT, 0, r_always },
    { "panel.history", "Query History", "history", "Panels", "recent executions sql log", ACT_PANEL_HISTORY, 0, r_always },
    { "panel.configure", "Configure Panels\xE2\x80\xA6", "panels", "Panels", "list docking", ACT_PANELS, 0, r_always },
    { "layout.reset", "Restore Default Panel Layout", "panels", "Panels", "reset docking", ACT_LAYOUT_DEFAULT, 0, r_always },
    /* the toolbar itself */
    { "toolbar.toggle", "Show Main Toolbar", "toolbar", "Toolbar", "hide appearance", ACT_TOOLBAR_TOGGLE, 0, r_always },
    { "toolbar.compact", "Compact Toolbar", "toolbar", "Toolbar", "small dense", ACT_TOOLBAR_COMPACT, 0, r_always },
    { "toolbar.labels", "Show Toolbar Labels", "toolbar", "Toolbar", "text names", ACT_TOOLBAR_LABELS, 0, r_always },
    { "toolbar.customize", "Customize Toolbar\xE2\x80\xA6", "toolbar", "Toolbar", "add action reorder hide preset", ACT_TOOLBAR_CUSTOMIZE, 0, r_always },
    { "toolbar.reset", "Reset Toolbar to Default", "toolbar", "Toolbar", "restore defaults", ACT_TOOLBAR_RESET, 0, r_always },
    { "toolbar.focus", "Focus Main Toolbar", "toolbar", "Toolbar", "keyboard navigate", ACT_TOOLBAR_FOCUS, 0, r_always },
    { "toolbar.export", "Export Toolbar Settings\xE2\x80\xA6", "toolbar", "Toolbar", "support diagnostics", ACT_TOOLBAR_EXPORT, 0, r_always },
};

/* Loads the core commands once, whoever comes first (a module may register before the toolbar). */
static bool core_loaded;
void commands_init(void) {
    if (core_loaded) return;
    core_loaded = true;
    /* core commands first, extension commands registered earlier keep their place after them */
    Command ext[MAX_COMMANDS]; int next = ncmds; memcpy(ext, cmds, sizeof cmds[0] * (size_t)next);
    ncmds = 0;
    for (size_t i = 0; i < sizeof core / sizeof core[0] && ncmds < MAX_COMMANDS; i++) cmds[ncmds++] = core[i];
    for (int i = 0; i < next && ncmds < MAX_COMMANDS; i++) cmds[ncmds++] = ext[i];
    version++;
}
int commands_count(void) { commands_init(); return ncmds; }
const Command *commands_at(int i) { commands_init(); return i >= 0 && i < ncmds ? &cmds[i] : NULL; }
const Command *commands_find(const char *id) { commands_init(); for (int i = 0; id && i < ncmds; i++) if (!strcmp(cmds[i].id, id)) return &cmds[i]; return NULL; }
const Command *commands_by_act(int act) { commands_init(); for (int i = 0; act && i < ncmds; i++) if (cmds[i].act == act) return &cmds[i]; return NULL; }
void commands_set_dispatch(void (*fn)(App *a, int act)) { dispatch_fn = fn; }
void commands_dispatch(App *a, int act) { if (dispatch_fn) dispatch_fn(a, act); }

/* ---- context snapshot ------------------------------------------------------ */

static LumeCtx cur;
static uint32_t stmts_rev = 0xFFFFFFFF; static int stmts_cached, stmts_dialect; static bool stmts_has_text;   /* cached per editor revision */
static const Editor *stmts_ed;
static char ran_conns[16][16]; static int nran;                  /* connections where something ran (Re-run Last) */
static void note_ran(const char *id) {
    if (!id || !id[0]) return;
    for (int i = 0; i < nran; i++) if (!strcmp(ran_conns[i], id)) return;
    if (nran == 16) { memmove(ran_conns[0], ran_conns[1], 16 * 15); nran--; }
    snprintf(ran_conns[nran++], 16, "%s", id);
}
static bool has_ran(const char *id) { for (int i = 0; i < nran; i++) if (!strcmp(ran_conns[i], id)) return true; return false; }

const LumeCtx *commands_ctx(void) { return &cur; }

void commands_snapshot(App *a, LumeCtx *o) {
    unsigned long long gen = cur.generation;
    memset(o, 0, sizeof *o);
    o->tx_state = -1;
    Connection *c = NULL;
    if (a->panels && a->editor && !panels_active_report(a->panels, a) && !panels_active_designer(a->panels, a)) {   /* a report / designer tab is not a query tab */
        c = panels_tab_connection(a->panels, a, o->tab_title, sizeof o->tab_title);
        o->sql_editor_active = true;
        o->editor_id = (int)(uintptr_t)a->editor % 1000000 + 1;
        /* text facts are recomputed only when the editor changed (no per-frame copy in idle) */
        uint32_t rev = editor_revision(a->editor);
        int dialect = c && c->db ? sql_dialect_for(c->db->drv) : SQL_DIALECT_GENERIC;
        if (rev != stmts_rev || a->editor != stmts_ed || dialect != stmts_dialect) {
            int len = 0; char *txt = editor_text(a->editor, &len);
            stmts_has_text = false;
            for (int i = 0; txt && i < len; i++) if (txt[i] != ' ' && txt[i] != '\n' && txt[i] != '\t' && txt[i] != '\r') { stmts_has_text = true; break; }
            SqlStmt *parts = malloc(sizeof *parts * 1024);
            stmts_cached = txt && parts ? sql_split(txt, len, dialect, parts, 1024) : 0;
            free(parts); free(txt);
            stmts_rev = rev; stmts_ed = a->editor; stmts_dialect = dialect;
        }
        o->has_text = stmts_has_text;
        o->statements = stmts_cached;
        int sl = 0; char *sel = editor_selection(a->editor, &sl);
        o->has_selection = sel && sl > 0; free(sel);
    }
    if (c) {
        o->has_conn = true;
        snprintf(o->conn_id, sizeof o->conn_id, "%s", c->id);
        snprintf(o->conn_name, sizeof o->conn_name, "%s", c->name);
        snprintf(o->driver, sizeof o->driver, "%s", c->driver);
        o->conn_state = c->state; o->conn_generation = c->generation;
        o->connected = c->db != NULL;
        if (o->connected && o->conn_state != CONN_CONNECTED) o->conn_state = CONN_CONNECTED;
        o->env = c->env; o->env_color = conn_env_color(c);
        o->read_only = c->read_only; o->ro_level = c->ro_level;
        snprintf(o->schema, sizeof o->schema, "%s", c->schema);
        snprintf(o->error, sizeof o->error, "%s", c->error);
        o->manual_tx = c->manual_tx;
        o->caps = caps_for(c->driver);
        if (c->db) {
            o->running = output_db_running(a->output, c->db);
            for (int k = 0; o->running; k++) {
                OutRunInfo ri; if (!output_running_info(a->output, k, &ri)) break;
                if (ri.db == c->db) { o->exec_id = ri.exec_id; o->elapsed_ms = ri.elapsed_ms; o->cancelling = ri.cancelling; o->running_here = ri.editor == (const void *)a->editor; break; }
            }
            /* the driver's own report, read only while the connection is idle (never inferred) */
            if (!o->running && c->db->drv->tx_state && !__atomic_load_n(&c->db->busy, __ATOMIC_SEQ_CST)) o->tx_state = c->db->drv->tx_state(c->db);
        }
        o->has_last_run = has_ran(c->id);
    }
    o->running_total = output_running_count(a->output);
    /* generation: compare everything but itself and the clock */
    LumeCtx a1 = *o, b1 = cur; a1.generation = b1.generation = 0; a1.elapsed_ms = b1.elapsed_ms = 0;
    o->generation = memcmp(&a1, &b1, sizeof a1) ? gen + 1 : gen;
}

void commands_frame(App *a) { LumeCtx n; commands_snapshot(a, &n); cur = n; }

/* ---- rules, invoke, gate ---------------------------------------------------- */

bool commands_enabled(const char *id, char *why, size_t n) {
    const Command *c = commands_find(id);
    if (why && n) why[0] = 0;
    if (!c) { if (why && n) snprintf(why, n, "Unknown command"); return false; }
    return c->enabled ? c->enabled(&cur, why, n) : true;
}

static Uint64 last_invoke_ms[MAX_COMMANDS]; static char last_invoke_key[MAX_COMMANDS][32];

bool commands_invoke(App *a, const char *id) {
    const Command *c = commands_find(id);
    if (!c) { app_status(a, "Unknown command %s", id); return false; }
    commands_frame(a);   /* resolve the context at the instant of the action (TBR-025) */
    char why[200] = "";
    if (c->enabled && !c->enabled(&cur, why, sizeof why)) { app_status(a, "%s: %s", c->label, why); LOG_I("command %s refused: %s", id, why); return false; }
    /* a repeated click / key for the same execution target within 400 ms is the same gesture */
    int k = (int)(c - cmds);
    if (c->policy & CMD_RUNS_SQL) {
        char key[32]; snprintf(key, sizeof key, "%d/%s", cur.editor_id, cur.conn_id);
        Uint64 now = SDL_GetTicks();
        if (now - last_invoke_ms[k] < 400 && !strcmp(key, last_invoke_key[k])) { LOG_I("command %s ignored: repeated within 400 ms", id); return false; }
        last_invoke_ms[k] = now; snprintf(last_invoke_key[k], sizeof last_invoke_key[k], "%s", key);
        note_ran(cur.conn_id);
    }
    LOG_I("command %s (tab \"%s\", connection %s)", id, cur.tab_title, cur.has_conn ? cur.conn_name : "none");
    if (c->act && dispatch_fn) dispatch_fn(a, c->act);
    else if (c->invoke) { LumeCtx snap = cur; c->invoke(c->ud, &snap); }   /* extensions: a copy, no App */
    return true;
}

bool commands_act_enabled(int act) {
    const Command *c = commands_by_act(act);
    return !c || !c->enabled || c->enabled(&cur, NULL, 0);
}

bool commands_gate(App *a, int act) {
    const Command *c = commands_by_act(act);
    if (!c || !c->enabled) return true;
    char why[200] = "";
    LumeCtx now; commands_snapshot(a, &now);
    if (c->enabled(&now, why, sizeof why)) {
        if (c->policy & CMD_RUNS_SQL) note_ran(now.conn_id);
        return true;
    }
    app_status(a, "%s: %s", c->label, why);
    LOG_I("command %s refused: %s", c->id, why);
    return false;
}

/* ---- shortcuts ---------------------------------------------------------------- */

const char *commands_shortcut(const char *id) {
    static char buf[48];
    const Command *c = commands_find(id);
    buf[0] = 0;
    if (!c || !c->act) return buf;
    for (int i = 0; i < menu_item_count(); i++) {
        const char *menu, *label; char sc[48] = ""; int act = 0;
        if (menu_item_at(i, &menu, &label, sc, sizeof sc, &act) && act == c->act && sc[0]) { snprintf(buf, sizeof buf, "%s", sc); break; }
    }
    return buf;
}

int commands_collisions(const char **a, const char **b) {
    int pairs = 0;
    for (int i = 0; i < ncmds; i++) {
        char si[48]; snprintf(si, sizeof si, "%s", commands_shortcut(cmds[i].id));
        if (!si[0]) continue;
        for (int j = i + 1; j < ncmds; j++) {
            if (cmds[j].act == cmds[i].act) continue;
            if (!strcmp(si, commands_shortcut(cmds[j].id))) { if (!pairs && a && b) { *a = cmds[i].id; *b = cmds[j].id; } pairs++; }
        }
    }
    return pairs;
}

/* ---- extensions (TBR-050) ------------------------------------------------------ */

bool commands_register(const Command *c) {
    commands_init();
    if (!c || !c->id || !c->label || !c->module) return false;
    if (commands_find(c->id)) { LOG_W("command %s: duplicate id refused", c->id); return false; }
    if (c->act || (c->policy & CMD_RUNS_SQL)) { LOG_W("command %s from %s refused: extensions cannot run SQL or bind core actions", c->id, c->module); return false; }
    if (!c->invoke) return false;
    if (ncmds >= MAX_COMMANDS) return false;
    cmds[ncmds++] = *c;
    version++;
    LOG_I("command %s registered by module %s", c->id, c->module);
    return true;
}

int commands_unregister_module(const char *module) {
    int removed = 0;
    for (int i = 0; i < ncmds;) {
        if (cmds[i].module && !strcmp(cmds[i].module, module)) {
            memmove(&cmds[i], &cmds[i + 1], sizeof cmds[0] * (size_t)(ncmds - i - 1));
            memmove(&last_invoke_ms[i], &last_invoke_ms[i + 1], sizeof last_invoke_ms[0] * (size_t)(ncmds - i - 1));
            memmove(&last_invoke_key[i], &last_invoke_key[i + 1], sizeof last_invoke_key[0] * (size_t)(ncmds - i - 1));
            ncmds--; removed++;
        } else i++;
    }
    if (removed) { version++; LOG_I("module %s: %d command(s) removed", module, removed); }
    return removed;
}
