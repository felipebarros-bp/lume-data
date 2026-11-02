/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Lume Git Core, headless, against real Git in temporary repositories (GIT-001 / 005 / 009..012 /
 * 018..020 / 023 / 025 / 026 / 029..031 / 034 / 062 / 063 / 065 / 067 / 069 / 071): executable,
 * runner (argv only, timeout, cancel, no prompts), redaction, discovery (worktree .git file, Unicode
 * and spaces), porcelain v2 status, diff / side by side / one-hunk staging, log, refs, remotes,
 * provider, names, messages, sensitive paths, merge state, 1,000-file status, credential-less push.
 */
#include "gitcore.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int fails, oks;
#define EXPECT(cond, ...) do { if (cond) { oks++; printf("ok    "); } else { fails++; printf("FAIL  "); } printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
static char GIT[512];
static char BASE[1024];

static bool g(const char *cwd, GitOut *o, ...) {   /* git with NULL-terminated args */
    const char *av[32]; int n = 0; va_list ap; va_start(ap, o); const char *s; while ((s = va_arg(ap, const char *)) && n < 31) av[n++] = s; va_end(ap); av[n] = NULL;
    GitOpts op = { .timeout_ms = 30000 };
    return gitc_run(GIT, cwd, av, &op, o);
}
static bool gq(const char *cwd, ...) {
    const char *av[32]; int n = 0; va_list ap; va_start(ap, cwd); const char *s; while ((s = va_arg(ap, const char *)) && n < 31) av[n++] = s; va_end(ap); av[n] = NULL;
    GitOpts op = { .timeout_ms = 30000 }; GitOut o; bool ok = gitc_run(GIT, cwd, av, &op, &o);
    if (!ok) printf("      git %s failed: %s\n", av[0], o.err);
    gitc_out_free(&o); return ok;
}
static void wfile(const char *dir, const char *name, const char *text) { char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "w"); fputs(text, f); fclose(f); }
static bool status(const char *repo, GitStatus *st) {
    const char *av[] = { "status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all", NULL };
    GitOpts op = { .timeout_ms = 30000, .no_optional_locks = true }; GitOut o;
    bool ok = gitc_run(GIT, repo, av, &op, &o);
    if (ok) gitc_status_parse(o.out, o.out_len, st);
    gitc_out_free(&o); return ok;
}
static const GitEntry *entry(const GitStatus *st, const char *path) { for (int i = 0; i < st->n; i++) if (!strcmp(st->e[i].path, path)) return &st->e[i]; return NULL; }
static double ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0; }

int main(void) {
    char ver[64], why[300];
    EXPECT(gitc_find(NULL, GIT, sizeof GIT, ver, sizeof ver, why, sizeof why), "GIT-005: Git found without a shell (%s %s)", GIT, ver);
    EXPECT(!gitc_find("/nonexistent/git", GIT, sizeof GIT, ver, sizeof ver, why, sizeof why) || strcmp(GIT, "/nonexistent/git"), "GIT-005: an invalid configured path is not used");
    gitc_find(NULL, GIT, sizeof GIT, ver, sizeof ver, why, sizeof why);
    const char *env[16]; int ne = gitc_env_extra(env, 16); bool np = false, pg = false; for (int i = 0; i < ne; i++) { np |= !strcmp(env[i], "GIT_TERMINAL_PROMPT=0"); pg |= !strcmp(env[i], "GIT_PAGER=cat"); }
    EXPECT(np && pg, "GIT-030 / 063: Git never prompts on a terminal and never pages");

    /* runner */
    { const char *av[] = { "a; echo injected $(whoami)", NULL }; GitOpts o = { .timeout_ms = 5000 }; GitOut r;
      gitc_run("/bin/echo", NULL, av, &o, &r);
      EXPECT(r.exit_code == 0 && r.out && !strcmp(r.out, "a; echo injected $(whoami)\n"), "GIT-063: arguments are argv, never a shell (metacharacters stay literal)"); gitc_out_free(&r); }
    { const char *av[] = { "5", NULL }; GitOpts o = { .timeout_ms = 300 }; GitOut r; double t0 = ms();
      gitc_run("/bin/sleep", NULL, av, &o, &r);
      EXPECT(r.timed_out && r.exit_code == -1 && ms() - t0 < 2000, "GIT-031: a timeout kills the process (%.0f ms)", ms() - t0); gitc_out_free(&r); }
    { volatile int cancel = 1; const char *av[] = { "5", NULL }; GitOpts o = { .timeout_ms = 10000, .cancel = &cancel }; GitOut r; double t0 = ms();
      gitc_run("/bin/sleep", NULL, av, &o, &r);
      EXPECT(r.cancelled && ms() - t0 < 2000, "GIT-031: cancel stops it and says cancelled"); gitc_out_free(&r); }

    /* redaction */
    char red[400];
    gitc_redact("fatal: unable to access 'https://isa:ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ012345@github.com/o/r.git/'", red, sizeof red);
    EXPECT(!strstr(red, "ghp_") && !strstr(red, "isa:") && strstr(red, "https://***@github.com"), "GIT-065: user:token@ in URLs is masked (%s)", red);
    gitc_redact("Authorization: Bearer eyJhbGciOi.secret and glpat-abcdefghij1234567890 password=hunter2", red, sizeof red);
    EXPECT(!strstr(red, "eyJ") && !strstr(red, "glpat-abc") && !strstr(red, "hunter2"), "GIT-065 / 070: bearer tokens, GitLab tokens and password= are masked (%s)", red);

    /* repositories with a space and an accent in the path */
    snprintf(BASE, sizeof BASE, "%s/build/gitc", getcwd(why, sizeof why));
    char cmd[2400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", BASE); if (system(cmd)) {}
    mkdir("build", 0755); mkdir(BASE, 0755);
    char repo[1200]; snprintf(repo, sizeof repo, "%s/meu repó", BASE); mkdir(repo, 0755);
    EXPECT(gq(repo, "init", "-q", "-b", "main", NULL), "GIT-002: init with an initial branch name");
    gq(repo, "config", "user.name", "Lume Test", NULL); gq(repo, "config", "user.email", "test@example.invalid", NULL);
    char sub[1300]; snprintf(sub, sizeof sub, "%s/sql/views", repo); snprintf(cmd, sizeof cmd, "mkdir -p '%s'", sub); if (system(cmd)) {}
    wfile(sub, "receita líquida.sql", "CREATE VIEW v AS\nSELECT 1 AS a;\n");
    char root[1200], gd[1300], deep[1500]; snprintf(deep, sizeof deep, "%s/receita líquida.sql", sub);
    EXPECT(gitc_find_root(deep, root, sizeof root, gd, sizeof gd) && !strcmp(root, repo), "GIT-001: the repository is found from a file deep inside it (spaces and accents)");
    EXPECT(!gitc_find_root("/", root, sizeof root, gd, sizeof gd), "GIT-001: outside any repository: no repository (onboarding, not an error)");

    /* status of every kind */
    for (int i = 0; i < 5; i++) { char n[32], t[64]; snprintf(n, sizeof n, "f%d.txt", i); snprintf(t, sizeof t, "line 1\nline 2\nline 3\nfile %d\n", i); wfile(repo, n, t); }
    gq(repo, "add", "-A", NULL); gq(repo, "commit", "-q", "-m", "first", NULL);
    wfile(repo, "f0.txt", "line 1 changed\nline 2\nline 3\nfile 0\n");      /* modified, unstaged */
    wfile(repo, "f1.txt", "staged change\n"); gq(repo, "add", "f1.txt", NULL);   /* modified, staged */
    gq(repo, "rm", "-q", "f2.txt", NULL);                                         /* deleted, staged */
    gq(repo, "mv", "f3.txt", "f3 renamed.txt", NULL);                             /* renamed */
    wfile(repo, "novo arquivo.sql", "SELECT 2;\n");                               /* untracked */
    wfile(repo, ".gitignore", "*.log\n"); wfile(repo, "ignored.log", "x\n");
    GitStatus st;
    EXPECT(status(repo, &st), "GIT-009: status runs (no optional locks)");
    const GitEntry *m0 = entry(&st, "f0.txt"), *m1 = entry(&st, "f1.txt"), *d2 = entry(&st, "f2.txt"), *r3 = entry(&st, "f3 renamed.txt"), *u = entry(&st, "novo arquivo.sql");
    EXPECT(m0 && m0->x == '.' && m0->y == 'M' && gitc_entry_unstaged(m0) && !gitc_entry_staged(m0), "GIT-009 / 010: modified in the worktree only");
    EXPECT(m1 && m1->x == 'M' && gitc_entry_staged(m1), "GIT-010: staged change shown on the index side");
    EXPECT(d2 && d2->x == 'D', "GIT-009: deletion");
    EXPECT(r3 && r3->kind == GE_RENAME && r3->orig && !strcmp(r3->orig, "f3.txt"), "GIT-009 / 012: rename with its original path");
    EXPECT(u && u->kind == GE_UNTRACKED && !entry(&st, "ignored.log"), "GIT-009: untracked shown, .gitignore respected (ignored.log absent)");
    EXPECT(!strcmp(st.head, "main") && !st.detached && !st.has_upstream, "GIT-001: branch main, attached, no upstream");
    EXPECT(!strcmp(gitc_entry_label('M'), "modified") && !strcmp(gitc_entry_label('U'), "conflict"), "GIT-068: states have text labels, not colour only");
    gitc_status_free(&st);
    gq(repo, "add", "-A", NULL); gq(repo, "commit", "-q", "-m", "second", NULL);

    /* diff, side by side, staging one hunk (GIT-011 / 012) */
    { char big[4000] = ""; for (int i = 1; i <= 40; i++) { char l[32]; snprintf(l, sizeof l, "row %d\n", i); strcat(big, l); } wfile(repo, "big.txt", big); }
    gq(repo, "add", "big.txt", NULL); gq(repo, "commit", "-q", "-m", "big", NULL);
    { char big[4000] = ""; for (int i = 1; i <= 40; i++) { char l[32]; snprintf(l, sizeof l, i == 3 ? "row three\n" : i == 35 ? "row thirty-five\n" : "row %d\n", i); strcat(big, l); } wfile(repo, "big.txt", big); }
    GitOut o;
    g(repo, &o, "diff", "--no-color", "--no-ext-diff", "-U3", "--", "big.txt", NULL);
    GitDiff d; gitc_diff_parse(o.out, &d); gitc_out_free(&o);
    EXPECT(d.nf == 1 && d.f[0].nh == 2 && d.adds == 2 && d.dels == 2 && !strcmp(d.f[0].new_path, "big.txt"), "GIT-012: diff parsed (1 file, 2 hunks, +2 -2)");
    int rows[200][2]; int nr = gitc_side_by_side(&d, 0, rows, 200);
    bool paired = false; for (int i = 0; i < nr; i++) if (rows[i][0] >= 0 && rows[i][1] >= 0 && d.l[rows[i][0]].t == '-' && d.l[rows[i][1]].t == '+') paired = true;
    EXPECT(nr > 10 && paired, "GIT-012: side by side pairs the removed and added lines (%d rows)", nr);
    char *patch = gitc_hunk_patch(&d, 0, 0);
    { const char *av[] = { "apply", "--cached", "--whitespace=nowarn", "-", NULL }; GitOpts op = { .timeout_ms = 10000, .stdin_data = patch }; GitOut r; bool ok = gitc_run(GIT, repo, av, &op, &r);
      EXPECT(ok, "GIT-011: one hunk staged through git apply --cached (%s)", r.err); gitc_out_free(&r); }
    free(patch); gitc_diff_free(&d);
    status(repo, &st); const GitEntry *bg = entry(&st, "big.txt");
    EXPECT(bg && bg->x == 'M' && bg->y == 'M', "GIT-011: the file is now partly staged (index and worktree both changed)");
    gitc_status_free(&st);
    g(repo, &o, "diff", "--cached", "--no-color", "-U3", "--", "big.txt", NULL);
    EXPECT(o.out && strstr(o.out, "row three") && !strstr(o.out, "thirty-five"), "GIT-011: only that hunk is in the index"); gitc_out_free(&o);
    gq(repo, "add", "big.txt", NULL); gq(repo, "commit", "-q", "-m", "hunks", NULL);
    { const char bin[] = { 'P', 'K', 0, 1, 2, 3, 0, 9 }; char p[1300]; snprintf(p, sizeof p, "%s/blob.bin", repo); FILE *f = fopen(p, "wb"); fwrite(bin, 1, sizeof bin, f); fclose(f); }
    gq(repo, "add", "blob.bin", NULL);
    g(repo, &o, "diff", "--cached", "--no-color", "--", "blob.bin", NULL); gitc_diff_parse(o.out, &d); gitc_out_free(&o);
    EXPECT(d.nf == 1 && d.f[0].binary && d.f[0].is_new, "GIT-012: a binary file is recognised (summary, not text)");
    gitc_diff_free(&d);
    gq(repo, "commit", "-q", "-m", "binary", NULL);

    /* log, refs, branches, a bare remote */
    g(repo, &o, "log", "-n", "50", "--format=" GITC_LOG_FORMAT, NULL);
    GitCommit cm[50]; int nc = gitc_log_parse(o.out, cm, 50); gitc_out_free(&o);
    EXPECT(nc == 5 && !strcmp(cm[0].subject, "binary") && strlen(cm[0].hash) == 40 && strstr(cm[0].refs, "main"), "GIT-020: log parsed (5 commits, newest first, refs)");
    char bare[1200]; snprintf(bare, sizeof bare, "%s/origin.git", BASE);
    { const char *av[] = { "init", "-q", "--bare", bare, NULL }; GitOut r; GitOpts op = { 0 }; gitc_run(GIT, NULL, av, &op, &r); gitc_out_free(&r); }
    gq(repo, "remote", "add", "origin", bare, NULL);
    EXPECT(gq(repo, "push", "-q", "-u", "origin", "main", NULL), "GIT-029: push to a remote, upstream set");
    EXPECT(gq(repo, "switch", "-q", "-c", "feature/relatório", NULL), "GIT-018: create and switch to a branch (offline, local)");
    char bw[200];
    EXPECT(gitc_branch_name_ok("feature/relatório", bw, sizeof bw) && !gitc_branch_name_ok("bad name", bw, sizeof bw) && !gitc_branch_name_ok("a..b", bw, sizeof bw) && !gitc_branch_name_ok("-x", bw, sizeof bw) && !gitc_branch_name_ok("x.lock", bw, sizeof bw), "GIT-018: branch names follow Git's rules");
    g(repo, &o, "for-each-ref", "--format=" GITC_REF_FORMAT, "refs/heads", "refs/remotes", NULL);
    GitRef rf[20]; int nrf = gitc_refs_parse(o.out, rf, 20); gitc_out_free(&o);
    bool cur = false, rem = false; for (int i = 0; i < nrf; i++) { if (rf[i].head && !strcmp(rf[i].shortn, "feature/relatório")) cur = true; if (rf[i].remote && !strcmp(rf[i].shortn, "origin/main")) rem = true; }
    EXPECT(cur && rem, "GIT-018: local and remote branches listed, current marked (%d refs)", nrf);
    gq(repo, "switch", "-q", "main", NULL);
    gq(repo, "remote", "add", "corp", "https://isa:ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ012345@git.example.invalid/team/repo.git", NULL);
    g(repo, &o, "remote", "-v", NULL);
    GitRemote rm[8]; int nrm = gitc_remotes_parse(o.out, rm, 8); gitc_out_free(&o);
    bool masked = false; for (int i = 0; i < nrm; i++) if (!strcmp(rm[i].name, "corp")) masked = strstr(rm[i].fetch, "***@git.example.invalid") && !strstr(rm[i].fetch, "ghp_");
    EXPECT(nrm == 2 && masked, "GIT-026 / 065: remotes listed with fetch / push URLs, credentials masked");
    EXPECT(!strcmp(gitc_provider("git@github.com:o/r.git"), "GitHub") && !strcmp(gitc_provider("https://gitlab.corp/x.git"), "GitLab") && !strcmp(gitc_provider("https://dev.azure.com/o/p/_git/r"), "Azure DevOps") && !strcmp(gitc_provider("https://git.example.invalid/r.git"), "self-hosted"), "GIT-033 / 034: provider detected from the URL; unknown hosts are self-hosted, still plain Git");
    { const char *av[] = { "push", "corp", "main", NULL }; GitOpts op = { .timeout_ms = 20000 }; GitOut r; double t0 = ms();
      bool ok = gitc_run(GIT, repo, av, &op, &r);
      EXPECT(!ok && !r.timed_out && ms() - t0 < 15000 && !strstr(r.err, "ghp_ABCDEFG"), "GIT-030 / 065: a push that would need credentials fails fast, no prompt, token not in the error (%.0f ms: %.120s)", ms() - t0, r.err);
      gitc_out_free(&r); }

    /* conflicts and in-progress state (GIT-023 / 067) */
    gq(repo, "switch", "-q", "-c", "other", NULL); wfile(repo, "f0.txt", "other side\n"); gq(repo, "commit", "-q", "-am", "other", NULL);
    gq(repo, "switch", "-q", "main", NULL); wfile(repo, "f0.txt", "main side\n"); gq(repo, "commit", "-q", "-am", "main", NULL);
    { const char *av[] = { "merge", "--no-edit", "other", NULL }; GitOpts op = { .timeout_ms = 20000 }; GitOut r; bool ok = gitc_run(GIT, repo, av, &op, &r); EXPECT(!ok, "GIT-023: a conflicting merge stops (Git says so, nothing forced)"); gitc_out_free(&r); }
    gitc_find_root(repo, root, sizeof root, gd, sizeof gd);
    status(repo, &st);
    EXPECT(gitc_repo_state(gd) == GS_MERGING && st.conflicts == 1 && entry(&st, "f0.txt")->kind == GE_UNMERGED, "GIT-067: merge in progress read from the repository, 1 conflict");
    gitc_status_free(&st);
    gq(repo, "merge", "--abort", NULL);
    EXPECT(gitc_repo_state(gd) == GS_NONE, "GIT-067: abort leaves no merge in progress");

    /* worktree: its .git is a file (GIT-025 / 001) */
    char wt[1200]; snprintf(wt, sizeof wt, "%s/wt dir", BASE);
    gq(repo, "worktree", "add", "-q", wt, "other", NULL);
    char wtf[1300]; snprintf(wtf, sizeof wtf, "%s/f0.txt", wt);
    EXPECT(gitc_find_root(wtf, root, sizeof root, gd, sizeof gd) && !strcmp(root, wt) && strstr(gd, "worktrees"), "GIT-001 / 025: a worktree is found through its .git file (gitdir %s)", gd);

    /* messages and AI policy */
    char w2[200];
    EXPECT(!gitc_message_ok("   ", why, sizeof why, w2, sizeof w2) && gitc_message_ok("feat: x", why, sizeof why, w2, sizeof w2) && !w2[0], "GIT-014 / 015: an empty message is refused; a short one passes");
    gitc_message_ok("feat: a very long summary line that goes on and on far beyond what reads well in logs", why, sizeof why, w2, sizeof w2);
    EXPECT(w2[0] != 0, "GIT-015: a long summary is a warning, not a block");
    EXPECT(gitc_path_sensitive("config/.env") && gitc_path_sensitive("keys/server.pem") && gitc_path_sensitive("prefs/secrets.txt") && gitc_path_sensitive("api_token.json") && !gitc_path_sensitive("sql/views/revenue.sql"), "GIT-062: secrets, keys and Lume's credential files are never sent to a model");

    /* 1,000 changed files (GIT-069 / 071) */
    char many[1300]; snprintf(many, sizeof many, "%s/many", repo); mkdir(many, 0755);
    for (int i = 0; i < 1000; i++) { char n[32]; snprintf(n, sizeof n, "file %04d.sql", i); wfile(many, n, "SELECT 1;\n"); }
    double t0 = ms(); status(repo, &st); double el = ms() - t0;
    EXPECT(st.untracked >= 1000, "GIT-069 / 071: 1,000 new files: status in %.0f ms (%d entries)", el, st.n);
    gitc_status_free(&st);

    printf("\nGITCORE: %s (%d ok, %d failures)\n", fails ? "FAILED" : "passed", oks, fails);
    return fails ? 1 : 0;
}
