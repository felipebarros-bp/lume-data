/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/* Query History panel (TBR-034): executions from history.tsv, filterable, with Open / Copy /
 * Re-run. Registered as a module before panels_create: panels_add_module("lume.history", history_module_init). */
#include "panels.h"

void history_module_init(Panels *p, App *a);
