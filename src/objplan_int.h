/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/* Helpers shared by the planner core and the engine adapters (not a public API). */
#include "objengine.h"
#include <stdarg.h>

void plan_sql(ObjPlan *p, const char *fmt, ...);           /* appends a statement */
void plan_raw(ObjPlan *p, const char *s, int len);         /* appends text as one statement */
void plan_create(ObjPlan *p, const char *s, int len);      /* appends the user's CREATE and marks it */
void plan_impact(ObjPlan *p, const char *fmt, ...);
void plan_pre(ObjPlan *p, const char *fmt, ...);
void plan_block(ObjPlan *p, const char *fmt, ...);         /* first reason wins */
void plan_deps_impact(ObjPlan *p, const ObjSnapshot *b, const char *what);
/* "schema"."name" with the dialect's quoting (double quotes, `backticks`, [brackets]) */
void obj_qname(const ObjKey *k, int dialect, char *out, int n);
void obj_qtable(const char *schema_dot_name, int dialect, char *out, int n);
/* SQL string literal with quotes doubled: 'it''s' */
void obj_qlit(const char *s, char *out, int n);
/* the definition range without trailing ';' (views sent to Oracle / SQL statements that must not end with one) */
int obj_trim_semicolon(const char *s, int a, int e);
/* PostgreSQL helpers used by its adapter */
void pg_norm_signature(const char *sig, char *out, int n);
void pg_norm_result(const char *r, char *out, int n);
