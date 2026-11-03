/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "lume.h"
#include "conn.h"

/* Modal "New / Edit connection" dialog, drawn in-app.
 *
 * Edits a draft copy of a Connection (driver picker, name, the driver's fields from the
 * registry in conn.h) and only writes to the ConnStore on Save. Also offers Test (a
 * blocking connect + close) and Delete (two clicks). UI thread only. */
/* Modal "New / Edit connection" dialog, drawn in-app. */
/* Allocates a closed dialog; free with dialog_destroy. */
Dialog *dialog_create(void);
/* Frees the dialog (the connections it edited belong to the store). */
void dialog_destroy(Dialog *d);
/* Opens the dialog on a copy of `existing` (which must stay in the store while the dialog
 * is open), or on SQLite defaults for a new connection. */
void dialog_open(Dialog *d, App *a, Connection *existing);   /* NULL = new */
/* A new connection prefilled from src (name "... (copy)", same driver, fields and labels) but with
 * every password field empty: saving it creates a separate connection. */
void dialog_open_copy(Dialog *d, App *a, const Connection *src);
/* True while the dialog is shown. */
bool dialog_is_open(Dialog *d);
/* A new connection whose Folder field starts as `folder` (the Explorer's "New Connection Here"). */
void dialog_open_in_folder(Dialog *d, App *a, const char *folder);
void dialog_update(Dialog *d, App *a);     /* handles input; call before every other panel */
void dialog_draw(Dialog *d, App *a);       /* overlay; call last */
/* Result of the Browse file picker: fills the driver's file field and, when the name is
 * still empty, names the connection after the file. */
void dialog_file_chosen(Dialog *d, const char *path);
bool dialog_testing(Dialog *d);   /* harness: Test connection is running */
void dialog_dump(Dialog *d);   /* harness: "dialogstate open=.. step=.. driver=.. ..." */
