/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * The operating system's secret store, for credentials the user pastes into Lume (the AI key):
 *
 *   macOS    Keychain (Security.framework, generic password item)
 *   Windows  Credential Manager (CredWrite / CredRead, generic credential)
 *   Linux    Secret Service (GNOME Keyring, KWallet) through libsecret, loaded at run time; without
 *            it Lume says there is no secret store and stores nothing (never a plain file)
 *
 * Secrets never go to Lume's files, logs or screens. Callers wipe the buffers they get back
 * (secstore_free). Main thread or worker: each call is self-contained.
 */
#include <stdbool.h>

/* the store of this platform ("macOS Keychain", ...) and whether it can be used now */
const char *secstore_name(void);
bool secstore_available(char *why, int n);
/* service: "Lume Data"; account: what the secret is for ("anthropic-api-key") */
bool secstore_set(const char *service, const char *account, const char *secret, char *why, int n);
char *secstore_get(const char *service, const char *account);   /* NULL when absent; free with secstore_free */
bool secstore_delete(const char *service, const char *account); /* true when gone (or never there) */
bool secstore_has(const char *service, const char *account);
void secstore_free(char *secret);                               /* wipes, then frees */
