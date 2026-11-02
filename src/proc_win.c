/* SPDX-License-Identifier: AGPL-3.0-only */
/* Child processes on Windows: see proc_win.h. Compiled to nothing on the other platforms. */
#ifdef _WIN32
#include "proc_win.h"
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct WinProc { HANDLE proc, job; };

/* One argument, quoted the way CommandLineToArgvW / the C runtime split it back. */
static void quote_arg(const char *a, char **buf, size_t *len, size_t *cap) {
    size_t need = strlen(a) * 2 + 4;
    if (*len + need + 1 > *cap) { *cap = (*len + need + 1) * 2; *buf = realloc(*buf, *cap); }
    char *o = *buf + *len;
    bool plain = *a && !strpbrk(a, " \t\n\v\"");
    if (plain) { size_t k = strlen(a); memcpy(o, a, k); *len += k; (*buf)[*len] = 0; return; }
    *o++ = '"';
    for (const char *p = a;; p++) {
        size_t bs = 0;
        while (*p == '\\') { bs++; p++; }
        if (!*p) { for (size_t i = 0; i < bs * 2; i++) *o++ = '\\'; break; }
        if (*p == '"') { for (size_t i = 0; i < bs * 2 + 1; i++) *o++ = '\\'; *o++ = '"'; }
        else { for (size_t i = 0; i < bs; i++) *o++ = '\\'; *o++ = *p; }
    }
    *o++ = '"'; *o = 0;
    *len = (size_t)(o - *buf);
}

WinProc *winproc_start(const char *const *argv, const char *const *envp, const char *cwd, bool stderr_to_stdout,
                       int *in_fd, int *out_fd, int *err_fd, char *err, size_t en) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL, er_r = NULL, er_w = NULL;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0) || (!stderr_to_stdout && !CreatePipe(&er_r, &er_w, &sa, 0))) {
        snprintf(err, en, "pipe: error %lu", GetLastError()); goto fail;
    }
    /* our ends are not inherited by the child */
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    if (er_r) SetHandleInformation(er_r, HANDLE_FLAG_INHERIT, 0);
    size_t len = 0, cap = 256; char *cmd = malloc(cap); cmd[0] = 0;
    for (int i = 0; argv[i]; i++) { if (i) { cmd[len++] = ' '; cmd[len] = 0; } quote_arg(argv[i], &cmd, &len, &cap); }
    char *envblock = NULL;
    if (envp) {   /* "K=V\0K=V\0\0" */
        size_t el = 1; for (int i = 0; envp[i]; i++) el += strlen(envp[i]) + 1;
        envblock = calloc(1, el + 1); char *q = envblock;
        for (int i = 0; envp[i]; i++) { size_t k = strlen(envp[i]); memcpy(q, envp[i], k + 1); q += k + 1; }
    }
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in_r; si.hStdOutput = out_w; si.hStdError = stderr_to_stdout ? out_w : er_w;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li); li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li); }
    BOOL ok = CreateProcessA(argv[0], cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, envblock, cwd && *cwd ? cwd : NULL, &si, &pi);
    free(cmd); free(envblock);
    if (!ok) { snprintf(err, en, "cannot start %s: error %lu", argv[0], GetLastError()); if (job) CloseHandle(job); goto fail; }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    CloseHandle(in_r); CloseHandle(out_w); if (er_w) CloseHandle(er_w);
    *in_fd = _open_osfhandle((intptr_t)in_w, _O_WRONLY | _O_BINARY);
    *out_fd = _open_osfhandle((intptr_t)out_r, _O_RDONLY | _O_BINARY);
    if (err_fd) *err_fd = er_r ? _open_osfhandle((intptr_t)er_r, _O_RDONLY | _O_BINARY) : -1;
    WinProc *p = calloc(1, sizeof *p); p->proc = pi.hProcess; p->job = job;
    return p;
fail:
    if (in_r) CloseHandle(in_r); if (in_w) CloseHandle(in_w); if (out_r) CloseHandle(out_r); if (out_w) CloseHandle(out_w);
    if (er_r) CloseHandle(er_r); if (er_w) CloseHandle(er_w);
    return NULL;
}

long winproc_peek(int fd) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD avail = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return -1;   /* broken pipe: the writer closed (EOF) */
    return (long)avail;
}
bool winproc_exited(WinProc *p, int *code) {
    if (!p) return true;
    if (WaitForSingleObject(p->proc, 0) != WAIT_OBJECT_0) return false;
    DWORD c = 0; GetExitCodeProcess(p->proc, &c); if (code) *code = (int)c; return true;
}
int winproc_wait(WinProc *p) {
    if (!p) return -1;
    WaitForSingleObject(p->proc, INFINITE);
    DWORD c = 0; GetExitCodeProcess(p->proc, &c); return (int)c;
}
void winproc_kill(WinProc *p) {
    if (!p) return;
    if (p->job) TerminateJobObject(p->job, 1); else TerminateProcess(p->proc, 1);
}
void winproc_free(WinProc *p) {
    if (!p) return;
    CloseHandle(p->proc); if (p->job) CloseHandle(p->job);
    free(p);
}
const char *winproc_curl(void) {
    static char path[MAX_PATH] = "";
    if (!path[0]) {
        char sys[MAX_PATH]; UINT n = GetSystemDirectoryA(sys, sizeof sys);
        if (n && n < sizeof sys) { snprintf(path, sizeof path, "%s\\curl.exe", sys); if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) path[0] = 0; }
    }
    return path;
}
bool winproc_random(unsigned char *b, size_t n) {
    return BCryptGenRandom(NULL, b, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}
void winproc_open_url(const char *url) {
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}
#else
typedef int lume_proc_win_unused;   /* ISO C: a translation unit is never empty */
#endif
