/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Execution Plan panel (EPL-001..084): the graphical plan next to Results and Output, its
 * operators table, raw text, evidence-based findings, comparison, and the opt-in Lume Intelligence
 * analysis. The model and parsers are in plan.h; the AI client in ai.h.
 */
#include "lume.h"

void planview_init(App *a);          /* registers the panel and its commands */
void planview_shutdown(App *a);
/* Explain the statement on connection c (mode PM_ESTIMATED / PM_ACTUAL; the caller confirmed an
 * actual plan). The plan opens in the panel when it arrives. */
void planview_explain(App *a, Connection *c, const char *stmt, int len, int mode, const char *note);
bool planview_supported(const char *driver, bool actual);
void planview_pre_update(App *a);    /* jobs */
void planview_draw_overlay(App *a);  /* menus */
/* harness: plan <op> [args]; planstate */
void planview_cmd(App *a, const char *arg);
void planview_dump(App *a);
char *planview_context_for(const char *conn_name);
bool planview_compare_sql(App *a, Connection *c, const char *sql);   /* a rewrite's estimated plan beside the current one */   /* the current plan for the assistant (malloc'd), NULL when none for that connection */
