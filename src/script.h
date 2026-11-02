/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "lume.h"
/* Event-injection harness: LUME_SCRIPT=<file> replays UI actions through the SDL event queue.
   Commands (coordinates in window units, i.e. SDL event coordinates): wait <ms> | move x y |
   click x y | dblclick x y | tripleclick x y | rclick x y | drag x0 y0 x1 y1 |
   key <name> (e.g. cmd+Return, shift+Right) | type <text> | wheel x y <dy> [dx] |
   mods shift|cmd|alt|ctrl|none | shot <file.bmp> | resize w h | quit.
   Semantic targets (found by label, stable across layout changes): button / rbutton / dbutton
   "Label", node / dnode / rnode "Label", cell / dcell / rcell R C, head / rhead C, menu "Label",
   dragcells R0 C0 R1 C1, dragto "from" "to", hold / release.
   Inspection (prints a line on stdout): stat <label> | fieldstate | checkfloat.
   See script.c for the full list. */

/* Load the script file at `path` (its lines are kept for the process lifetime) and schedule the
   first command shortly after start-up. Called once from main.c when LUME_SCRIPT is set. */
void script_load(App *a, const char *path);
/* Run the next command if it is due, then schedule a wake-up for the one after. Call once per
   frame; does nothing when no script is loaded or it has finished. */
void script_tick(App *a);
