/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Lume Git Core (GIT-001..071): the headless layer under Source Control. Git CLI only, never a
 * shell: every call is an argv vector run by posix_spawn with a timeout, a cancel flag, capped and
 * redacted output, and an environment that can never stop for a password prompt (credentials come
 * from the user's credential helper / SSH agent; Lume stores none). Parsers for the porcelain v2
 * status, unified diffs, log, refs and remotes; repository discovery (worktrees and submodules
 * included) and in-progress operation state; hunk patches for partial staging; the path policy for
 * what may be sent to an AI model. No SDL, no UI: tests/gitcore_check.c drives it.
 *
 * Git versions files. Nothing here talks to a database (rule 1: "Git is not deployment").
 */
#include <stdbool.h>
#include <stddef.h>

/* ---- executable (GIT-005) ---- */
/* finds git: the preferred path when valid, else LUME_GIT, else the usual install locations.
 * version receives "2.55.0". false with why when none works. */
bool gitc_find(const char *preferred, char *path, int pn, char *version, int vn, char *why, int wn);

/* ---- running (GIT-031 / 063 / 070) ---- */
typedef struct GitOut {
    int exit_code;               /* -1 when it did not exit normally */
    bool timed_out, cancelled, spawn_failed, truncated;
    char *out; size_t out_len;   /* stdout (malloc'd, NUL-terminated) */
    char err[4096];              /* stderr, redacted, last 4 KB */
    double ms;
} GitOut;
typedef struct GitOpts {
    int timeout_ms;              /* 0 = 60 s */
    size_t max_out;              /* 0 = 64 MB */
    volatile int *cancel;        /* set to 1 from another thread to stop the process */
    const char *stdin_data;      /* fed to the process, then closed (patches) */
    bool no_optional_locks;      /* GIT_OPTIONAL_LOCKS=0 for read-only status */
} GitOpts;
bool gitc_run(const char *git, const char *cwd, const char *const *argv, const GitOpts *o, GitOut *out);
void gitc_out_free(GitOut *o);
/* the environment Lume adds (tests check it): no prompts, batch SSH, C locale, no pager / editor */
int gitc_env_extra(const char **out, int max);

/* ---- secrets (GIT-026 / 065 / 070) ---- */
/* masks user:password@ in URLs, tokens (ghp_, github_pat_, glpat-, Bearer ...), and password= */
void gitc_redact(const char *in, char *out, size_t n);

/* ---- repositories (GIT-001 / 004 / 025 / 067) ---- */
/* walks up from path to the first .git (directory, or file "gitdir: ..." of a worktree / submodule) */
bool gitc_find_root(const char *path, char *root, int rn, char *gitdir, int gn);
enum { GS_NONE, GS_MERGING, GS_REBASING, GS_CHERRY_PICKING, GS_REVERTING, GS_BISECTING };
int gitc_repo_state(const char *gitdir);
const char *gitc_state_name(int st);

/* ---- status: git status --porcelain=v2 --branch -z (GIT-009) ---- */
enum { GE_ORDINARY, GE_RENAME, GE_UNMERGED, GE_UNTRACKED };
typedef struct GitEntry { char x, y; int kind; char *path, *orig; bool submodule; } GitEntry;
typedef struct GitStatus {
    char head[256], oid[48], upstream[256];
    int ahead, behind; bool has_upstream, detached, initial;
    GitEntry *e; int n, cap;
    int staged, unstaged, untracked, conflicts;
} GitStatus;
bool gitc_status_parse(const char *z, size_t len, GitStatus *st);
void gitc_status_free(GitStatus *st);
bool gitc_entry_staged(const GitEntry *e);      /* index side changed */
bool gitc_entry_unstaged(const GitEntry *e);    /* worktree side changed (untracked included) */
const char *gitc_entry_label(char c);           /* 'M' -> "modified" ... (text, not only colour: GIT-068) */

/* ---- diff (GIT-011 / 012) ---- */
typedef struct GitDLine { char t; int old_no, new_no; const char *s; int len; } GitDLine;   /* t: ' ', '+', '-', '\\' */
typedef struct GitHunk { int old_start, old_n, new_start, new_n; char header[160]; int first, nlines; } GitHunk;
typedef struct GitDFile {
    char *old_path, *new_path; bool binary, is_new, is_deleted, renamed;
    char *header;                /* "diff --git ..." through the "+++" line, for patches */
    GitHunk *h; int nh;
} GitDFile;
typedef struct GitDiff { char *text; GitDFile *f; int nf; GitDLine *l; int nl; int adds, dels; } GitDiff;
bool gitc_diff_parse(const char *text, GitDiff *d);   /* text is copied */
void gitc_diff_free(GitDiff *d);
/* the patch of one hunk (for git apply --cached [--reverse]); malloc'd */
char *gitc_hunk_patch(const GitDiff *d, int file, int hunk);
/* side by side: pairs '-' / '+' runs line by line; rows of (left index or -1, right index or -1) */
int gitc_side_by_side(const GitDiff *d, int file, int (*rows)[2], int max);

/* ---- log, refs, remotes (GIT-018..020 / 026 / 034) ---- */
#define GITC_LOG_FORMAT "%H%x1f%h%x1f%an%x1f%ae%x1f%aI%x1f%s%x1f%D%x1e"
typedef struct GitCommit { char hash[48], shorth[16], author[96], email[128], date[32], subject[256], refs[256]; } GitCommit;
int gitc_log_parse(const char *text, GitCommit *out, int max);
#define GITC_REF_FORMAT "%(refname)%1f%(refname:short)%1f%(objectname:short)%1f%(upstream:short)%1f%(HEAD)%1f%(upstream:track)%1e"
typedef struct GitRef { char name[200], shortn[160], oid[16], upstream[160], track[64]; bool remote, head; } GitRef;
int gitc_refs_parse(const char *text, GitRef *out, int max);
typedef struct GitRemote { char name[64], fetch[512], push[512]; } GitRemote;   /* URLs redacted */
int gitc_remotes_parse(const char *text, GitRemote *out, int max);
/* GitHub / GitLab / Azure DevOps / self-hosted, from a remote URL (no API call) */
const char *gitc_provider(const char *url);

/* ---- names and messages (GIT-015 / 018) ---- */
bool gitc_branch_name_ok(const char *name, char *why, int n);   /* git check-ref-format rules */
/* summary required, <= 72 chars recommended; returns false with why for a blocking problem,
 * warn for advice (long summary) */
bool gitc_message_ok(const char *summary, char *why, int n, char *warn, int wn);

/* ---- what may go to an AI model (GIT-058 / 062) ---- */
/* .env, keys, certificates, credentials, Lume's connections / secrets files... never sent */
bool gitc_path_sensitive(const char *path);
