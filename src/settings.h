/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Settings (Lume > Settings..., Cmd+,): one modal screen over the workspace with its sections on the
 * left. Lume Intelligence is the first section (roadmap item 25: provider and model, the fast model,
 * consents and scope remembered per connection, the glossary, this session's token use); the other
 * sections of the Settings work (General, Appearance, Query Execution...) join it here.
 * The screen edits the same files the rest of Lume uses; it never holds a key or a password.
 */
#include "lume.h"

void settings_init(App *a);          /* registers app.settings.open */
void settings_open(App *a);
void settings_close(App *a);
bool settings_is_open(void);
void settings_pre_update(App *a);    /* per frame, early: the open screen owns the frame's input */
void settings_draw(App *a);          /* per frame, last */
void settings_dump(void);            /* harness: settingsstate */
Uint32 settings_event_type(void);    /* posted by the native app menu's Settings... (0 when there is none) */
