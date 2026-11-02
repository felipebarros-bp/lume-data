/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/* Markdown notes module (PNL-047 example): two panels over <prefs>/notes.md, added through the
 * panel extension API only. Register with panels_add_module("lume.markdown", markdown_module_init)
 * before panels_create. */
#include "panels.h"

void markdown_module_init(Panels *p, App *a);
