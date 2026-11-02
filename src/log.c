/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Application log: see log.h for the file location, the line format and the crash handler.
 *
 * State is a handful of file-scope globals: the open FILE (plus its raw descriptor for the
 * async-signal-safe crash path), the active level, the resolved path and a mutex that
 * serialises writers. Each thread carries its own name in thread-local storage. Everything
 * else in the app logs through the LOG_x macros in log.h, from the UI thread and from the SQL
 * worker alike.
 */
#include "log.h"
#include <SDL3/SDL.h>
#include <errno.h>
#ifndef _WIN32
#include <execinfo.h>
#endif
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LOG_ROTATE_BYTES (5L << 20)   /* rotate when the file grows past 5 MB */

static FILE *log_file;                 /* NULL until log_init succeeds */
static int log_fd = -1;                /* same file, raw descriptor for the crash handler */
static int log_level = LOG_INFO;   /* lowest level written; set once in log_init */
static char log_file_path[1100];   /* absolute path of lume.log, "" when not open */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;   /* serialises writers and log_shutdown */
static __thread char thread_name[24];  /* per-thread label shown in each line */

/* Fixed-width (5 character) label for a level, so the message column lines up. */
static const char *level_name(int level) {
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    default:        return "ERROR";
    }
}

/* ---- crash handler ----------------------------------------------------- */

/* Only async-signal-safe calls in here: write(), backtrace(), backtrace_symbols_fd(),
 * signal(), raise(). No stdio, no malloc, no locks. */
static void write_str(int fd, const char *s) { if (fd >= 0) (void)!write(fd, s, strlen(s)); }

/* Fatal-signal handler: writes a header and a backtrace to the log (and to stderr), flushes
 * the file to disk, then re-raises the signal with the default action so the process still
 * dies the normal way and the OS crash reporter runs. */
static void crash_handler(int sig) {
    char head[96];
    /* snprintf is not formally async-signal-safe but does not allocate for %d on Darwin */
    snprintf(head, sizeof head, "\n----- FATAL signal %d (%s): backtrace follows -----\n", sig,
             sig == SIGSEGV ? "segmentation fault" : sig == SIGBUS ? "bus error" : sig == SIGABRT ? "abort"
             : sig == SIGILL ? "illegal instruction" : sig == SIGFPE ? "arithmetic error" : "signal");
    write_str(log_fd, head);
    write_str(STDERR_FILENO, head);
    void *frames[64];
#ifndef _WIN32
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, log_fd >= 0 ? log_fd : STDERR_FILENO);
    if (log_fd >= 0) backtrace_symbols_fd(frames, n, STDERR_FILENO);
#else
    (void)frames;
    write_str(log_fd, "(no backtrace on Windows yet)\n");
#endif
    write_str(log_fd, "----- end of backtrace -----\n");
    if (log_fd >= 0) fsync(log_fd);
    /* let the system finish the job: default action, macOS crash report, non-zero exit */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Installs crash_handler for the fatal signals listed in log.h. SA_RESETHAND makes each
 * handler one-shot. */
static void install_crash_handlers(void) {
#ifdef _WIN32   /* the C runtime's signal(): no SIGBUS, no sigaction; the handler re-raises with SIG_DFL itself */
    const int wsigs[] = { SIGSEGV, SIGABRT, SIGILL, SIGFPE };
    for (size_t i = 0; i < sizeof wsigs / sizeof wsigs[0]; i++) signal(wsigs[i], crash_handler);
    return;
#else
    const int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = crash_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND;   /* a second fault inside the handler goes straight to default */
        sigaction(sigs[i], &sa, NULL);
    }
#endif
}

/* ---- setup ------------------------------------------------------------- */

/* Picks <prefs>/logs/, the same base folder the connection store uses. */
static void resolve_dir(char *out, size_t cap) {
    const char *env = getenv("LUME_PREF_DIR");
    if (env && *env) snprintf(out, cap, "%s%s", env, env[strlen(env) - 1] == '/' ? "" : "/");
    else {
        char *pref = SDL_GetPrefPath("lume", "Lume");
        snprintf(out, cap, "%s", pref ? pref : "./");
        SDL_free(pref);
    }
    size_t l = strlen(out);
    snprintf(out + l, cap - l, "logs/");
}

/* See log.h. Order matters: level from LUME_LOG, thread name, folder, rotation, open, crash
 * handlers, banner. The file is opened with O_APPEND and line buffering so every line reaches
 * disk before a crash; its raw descriptor is kept in log_fd for the signal handler. If the
 * file cannot be opened, logging falls back to stderr and log_path() returns "". */
void log_init(const char *app_version) {
    if (log_file) return;
    const char *lv = getenv("LUME_LOG");
    if (lv && (strcmp(lv, "debug") == 0 || strcmp(lv, "DEBUG") == 0)) log_level = LOG_DEBUG;
    log_set_thread_name("main");

    char dir[1024];
    resolve_dir(dir, sizeof dir);
    SDL_CreateDirectory(dir);
    snprintf(log_file_path, sizeof log_file_path, "%slume.log", dir);

    /* rotation: one previous file is kept as lume.log.1 */
    struct stat st;
    if (stat(log_file_path, &st) == 0 && st.st_size > LOG_ROTATE_BYTES) {
        char old[1110];
        snprintf(old, sizeof old, "%s.1", log_file_path);
        rename(log_file_path, old);
    }
    int fd = open(log_file_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    log_file = fd >= 0 ? fdopen(fd, "a") : NULL;
    if (!log_file) {
        fprintf(stderr, "lume: cannot open the log %s: %s\n", log_file_path, strerror(errno));
        log_file_path[0] = 0;
    } else {
        log_fd = fd;
        setvbuf(log_file, NULL, _IOLBF, 0);   /* one flush per line: nothing lost before a crash */
    }
    install_crash_handlers();
    LOG_I("===== Lume %s started (pid %d, %s, log level %s) =====", app_version, (int)getpid(),
          SDL_GetPlatform(), log_level == LOG_DEBUG ? "debug" : "info");
}

/* See log.h. Takes the lock so a concurrent writer never sees a half-closed FILE. */
void log_shutdown(void) {
    LOG_I("===== Lume exited normally =====");
    pthread_mutex_lock(&log_lock);
    if (log_file) fclose(log_file);
    log_file = NULL;
    log_fd = -1;
    pthread_mutex_unlock(&log_lock);
}

/* ---- writing ----------------------------------------------------------- */

/* Thread label is thread-local, so each thread names itself once at startup. Names longer
 * than 23 characters are truncated. NULL resets to "thread". */
void log_set_thread_name(const char *name) { snprintf(thread_name, sizeof thread_name, "%s", name ? name : "thread"); }
/* True when LUME_LOG=debug was set at log_init. */
bool log_debug_enabled(void) { return log_level == LOG_DEBUG; }
/* Points into static storage; valid for the life of the process. */
const char *log_path(void) { return log_file_path; }

/* Formats one message (truncated at 2 KB), folds newlines into spaces, prefixes it with the
 * timestamp, level and thread name and writes it under log_lock. Before log_init (or when the
 * file could not be opened) lines go to stderr. */
void log_writev(int level, const char *fmt, va_list ap) {
    if (level < log_level) return;
    char msg[2048];
    vsnprintf(msg, sizeof msg, fmt, ap);
    for (char *p = msg; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';   /* one event = one line */

    /* timestamp: local time with milliseconds */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    time_t sec = (time_t)tv.tv_sec;   /* a long on Windows, a time_t elsewhere */
    localtime_r(&sec, &tm);
    char when[32];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);

    pthread_mutex_lock(&log_lock);
    FILE *out = log_file ? log_file : stderr;   /* before log_init, lines still go somewhere */
    fprintf(out, "%s.%03d  %s  [%s] %s\n", when, (int)(tv.tv_usec / 1000), level_name(level),
            thread_name[0] ? thread_name : "thread", msg);
    pthread_mutex_unlock(&log_lock);
}

/* Variadic front end of log_writev. */
void log_write(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_writev(level, fmt, ap);
    va_end(ap);
}
