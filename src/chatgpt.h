/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * "Continue with ChatGPT": the user's ChatGPT plan for Lume Intelligence (OpenAI's Sign in with
 * ChatGPT for open-source apps, https://developers.openai.com/siwc/token-sharing-open-source/).
 *
 *   OAuth 2.0 authorization code + PKCE (S256), loopback redirect http://127.0.0.1:1455/auth/callback,
 *   dynamic client registration (client_id=dynamic_agent_client the first time; the issued
 *   oaiapp_... id is kept and reused). Scopes: openid profile email offline_access resource.invoke
 *   chatgpt.tokens.use.direct; resource=https://api.openai.com/v1.
 *
 *   The session (tokens, email) lives in the OS secret store (secstore.h), never in a file.
 *   Access tokens last an hour and are refreshed on the worker before a request; refresh tokens
 *   rotate, refreshes are serialised, and the newest replacement is stored at once. A rejected
 *   refresh clears the session: the user signs in again.
 *
 *   Test overrides (loopback only): LUME_CHATGPT_AUTH_BASE, LUME_OPENAI_API_BASE, LUME_TEST_BROWSER=curl.
 */
#include <stdbool.h>

void chatgpt_init(const char *prefs_dir);   /* where the non-secret client id / host id are kept */

/* sign-in: starts the loopback listener and returns the URL to open in the browser */
bool chatgpt_signin_begin(char *url, int un, char *why, int wn);
enum { CS_IDLE, CS_WAITING, CS_EXCHANGING, CS_DONE, CS_FAILED };
int chatgpt_signin_status(char *msg, int mn);
void chatgpt_signin_cancel(void);
/* opens the URL in the user's browser (SDL_OpenURL in the app; a curl child under LUME_TEST_BROWSER) */
void chatgpt_open_browser(const char *url);
void chatgpt_open_url(const char *url);    /* any page (Manage usage): the system browser, never curl */

/* the stored session: signed in, with the plan-usage scope */
bool chatgpt_signed_in(char *email, int en);
void chatgpt_sign_out(void);
/* a fresh access token (refreshed when it expires within two minutes); malloc'd, wipe with
 * secstore_free. Worker threads only (it may call the network). false with why: sign in again. */
bool chatgpt_access_token(char **token, char *why, int wn);
void chatgpt_changed(void);   /* forget the cached sign-in state */

/* base URLs (overridable in tests) */
const char *chatgpt_auth_base(void);
const char *openai_api_base(void);
