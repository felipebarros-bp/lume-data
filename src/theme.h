/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/* Dark theme palette (0xRRGGBB, turn into a Color with color_hex()) and layout metrics
 * (UI points; convert to render pixels with px()). T_* are colors, T_SYN_* the editor's syntax
 * highlighting colors, M_* sizes. Header-only; included wherever something is drawn. */

#define T_BG          0x121316   /* deepest chrome: menu bar, tab strip, grid header */
#define T_PANEL       0x16181D   /* side/bottom chrome */
#define T_PANEL_ALT   0x1B1D23   /* panel headers, status bar */
#define T_EDITOR_BG   0x1B1E24   /* editor text area */
#define T_LINE_HL     0x22262E   /* editor current-line highlight */
#define T_BORDER      0x2A2E36   /* 1-px separators and outlines */
#define T_HOVER       0x22262E   /* hovered rows / buttons */
#define T_SELECTED    0x2A3A5C   /* selected rows, text selection */
#define T_ACCENT      0x5B8DEF   /* highlight color for active / focused elements */
#define T_ACCENT_DIM  0x3A4F80   /* muted accent (e.g. the table icon header) */
/* Text tiers (contrast on T_PANEL): primary 12:1, secondary 6.4:1, tertiary 4.6:1 */
#define T_TEXT        0xD9DDE4
#define T_TEXT_MUTED  0x9AA3B0
#define T_TEXT_DIM    0x7A8290
/* status colors: success, error, warning */
#define T_GREEN       0x5FD39B
#define T_RED         0xF27070
#define T_YELLOW      0xE8C266
#define T_SCROLLBAR   0x3A3D48   /* scrollbar thumb */

/* SQL syntax highlighting (editor.c) */
#define T_SYN_KEYWORD 0xC792EA
#define T_SYN_STRING  0xA5D6A7
#define T_SYN_NUMBER  0xF78C6C
#define T_SYN_COMMENT 0x7A8290
#define T_SYN_FUNC    0x82AAFF
#define T_SYN_OP      0x89DDFF
#define T_SYN_PUNCT   0x9AA3B0

/* layout metrics, in UI points */
#define M_MENUBAR     30.0f
#define M_STATUSBAR   24.0f
#define M_HEADER      30.0f   /* panel header height */
#define M_ROW         24.0f   /* list row height (Explorer, menus) */
#define M_GRID_ROW    22.0f   /* results grid row / header heights */
#define M_GRID_HEAD   26.0f
#define M_FONT_UI     13.0f   /* font sizes: UI, small labels, editor monospace */
#define M_FONT_SMALL  11.0f
#define M_FONT_MONO   13.0f
#define M_RADIUS      5.0f    /* default corner radius */
#define M_SIDEBAR_MIN 180.0f  /* sidebar width limits and minimum results height */
#define M_SIDEBAR_MAX 520.0f
#define M_RESULTS_MIN 120.0f
