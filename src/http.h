/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * HTTPS requests from inside Lume (no external program): the system's libcurl loaded at run time on
 * macOS and Linux (http_curl.c), WinHTTP on Windows (http_win.c). Used by the AI providers (ai.c) and
 * the ChatGPT sign-in (chatgpt.c). Keys and tokens travel only in memory, as request headers.
 */
#ifndef LUME_HTTP_H
#define LUME_HTTP_H
#include <stdbool.h>
#include <stddef.h>

/* Called with each piece of the answer as it arrives (streamed answers). */
typedef void (*HttpChunkFn)(void *ud, const char *buf, size_t len);

/* One request. headers: "Name: value" lines separated by '\n' (NULL none). body: NULL for none
 * (len bytes otherwise). Only http:// and https:// are used, redirects are not followed unless
 * follow is set. *cancel (may be NULL) is checked while the request runs. On success *out holds the
 * answer body (malloc'ed, NUL-terminated) and *status the HTTP status; on failure err says why and
 * *out is NULL. A timeout_s <= 0 means 60 seconds. */
bool http_request(const char *method, const char *url, const char *headers, const char *body, size_t len,
                  int timeout_s, bool follow, volatile int *cancel, HttpChunkFn chunk, void *ud,
                  char **out, int *status, char *err, size_t en);
/* Whether requests can be made here (the HTTP library loaded); why says what is missing. */
bool http_available(char *why, size_t n);

#endif
