/* SPDX-License-Identifier: AGPL-3.0-only */
/* http.h on Windows: WinHTTP (part of Windows, no extra library). */
#ifdef _WIN32
#include "http.h"
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool http_available(char *why, size_t n) { (void)why; (void)n; return true; }

static wchar_t *wide(const char *s, int len) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    wchar_t *w = calloc((size_t)n + 1, sizeof *w);
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, len, w, n);
    return w;
}
static void win_err(char *err, size_t en, const char *what) {
    DWORD e = GetLastError();
    if (e == ERROR_WINHTTP_TIMEOUT) snprintf(err, en, "%s: timed out", what);
    else if (e == ERROR_WINHTTP_CANNOT_CONNECT) snprintf(err, en, "%s: cannot connect", what);
    else if (e == ERROR_WINHTTP_NAME_NOT_RESOLVED) snprintf(err, en, "%s: the name could not be resolved", what);
    else if (e == ERROR_WINHTTP_SECURE_FAILURE) snprintf(err, en, "%s: the TLS certificate was not accepted", what);
    else snprintf(err, en, "%s: error %lu", what, (unsigned long)e);
}

bool http_request(const char *method, const char *url, const char *headers, const char *body, size_t len,
                  int timeout_s, bool follow, volatile int *cancel, HttpChunkFn chunk, void *ud,
                  char **out, int *status, char *err, size_t en) {
    *out = NULL; *status = 0;
    bool https = !strncmp(url, "https://", 8);
    if (!https && strncmp(url, "http://", 7)) { snprintf(err, en, "Only http and https addresses are used"); return false; }
    wchar_t *wurl = wide(url, -1);
    URL_COMPONENTS uc; memset(&uc, 0, sizeof uc); uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = (DWORD)-1; uc.dwUrlPathLength = (DWORD)-1; uc.dwExtraInfoLength = (DWORD)-1;
    if (!wurl || !WinHttpCrackUrl(wurl, 0, 0, &uc)) { free(wurl); snprintf(err, en, "Not a valid address"); return false; }
    wchar_t *host = calloc(uc.dwHostNameLength + 1, sizeof *host); memcpy(host, uc.lpszHostName, uc.dwHostNameLength * sizeof *host);
    size_t pl = uc.dwUrlPathLength + uc.dwExtraInfoLength;
    wchar_t *path = calloc(pl + 2, sizeof *path);
    if (uc.dwUrlPathLength) memcpy(path, uc.lpszUrlPath, uc.dwUrlPathLength * sizeof *path); else path[0] = L'/';
    if (uc.dwExtraInfoLength) memcpy(path + (uc.dwUrlPathLength ? uc.dwUrlPathLength : 1), uc.lpszExtraInfo, uc.dwExtraInfoLength * sizeof *path);
    wchar_t *wmethod = wide(method ? method : (body ? "POST" : "GET"), -1);
    HINTERNET ses = NULL, con = NULL, req = NULL; bool ok = false;
    char *buf = NULL; size_t blen = 0, cap = 0;
    int t = (timeout_s > 0 ? timeout_s : 60) * 1000;
    ses = WinHttpOpen(L"Lume Data", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"Lume Data", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { win_err(err, en, "WinHTTP"); goto done; }
    WinHttpSetTimeouts(ses, 15000, 15000, t, t);
    con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) { win_err(err, en, "Connect"); goto done; }
    req = WinHttpOpenRequest(con, wmethod, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { win_err(err, en, "Request"); goto done; }
    if (!follow) { DWORD f = WINHTTP_DISABLE_REDIRECTS; WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &f, sizeof f); }
    if (headers && *headers) {   /* "A: b\nC: d" -> "A: b\r\nC: d\r\n" */
        size_t hl = strlen(headers); char *crlf = malloc(hl * 2 + 3); size_t k = 0;
        for (size_t i = 0; i < hl; i++) { if (headers[i] == '\n' && (i == 0 || headers[i - 1] != '\r')) crlf[k++] = '\r'; crlf[k++] = headers[i]; }
        crlf[k] = 0;
        wchar_t *wh = wide(crlf, -1); memset(crlf, 0, k); free(crlf);   /* a key may be in there */
        BOOL hok = wh && WinHttpAddRequestHeaders(req, wh, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        if (wh) { SecureZeroMemory(wh, wcslen(wh) * sizeof *wh); free(wh); }
        if (!hok) { win_err(err, en, "Headers"); goto done; }
    }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body, body ? (DWORD)len : 0, body ? (DWORD)len : 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) { win_err(err, en, "Could not reach the server"); goto done; }
    DWORD code = 0, cl = sizeof code;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &cl, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        if (cancel && *cancel) { snprintf(err, en, "Cancelled"); goto done; }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { win_err(err, en, "Reading the answer"); goto done; }
        if (!avail) break;
        if (blen + avail + 1 > cap) { cap = (blen + avail + 1) * 2; char *nb = realloc(buf, cap); if (!nb) { snprintf(err, en, "Out of memory"); goto done; } buf = nb; }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf + blen, avail, &got)) { win_err(err, en, "Reading the answer"); goto done; }
        if (!got) break;
        if (chunk) chunk(ud, buf + blen, got);
        blen += got; buf[blen] = 0;
    }
    if (!buf) buf = calloc(1, 1);
    *out = buf; buf = NULL; *status = (int)code; ok = true;
done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(buf); free(wurl); free(host); free(path); free(wmethod);
    return ok;
}
#else
typedef int lume_http_win_unused;   /* ISO C: a translation unit is never empty */
#endif
