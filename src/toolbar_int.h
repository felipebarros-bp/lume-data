/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/* Internal state of the main toolbar, shared by toolbar.c (bar, menus, preferences) and
 * toolbar_actions.c (commands and dialogs). Not part of the public API (toolbar.h). */
#include "toolbar.h"
#include "commands.h"
#include "ui.h"

/* item kinds */
enum { IK_BUTTON, IK_DROPDOWN, IK_DATASOURCE, IK_SCHEMA, IK_TX, IK_RUN, IK_STOP, IK_FLEX, IK_MORE };
/* groups, left to right; a separator is drawn between neighbouring groups (TBR-002) */
enum { G_FILE, G_CONTEXT, G_EXEC, G_FLEX, G_NAV };
/* presets (TBR-041) */
enum { PRESET_MINIMAL, PRESET_DEFAULT, PRESET_ADVANCED, PRESET_CUSTOM, PRESET_COUNT };

#define TB_MAX_ITEMS 32
#define TB_MAX_CUSTOM 8
#define TB_RECENT 5

/* One item the bar can show: a built-in widget or an action the user added (a command id). */
typedef struct TbItem {
    char id[40];             /* "run", "datasource", or "cmd:sql.run_file" for an added action */
    int kind, group, prio;   /* IK_*, G_*, overflow priority (higher = kept longer) */
    const char *cmd;         /* command invoked by a click (buttons, Run's main part) */
    const char *icon, *label;
    bool required;           /* cannot be removed (datasource, Run, Stop, More) */
    unsigned presets;        /* bit per PRESET_* where it is visible by default */
    bool custom;             /* added by the user */
} TbItem;

/* Preferences, persisted in toolbar.json (schemaVersion 1). */
typedef struct TbPrefs {
    bool visible, compact, labels;
    int preset;                         /* PRESET_*; CUSTOM once the user changed a preset */
    char order[TB_MAX_ITEMS][40]; int norder;   /* item ids in display order */
    char hidden[TB_MAX_ITEMS][40]; int nhidden; /* items switched off */
    char custom[TB_MAX_CUSTOM][40]; int ncustom; /* added actions (command ids) */
    char recent[TB_RECENT][16]; int nrecent;     /* recently chosen datasources (connection ids) */
} TbPrefs;

/* One drawn item of this frame. */
typedef struct TbSlot { int item; Rect r; Rect chev; bool short_form; float text_w; } TbSlot;

/* What a popup entry does. */
typedef struct TbEntry { int kind; const char *cmd; int arg; char text[64]; } TbEntry;
enum { EN_CMD, EN_SCHEMA, EN_SCHEMA_DEFAULT, EN_CANCEL_EXEC, EN_PANEL, EN_OVERFLOW, EN_PRESET, EN_ISOLATION, EN_RECENT_WS, EN_TB_TOGGLE };

/* modal dialogs (toolbar_actions.c) */
enum { TM_NONE, TM_CONFIRM, TM_PARAMS, TM_SETTINGS, TM_CUSTOMIZE, TM_SESSION, TM_LIST, TM_NAME };
/* what a confirmation continues with when accepted */
enum { CF_RUN, CF_SWITCH_DS, CF_TX_MODE_AUTO, CF_COMMIT, CF_ROLLBACK, CF_DISCONNECT, CF_RECONNECT, CF_CLOSE_TAB, CF_RESET_TOOLBAR, CF_WS_SWITCH, CF_CONN_FAILED, CF_CRASH, CF_UPDATE, CF_SSH_TRUST };
/* list dialogs */
enum { LIST_SWITCH_TAB, LIST_QUICK_OPEN, LIST_WORKSPACES };

#define TB_MAX_PARAMS 12
typedef struct TbParam { char name[40]; TextField f; } TbParam;

/* A run waiting for a confirmation or parameters. */
typedef struct TbRun {
    int src;                 /* RUNSRC_* */
    int mode;                /* RUN_* */
    char *sql; int len, base;   /* text to run (owned) */
    char *display;           /* shown instead (parameters), owned or NULL */
    char title[64], note[200];
    char *export_path;       /* Execute to File, owned or NULL */
    char conn_id[16];        /* the target captured at the click (TBR-025 / spec 8.2) */
    unsigned conn_generation;
    bool plan_actual; char *plan_stmt;   /* Explain Analyze: the statement for the plan panel (owned) */
} TbRun;

struct Toolbar {
    TbPrefs prefs, before_customize;
    char ws_name[48];        /* active workspace ("Lume Data" = the default one) */
    char ws_dir[1100];       /* its folder, with a trailing slash */
    bool dirty; Uint64 dirty_at;   /* prefs to save (coalesced) */
    /* items and this frame's layout */
    TbItem items[TB_MAX_ITEMS]; int nitems; unsigned items_version;   /* commands_version() they were built from */
    TbSlot slots[TB_MAX_ITEMS]; int nslots;
    int overflow[TB_MAX_ITEMS]; int noverflow;
    Rect bar;
    /* interaction */
    int hover; Uint64 hover_since; Rect hover_rect;
    int focus;               /* slot focused from the keyboard, -1 none (TBR-047) */
    bool kb_mode; int focus_return;   /* keyboard focus is on the bar; a->focus to restore */
    char tip[400];           /* tooltip of the hovered item */
    /* popup menus */
    PopupMenu menu, sub;
    TbEntry entries[64]; int nentries;
    char mtext[64][112]; int nmtext;
    int menu_item;           /* item whose dropdown is open, -1 */
    /* datasource popover (search + list + actions, TBR-009) */
    bool ds_open; Rect ds_rect; TextField ds_search; int ds_sel; int ds_rows[64]; int nds_rows; Rect ds_row_rect[64];
    /* dialogs */
    int modal; Rect modal_rect;
    int cf_kind; char cf_title[120], cf_body[2400], cf_btn[4][40]; int cf_nbtn; int cf_choice; char cf_arg[64];
    TbRun run;               /* run waiting for a confirmation / parameters */
    TbParam params[TB_MAX_PARAMS]; int nparams; int param_focus;
    int list_kind; TextField list_search; int list_sel; int list_scroll;
    char list_text[256][160]; char list_sub[256][120]; int list_arg[256]; int nlist;
    int cust_sel; int cust_preset_view;
    TextField name_field;
    char session_text[900];
    /* execution bookkeeping */
    struct { char conn_id[16]; char *sql; int len; char title[64]; char when[16]; } last[8]; int nlast;
    struct { int exec_id; char conn_id[16]; char schema[64]; bool reset; } pending_schema[4]; int npending_schema;
    struct { int exec_id; char conn_id[16]; int kind; char arg[64]; } follow[4]; int nfollow;   /* after Commit / Rollback */
    char state_line[600];
};

/* toolbar.c */
void tb_mark_dirty(Toolbar *t);
void tb_apply_preset(Toolbar *t, int preset);
void tb_rebuild_items(Toolbar *t);
bool tb_item_hidden(const Toolbar *t, const char *id);
void tb_set_hidden(Toolbar *t, const char *id, bool hidden);
void tb_save_prefs(Toolbar *t, App *a);
void tb_load_workspace(Toolbar *t, App *a, const char *name);
const char *tb_preset_name(int p);
Connection *tb_conn_by_id(App *a, const char *id);
void tb_note_recent(Toolbar *t, const char *conn_id);
void tb_open_datasource(Toolbar *t, App *a);   /* the datasource popover, under its button (or "...") */

/* toolbar_actions.c */
bool tba_action(Toolbar *t, App *a, int act);
void tba_modal_input(Toolbar *t, App *a);     /* in toolbar_frame */
void tba_modal_draw(Toolbar *t, App *a);      /* in toolbar_draw_overlay */
void tba_choose_datasource(Toolbar *t, App *a, Connection *c);
void tba_set_schema(Toolbar *t, App *a, const char *schema);   /* NULL = the server's default */
void tba_set_isolation(Toolbar *t, App *a, const char *level);
void tba_confirm(Toolbar *t, int kind, const char *title, const char *body, const char *b0, const char *b1, const char *b2, const char *arg);
void tba_confirm4(Toolbar *t, int kind, const char *title, const char *body, const char *b0, const char *b1, const char *b2, const char *b3, const char *arg);
void tba_run_free(TbRun *r);
void tba_execution_finished(Toolbar *t, App *a, Db *db, int exec_id, int status);
void tba_ws_request(Toolbar *t, App *a, const char *name);
