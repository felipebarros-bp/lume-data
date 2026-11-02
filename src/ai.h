/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Lume Intelligence: an opt-in AI client (EPL-043..054 / 073..077): the Claude Messages API
 * (provider=anthropic / local), the OpenAI Responses API with an API key (provider=openai) or with
 * the user's ChatGPT plan (provider=chatgpt, see chatgpt.h).
 *
 *   Off by default: provider=none until <prefs>/ai.txt says otherwise. Keys come from the
 *   environment or the OS secret store and are written only to a 0600 header file for the curl
 *   child; they are never logged, printed, put on a command line or saved in a file.
 *
 *   Transport: /usr/bin/curl as a child process (posix_spawn, no shell), on a worker thread; the
 *   UI polls. Cancel kills the child. https endpoints only (plus loopback for tests).
 *
 *   What goes out is decided by the caller and shown to the user first (plan_context + a
 *   consent); what comes back is text for the user to read: it never runs SQL, never grants a
 *   permission, never calls a tool.
 */
#include <stdbool.h>
#include <stddef.h>

typedef struct AiConfig {
    char provider[16];       /* "none" (default), "anthropic", "openai" (API key), "chatgpt" (the user's ChatGPT plan), "local" */
    char model[64];          /* claude-opus-5-5 */
    char model_fast[64];     /* optional: quick requests (Explain, Fix the error, Cmd+K); "" = model */
    char endpoint[256];      /* https://api.anthropic.com/v1/messages */
    char effort[12];         /* low / medium / high / xhigh / max */
    int max_tokens, timeout_s;
    bool fallback;           /* server-side refusal fallback ("default" routing) */
    bool ro_required;        /* require_readonly_ai_sql=1: the assistant's SQL runs only on a read-only login (BL-051) */
    bool stream;             /* ask the provider to stream (Anthropic SSE); ai_partial then shows the text so far */
    const char *key;         /* a key being tested before it is saved (never persisted); NULL = environment, then the OS secret store */
} AiConfig;

void ai_config_default(AiConfig *c);
void ai_config_load(const char *prefs_dir, AiConfig *c);
/* true when a request can be sent; why says what is missing (no secret in it) */
bool ai_ready(const AiConfig *c, char *why, int n);
/* where the key comes from: the environment wins over the OS secret store (Keychain / Credential
 * Manager / Secret Service), where the panel saves a pasted key. The store is checked once and
 * cached; call ai_key_changed after saving or removing it. */
#define AI_KEY_SERVICE "Lume Data"
#define AI_KEY_ACCOUNT "anthropic-api-key"
#define AI_OPENAI_KEY_ACCOUNT "openai-api-key"
enum { AK_NONE, AK_ENV, AK_STORE };
int ai_key_source(void);                       /* Anthropic */
int ai_key_source_for(const char *provider);   /* "anthropic" / "openai" */
void ai_key_changed(void);
/* provider defaults: endpoint (and, for Anthropic, the model) */
void ai_provider_defaults(AiConfig *c, const char *provider);
const char *ai_provider_label(const char *provider);   /* "Anthropic", "OpenAI", "ChatGPT plan", "Local model" */

/* a synchronous HTTPS call through the curl child (worker threads only): headers and body go
 * through 0600 temp files, never the command line; out is malloc'd */
bool ai_http(const char *method, const char *url, const char *headers, const char *body, int timeout_s, char **out, int *http, char *err, int en);
/* models offered to this account: OpenAI (API key or ChatGPT plan); names into out, count */
int ai_list_models(const AiConfig *c, char out[][96], int max, char *err, int en);

/* the request body (tests check it); malloc'd */
char *ai_request_json(const AiConfig *c, const char *system, const char *user);
/* the response: text (malloc'd), model, stop reason, tokens; false with err (HTTP / API error / refusal) */
typedef struct AiResult { char *text; char model[64], stop_reason[32], err[512]; int in_tokens, out_tokens, http; } AiResult;
bool ai_parse_response(const char *body, int http, AiResult *r);           /* Anthropic Messages */
bool ai_parse_response_for(const AiConfig *c, const char *body, int http, AiResult *r);   /* per provider (OpenAI Responses, ChatGPT SSE) */
void ai_result_free(AiResult *r);

typedef struct AiJob AiJob;
AiJob *ai_start(const AiConfig *c, const char *system, const char *user, const char *tmp_dir);
/* while a streamed request runs: the model's text received so far (text deltas only); "" before any.
 * Returns the number of bytes written. Safe to call every frame from the UI thread. */
size_t ai_partial(AiJob *j, char *out, size_t n);
bool ai_done(AiJob *j);
void ai_cancel(AiJob *j);
const AiResult *ai_result(AiJob *j);    /* after ai_done */
double ai_elapsed(AiJob *j);
void ai_free(AiJob *j);                 /* cancels and joins */

/* fenced ```sql blocks of an answer (candidate rewrites, never run); returns count */
int ai_sql_blocks(const char *text, char out[][4096], int max);
/* the system prompt for plan analysis (spec EP05, EPL-077) */
const char *ai_plan_system_prompt(void);
