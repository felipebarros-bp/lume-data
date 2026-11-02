/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Crash reports, only with the user's consent. When Lume crashes, the handler in log.c leaves
 * <prefs>/logs/crash-pending.txt (version, build, signal, stack). On the next launch, per Settings >
 * Privacy (ask each time, the default / always send / never send), the report is offered, sent or dropped.
 *
 * What is sent (to GlitchTip, as a Sentry-format event): the version, build and channel, the OS name,
 * major version and architecture, the signal and the stack frames (function names, the binary's base
 * name, addresses). Never: paths, user or host names, SQL, data, connection names. No installation id.
 * Under the test harness only a loopback LUME_CRASH_URL is ever used.
 */
#ifndef LUME_CRASHREP_H
#define LUME_CRASHREP_H
#include "lume.h"

enum { CRASH_ASK, CRASH_ALWAYS, CRASH_NEVER };

/* At startup: a report left by a crash is offered / sent / dropped per the user's choice. */
void crashrep_startup(App *a);
/* The dialog's answer: 0 send, 1 view (opens the exact JSON in a tab, then asks again), 2 don't send. */
void crashrep_answer(App *a, int choice);
/* The event that would be sent for the pending report (malloc'ed JSON), or NULL when none is pending. */
char *crashrep_event_json(void);

#endif
