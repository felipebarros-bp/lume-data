/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Custom extractors (US-027 / US-045): commands registered in extractors.txt that turn a TSV
 * of the rows into a file. They never go through a shell: the line is split into argv,
 * {file} / {out} are substituted per token, and the program runs in a separate process
 * inside an OS sandbox (macOS sandbox-exec, Linux bubblewrap when present) that can read
 * only system paths and a private scratch folder, write only inside that folder, and has
 * no network. Each entry must be approved once by the user (trust is bound to a hash of
 * the line, so an edited command needs a new approval). No UI here; tests/extract_check.c
 * exercises it headless. */

/* Maximum number of extractors read from extractors.txt. */
#define EXTRACT_MAX 8

/* One entry of extractors.txt. */
typedef struct Extractor {
    char name[48];   /* label shown in the export menu */
    char cmd[400];          /* program and arguments, {file} = input TSV, {out} = output file */
    int timeout_secs;       /* per-entry "timeout=N" (wall clock), else EXTRACT_DEFAULT_SECS */
    char hash[12];          /* fnv-1a of name + cmd + timeout; what the trust file records */
    bool trusted;   /* the user approved this exact entry (hash found in the trust file) */
    char origin[640];       /* file the entry came from */
} Extractor;

/* Wall-clock timeout when the entry sets none, and the cap applied to any entry. */
enum { EXTRACT_DEFAULT_SECS = 120, EXTRACT_MAX_SECS = 3600 };
/* Resource limits of one run: wall-clock and CPU seconds, memory and output-file size in MB. */
typedef struct ExtractLimits { int wall_secs, cpu_secs; long mem_mb, out_mb; } ExtractLimits;
/* Limits for an entry: its timeout (capped) for wall and CPU, 1024 MB memory, 4096 MB output.
 * e may be NULL. */
ExtractLimits extract_default_limits(const Extractor *e);

/* Outcome of extract_run. DENIED: not approved; BAD_COMMAND: program missing, relative or not
 * executable, or the line cannot be split; NO_OUTPUT: exit 0 but no {out} file. */
enum { EXTRACT_OK, EXTRACT_DENIED, EXTRACT_NO_SANDBOX, EXTRACT_BAD_COMMAND, EXTRACT_SPAWN_FAILED,
       EXTRACT_FAILED, EXTRACT_TIMEOUT, EXTRACT_CANCELLED, EXTRACT_NO_OUTPUT };
/* status (EXTRACT_*), the program's exit code (-1 when it did not exit normally), duration
 * and a one-line user-facing message. */
typedef struct ExtractResult { int status, exit_code; double secs; char message[240]; } ExtractResult;
/* Short static label of a status ("ok", "timeout", ...); "?" when unknown. */
const char *extract_status_name(int status);

/* extractors.txt: "name<TAB>command[<TAB>timeout=N]" per line; trust from extractors-trust.txt */
/* Reads <dir>extractors.txt (dir ends with a slash) into out, at most max entries, and
 * looks up each entry's approval. Comment lines (#) and malformed lines are skipped.
 * Returns the number read (0 when the file does not exist). */
int extract_load(const char *dir, Extractor *out, int max);
/* Approves (trust = true) or revokes an entry in <dir>extractors-trust.txt, rewriting it via
 * a temporary file and rename. Updates e->trusted on success. */
bool extract_trust(const char *dir, Extractor *e, bool trust);
/* True when the OS sandbox tool exists (sandbox-exec on macOS, bwrap on Linux). Without it
 * extractors refuse to run. */
bool extract_sandbox_available(void);
/* human-readable permission summary shown before an entry is approved */
void extract_describe(const Extractor *e, char *buf, size_t n);

/* One run: begin() creates the scratch folder and names the input file the caller writes;
 * run() executes the entry and, on success, moves its output to `dest`; end() removes the
 * scratch folder. pump() is called while waiting (return false to cancel). */
typedef struct ExtractRun { char scratch[1024], infile[1100], outfile[1100]; bool active; } ExtractRun;
/* Creates a private scratch folder under dir and sets run->infile (rows.tsv in it), which
 * the caller then writes. False with err filled when the folder cannot be created. */
bool extract_begin(const char *dir, ExtractRun *run, char *err, size_t errn);
/* Runs an approved entry in the sandbox with the given limits (NULL: defaults) and, on
 * success, moves the output to dest. Blocks the calling thread, calling pump(ud) about every
 * 15 ms; pump may be NULL. Returns res->status. */
int extract_run(ExtractRun *run, const Extractor *e, const char *dest, const ExtractLimits *l,
                bool (*pump)(void *ud), void *ud, ExtractResult *res);
/* Deletes the scratch folder and everything in it. Safe to call twice. */
void extract_end(ExtractRun *run);
/* audit line in extractors-log.txt: time, name, status, exit code, seconds, rows, destination
 * basename; never the data or the environment */
void extract_log(const char *dir, const Extractor *e, const ExtractResult *res, long rows, const char *dest);
