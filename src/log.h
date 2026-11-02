/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Application log
 * ===============
 *
 * One plain-text file per installation, for debugging and for reading what happened before a
 * crash:
 *
 *     <preferences folder>/logs/lume.log        (current session and the ones before it)
 *     <preferences folder>/logs/lume.log.1      (previous file, after rotation at 5 MB)
 *
 * The preferences folder is LUME_PREF_DIR when set (tests), otherwise SDL's per-user folder
 * (~/Library/Application Support/Lume/lume on macOS).
 *
 * Line format:
 *
 *     2026-10-31 13:40:02.118  INFO   [main] Connected to Demo (in-memory) (SQLite)
 *     ^ local time, ms         ^level ^thread  ^message (newlines folded to spaces)
 *
 * Levels: DEBUG < INFO < WARN < ERROR. DEBUG is written only with LUME_LOG=debug in the
 * environment; it is the only level that may carry SQL text, because SQL can hold
 * sensitive literals.
 *
 * Crashes: log_init installs handlers for SIGSEGV, SIGBUS, SIGABRT, SIGILL and SIGFPE. The
 * handler appends "FATAL signal ..." and a symbolised backtrace to the log, then restores the
 * default action and re-raises the signal, so macOS still writes its own crash report.
 *
 * Thread safety: every function can be called from any thread (the SQL worker logs too).
 */
#include <stdarg.h>
#include <stdbool.h>

/* Severity levels, in increasing order; pass one of these to log_write(). */
enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR };

/* Opens (or creates) the log file, rotates it when it is over 5 MB, writes a session banner
 * and installs the crash handlers. Safe to call once at startup; later calls do nothing. */
void log_init(const char *app_version);
/* Where a crash leaves its report for the next launch (crashrep.c); "" before log_init. */
const char *log_crash_path(void);
/* Writes the crash report for the next launch (the crash handler; the test harness simulates a crash with it). */
void log_write_crash_report(int sig);

/* Writes the closing line of the session and closes the file. */
void log_shutdown(void);

/* Writes one line at the given level. `fmt` is printf-style. Messages below the active level
 * (INFO, or DEBUG with LUME_LOG=debug) are dropped without formatting cost. */
void log_write(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Same as log_write but takes an already started va_list (for wrappers that forward their
 * own variadic arguments). Does not call va_end on `ap`. */
void log_writev(int level, const char *fmt, va_list ap);

/* Names the calling thread in log lines ("main", "sql-worker", ...); default "thread". */
void log_set_thread_name(const char *name);

/* True when DEBUG lines are written (use it to skip building expensive debug strings). */
bool log_debug_enabled(void);

/* Absolute path of the current log file ("" before log_init or when it could not open). */
const char *log_path(void);

/* Shorthands: LOG_D / LOG_I / LOG_W / LOG_E(fmt, ...) log at DEBUG / INFO / WARN / ERROR. */
#define LOG_D(...) log_write(LOG_DEBUG, __VA_ARGS__)
#define LOG_I(...) log_write(LOG_INFO, __VA_ARGS__)
#define LOG_W(...) log_write(LOG_WARN, __VA_ARGS__)
#define LOG_E(...) log_write(LOG_ERROR, __VA_ARGS__)
