/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Child processes on Windows: what posix_spawn + pipe + poll + waitpid + kill do on the other platforms.
 * Used by ai.c (curl), gitcore.c (git) and chatgpt.c on Windows only; the POSIX code paths stay as they are.
 */
#ifndef LUME_PROC_WIN_H
#define LUME_PROC_WIN_H
#ifdef _WIN32
#include <stdbool.h>
#include <stddef.h>

typedef struct WinProc WinProc;

/* Starts argv[0] (a full path; argv is quoted for the command line) with its stdin / stdout / stderr on
 * pipes. envp NULL inherits the environment, else it is the complete environment ("K=V" strings). cwd may
 * be NULL. stderr_to_stdout sends both streams to *out_fd (err_fd may then be NULL). The descriptors are
 * C runtime ones (read / write / close). The whole process tree lives in a job: winproc_kill stops it all.
 * Returns NULL with err on failure. */
WinProc *winproc_start(const char *const *argv, const char *const *envp, const char *cwd, bool stderr_to_stdout,
                       int *in_fd, int *out_fd, int *err_fd, char *err, size_t en);
/* Bytes waiting on a pipe read end: > 0 data, 0 nothing yet, -1 closed (EOF) or error. */
long winproc_peek(int fd);
/* Non-blocking: true when the process ended (exit code in *code). */
bool winproc_exited(WinProc *p, int *code);
/* Blocks until the process ends; returns its exit code (-1 unknown). */
int winproc_wait(WinProc *p);
/* Terminates the process and everything it started. */
void winproc_kill(WinProc *p);
void winproc_free(WinProc *p);
/* %SystemRoot%\System32\curl.exe (Windows 10 1803+), or "" when missing. */
const char *winproc_curl(void);
/* Random bytes from the system generator. */
bool winproc_random(unsigned char *b, size_t n);
/* Opens a URL in the default browser. */
void winproc_open_url(const char *url);

#endif
#endif
