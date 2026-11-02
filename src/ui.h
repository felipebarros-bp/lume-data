/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "lume.h"

/* Small immediate-mode widgets for dialogs (ui.c). Each call draws the widget for this frame and
 * handles this frame's input from App.in; rectangles are in render pixels. Persistent state
 * (TextField, PopupMenu) is owned by the caller. Main thread only. */

/*
 * Single-line text field with the editing model of a native macOS text box:
 *
 *   caret       Left / Right (character), Option+Left / Right (word), Cmd+Left / Right,
 *               Home / End, Ctrl+A / Ctrl+E (start / end of the line)
 *   selection   any of the moves above with Shift; Cmd+A selects everything; mouse drag;
 *               Shift+click extends; double-click selects a word, triple-click the whole text
 *   editing     typing or pasting replaces the selection; Backspace / Delete remove it or one
 *               character; with Option a word, with Cmd everything before / after the caret;
 *               Ctrl+K deletes to the end
 *   clipboard   Cmd+C / Cmd+X / Cmd+V (copy and cut are disabled on password fields; pasted line
 *               breaks become spaces)
 *   history     Cmd+Z / Cmd+Shift+Z, with consecutive typing grouped into one step
 *
 * Long text scrolls horizontally so the caret stays visible. Up / Down, Tab, Return and Esc are
 * left for the caller (lists, dialogs and search bars give them their own meaning).
 * Positions are byte offsets into buf, always on a UTF-8 boundary.
 */
#define TF_UNDO 8
typedef struct TfSnap { char buf[256]; short len, cursor, anchor; } TfSnap;
typedef struct TextField {
    char buf[256];
    int len, cursor;      /* bytes */
    int anchor;           /* other end of the selection; == cursor when nothing is selected */
    bool password;
    Uint64 blink;
    Rect rect;
    float scroll;         /* horizontal scroll of the text, render pixels */
    bool dragging;        /* a mouse selection started in this field is in progress */
    Uint64 input_frame;   /* frame whose keys / text were applied: a second call in the same frame
                           * (dialogs draw in a separate pass) only draws */
    int last_op;          /* kind of the last edit, to group typing into one undo step */
    TfSnap undo[TF_UNDO]; int nundo;
    TfSnap redo[TF_UNDO]; int nredo;
} TextField;

/* Replaces the content, puts the caret at the end and clears the selection and the history. */
void textfield_set(TextField *t, const char *s);
/* True while a focused text field was drawn in the last frame: global shortcuts then leave the
 * editing keys (Cmd+A / C / X / V / Z) to the field. */
bool ui_text_editing(void);
/* Right-click menu of text fields (Undo, Redo, Cut, Copy, Paste, Delete, Select All). One menu
 * for the whole app: main.c calls the update before any widget reads input (it owns the input
 * while open) and the draw after everything else. The chosen action is applied the next time
 * its field is drawn, so a field that disappears meanwhile is never touched. */
void ui_textfield_menu_update(App *a);
void ui_textfield_menu_draw(App *a);
bool ui_textfield_menu_pos(App *a, const char *label, float *x, float *y);   /* harness, window units */
/* Test harness: the field drawn focused most recently (NULL before any). */
const TextField *ui_focused_field(void);

/* Test-harness targets: every button / checkbox / segment (and whatever the shell registers)
 * records its label and rectangle each frame, so scripts click "Apply" instead of x, y. */
void ui_new_frame(void);                         /* clears the targets; called once per frame by main.c */
void ui_record(const char *label, Rect rc);      /* registers a widget: label (copied, max 63 bytes) and rect in render pixels */
bool ui_target(const char *prefix, Rect *out);   /* topmost widget of the last frame whose label starts with prefix */
/* Draws the field and, when `focused`, edits it with this frame's input. Returns true when
 * the field was clicked (caller moves focus). */
bool ui_textfield(App *a, TextField *t, Rect rc, const char *placeholder, bool focused);

/* Button styles: bordered default, filled accent, red destructive, borderless until hovered. */
enum { BTN_NORMAL, BTN_PRIMARY, BTN_DANGER, BTN_QUIET, BTN_SELECTED };   /* SELECTED: a toggle / tab that is on (never the primary blue) */
/* Push button; returns true on the frame it is pressed (disabled buttons never fire). */
bool ui_button(App *a, Rect rc, const char *label, int style, bool enabled);
/* Row of options; returns the newly selected index or -1. */
int ui_segmented(App *a, Rect rc, const char *const *labels, int n, int selected);
/* Box + label; returns true when toggled this frame (caller flips *value). */
bool ui_checkbox(App *a, Rect rc, const char *label, bool value, bool enabled);
/* Static text, ellipsised to rc; align is ALIGN_* (draw.h), color 0xRRGGBB. */
void ui_label(App *a, Rect rc, const char *text, int align, uint32_t color);

/*
 * Context menu with cascading submenus
 * ------------------------------------
 * Fill a menu with popup_add / popup_add_caption, then every frame call popup_update (input)
 * and popup_draw (last, on top of everything).
 *
 * Submenus: an item whose label ends with "  \u203A" (the "›" chevron) opens a submenu. When the
 * pointer rests on it, or the keyboard presses Right / Enter on it, popup_update returns that
 * item's id WITHOUT closing the menu; the owner then calls popup_open_sub(parent, child) and
 * fills the child like any menu. The child is placed beside its item: to the right when it
 * fits on screen, otherwise to the left, and moved up when it would leave the bottom edge.
 * The parent stays visible. Left / Esc close the deepest submenu; moving to another item of
 * the parent switches or closes it after a short delay (so a diagonal move does not flicker);
 * a click outside every open level closes them all.
 *
 * The child's storage belongs to the owner (a second PopupMenu next to the first); its labels
 * must stay valid while it is open, so use buffers that the parent does not use.
 */
#define POPUP_MAX 32
typedef struct PopupMenu {
    bool open;
    float x, y;                    /* anchor of a top-level menu (ignored for a submenu) */
    int n;                         /* number of items */
    const char *labels[POPUP_MAX]; /* borrowed, not copied */
    int ids[POPUP_MAX];            /* returned by popup_update when the item is chosen; -1 for captions */
    bool sep_before[POPUP_MAX];    /* draw a separator line above the item */
    bool enabled[POPUP_MAX];
    bool caption[POPUP_MAX];       /* non-interactive section label */
    bool is_sub[POPUP_MAX];        /* item opens a submenu (label ends with the chevron) */
    uint32_t color[POPUP_MAX];     /* caption colour */
    int kb;                        /* item highlighted from the keyboard (US-054); -1 = none */
    /* cascade links */
    struct PopupMenu *parent;      /* NULL for a top-level menu */
    struct PopupMenu *child;       /* open submenu, or NULL */
    int parent_item;               /* index in the parent of the item this submenu belongs to */
    int pending_sub;               /* item whose submenu was just requested (read by popup_open_sub) */
    bool pending_by_kb;            /* requested from the keyboard: highlight the child's first item */
    int hover_item; unsigned long long hover_since;   /* delayed switch / close on hover */
    int hold_item; float hold_x, hold_y;   /* submenu closed from the keyboard: no hover re-open until the mouse moves */
} PopupMenu;
/* Opens m as an empty top-level menu with its top-left at (x, y), render pixels. */
void popup_open(PopupMenu *m, float x, float y);
/* Opens `child` as the submenu of the item `parent` just requested (see above) and returns it. */
PopupMenu *popup_open_sub(PopupMenu *parent, PopupMenu *child);
void popup_close(PopupMenu *m);                     /* closes m and every submenu below it */
int popup_step(PopupMenu *m, int from, int dir);   /* next enabled item from `from` in `dir`; -1 wraps */
/* Appends an item (label not copied; must outlive the open menu). */
void popup_add(PopupMenu *m, int id, const char *label, bool sep_before, bool enabled);
/* Appends a non-interactive caption; color 0xRRGGBB, 0 = default dim text. */
void popup_add_caption(PopupMenu *m, const char *label, uint32_t color, bool sep_before);
/* Handles input for the whole cascade (call it on the top-level menu). Returns the chosen id,
 * a submenu item's id when its submenu should open, or -1. Consumes input while open. */
int popup_update(App *a, PopupMenu *m);
void popup_draw(App *a, PopupMenu *m);              /* draws the menu and its open submenus */
/* rect (render pixels) of the first enabled item whose label starts with `prefix`, searched in
 * the deepest open submenu first; for the test harness so scripts name items, not coordinates */
bool popup_item_rect(App *a, PopupMenu *m, const char *prefix, Rect *out);

/* Modern overlay scrollbar (vertical): hidden at rest, a thin thumb while scrolling that fades out,
 * wider on hover (easier to grab), drag the thumb or click the track to jump. It sits over the
 * content's right edge. Call ui_scrollbar_input BEFORE drawing the content (it takes the press on
 * the bar), ui_scrollbar_draw AFTER; ui_scrollbar_touch when the content scrolled by the wheel. */
typedef struct ScrollBar { double active_at; float grow; bool drag; float grab; float last; } ScrollBar;   /* any change of position shows the bar */
void ui_scrollbar_touch(ScrollBar *sb);
bool ui_scrollbar_input(App *a, ScrollBar *sb, Rect view, float content_h, float *scroll);
void ui_scrollbar_draw(App *a, ScrollBar *sb, Rect view, float content_h, float scroll);
/* the horizontal one, along the bottom edge */
bool ui_hscrollbar_input(App *a, ScrollBar *sb, Rect view, float content_w, float *scroll);
void ui_hscrollbar_draw(App *a, ScrollBar *sb, Rect view, float content_w, float scroll);
/* lists that keep their scroll in rows (scroll_rows may be fractional) */
bool ui_scrollbar_input_rows(App *a, ScrollBar *sb, Rect view, float rows, float row_h, float *scroll_rows);
void ui_scrollbar_draw_rows(App *a, ScrollBar *sb, Rect view, float rows, float row_h, float scroll_rows);
