/* SPDX-License-Identifier: AGPL-3.0-only */
/* Headless checks for the sandboxed extractors (US-045): approval, no shell, secrets
 * unreadable, network denied, timeout kills, Lume stays responsive, audit log. Uses a
 * throwaway app-data folder; nothing touches the real one. */
#include "extract.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <strings.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>

static int failures = 0;
static void check(bool ok, const char *what, const char *detail) {
    printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) failures++;
}
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    char *s = calloc(1, 65536); size_t n = fread(s, 1, 65535, f); s[n] = 0; fclose(f); return s;
}
static void write_file(const char *path, const char *text) { FILE *f = fopen(path, "wb"); if (f) { fputs(text, f); fclose(f); } }
static int pumps;
static bool pump(void *ud) { (void)ud; pumps++; return true; }
static bool pump_cancel(void *ud) { (void)ud; pumps++; return pumps < 10; }
static char detail[200];
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

int main(int argc, char **argv) {
    const char *base = argc > 1 ? argv[1] : "/tmp/lume_extract_check";
    char dir[600], secret[700], list[700], out[700], cmd[2000];
    snprintf(dir, sizeof dir, "%s/", base);
    mkdir(base, 0700);
    snprintf(secret, sizeof secret, "%ssecret.env", dir);
    write_file(secret, "DB_PASSWORD=hunter2-not-for-extractors\n");
    snprintf(out, sizeof out, "%sresult.txt", dir);
    snprintf(list, sizeof list, "%sextractors.txt", dir);
    const char *py = access("/usr/bin/python3", X_OK) == 0 ? "/usr/bin/python3" : access("/opt/homebrew/bin/python3", X_OK) == 0 ? "/opt/homebrew/bin/python3" : NULL;
    /* a TCP listener on the host's loopback: a sandboxed program must not be able to reach it
     * (macOS: the sandbox denies the socket; Linux: the program has its own network namespace) */
    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    socklen_t sl = sizeof sa;
    int port = (lsock >= 0 && bind(lsock, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(lsock, 4) == 0 && getsockname(lsock, (struct sockaddr *)&sa, &sl) == 0) ? ntohs(sa.sin_port) : 9;
    char netcmd[400];
    snprintf(netcmd, sizeof netcmd, "net\t/usr/bin/env python3 -c \"import socket,sys; socket.create_connection(('127.0.0.1', %d), 2); open(sys.argv[1],'w').write('reached')\" {out}", port);
    snprintf(cmd, sizeof cmd,
             "# comment line\n"
             "copy\t/bin/cp {file} {out}\n"
             "leak\t/bin/cp %s {out}\n"
             "shell\t/bin/sh -c \"echo first > {out}; cat %s >> {out}\"\n"
             "hang\t/bin/sleep 30\ttimeout=1\n"
             "relative\tcp {file} {out}\n"
             "%s\n",
             secret, secret,
             py ? netcmd : "net\t/bin/false");
    /* the net entry must name an absolute program: use the interpreter directly */
    if (py) { char *p = strstr(cmd, "/usr/bin/env python3"); if (p) { char rest[2000]; snprintf(rest, sizeof rest, "%s", p + strlen("/usr/bin/env python3")); snprintf(p, sizeof cmd - (p - cmd), "%s%s", py, rest); } }
    write_file(list, cmd);
    char trust[700]; snprintf(trust, sizeof trust, "%sextractors-trust.txt", dir); remove(trust);
    char logp[700]; snprintf(logp, sizeof logp, "%sextractors-log.txt", dir); remove(logp);

    check(extract_sandbox_available(), "a sandbox is available on this system", NULL);
    Extractor ex[EXTRACT_MAX];
    int n = extract_load(dir, ex, EXTRACT_MAX);
    check(n == 6, "six entries loaded, the comment skipped", NULL);
    check(!ex[0].trusted && !ex[1].trusted, "entries start unapproved", NULL);
    check(ex[3].timeout_secs == 1 && ex[0].timeout_secs == EXTRACT_DEFAULT_SECS, "per-entry timeout parsed, default otherwise", NULL);
    char desc[600]; extract_describe(&ex[0], desc, sizeof desc);
    check(strstr(desc, "no network") && strstr(desc, "no shell"), "description states the permissions", NULL);

    ExtractRun run; ExtractResult res; char err[200];
    /* 1. unapproved: refused before anything runs */
    extract_begin(dir, &run, err, sizeof err);
    write_file(run.infile, "id\tname\n1\tAna\n");
    extract_run(&run, &ex[0], out, NULL, pump, NULL, &res);
    check(res.status == EXTRACT_DENIED && access(out, F_OK) != 0, "unapproved extractor does not run", res.message);
    extract_end(&run);

    /* 2. approved copy works; output moved to the destination; scratch removed */
    check(extract_trust(dir, &ex[0], true) && ex[0].trusted, "approval recorded", NULL);
    extract_begin(dir, &run, err, sizeof err);
    write_file(run.infile, "id\tname\n1\tAna\n");
    extract_run(&run, &ex[0], out, NULL, pump, NULL, &res);
    char *got = slurp(out);
    check(res.status == EXTRACT_OK && got && strcmp(got, "id\tname\n1\tAna\n") == 0, "approved copy extractor writes the destination", res.message);
    free(got);
    extract_log(dir, &ex[0], &res, 1, out);
    char scratch[1100]; snprintf(scratch, sizeof scratch, "%s", run.scratch);
    extract_end(&run);
    check(access(scratch, F_OK) != 0, "scratch folder removed after the run", NULL);
    remove(out);

    /* 3. editing the command invalidates the approval */
    char *txt = slurp(list);
    char *p = strstr(txt, "/bin/cp {file} {out}");
    if (p) memcpy(p, "/bin/cp {file} {out} ", 20);
    char edited[4000]; snprintf(edited, sizeof edited, "%s", txt); free(txt);
    char *q = strstr(edited, "copy\t/bin/cp"); if (q) { memmove(q + strlen("copy\t/bin/cp -p"), q + strlen("copy\t/bin/cp"), strlen(q + strlen("copy\t/bin/cp")) + 1); memcpy(q, "copy\t/bin/cp -p", strlen("copy\t/bin/cp -p")); }
    write_file(list, edited);
    n = extract_load(dir, ex, EXTRACT_MAX);
    check(n == 6 && !ex[0].trusted && strstr(ex[0].cmd, "-p") != NULL, "edited command needs a new approval", ex[0].cmd);

    /* 4. secrets are unreadable even for an approved extractor */
    extract_trust(dir, &ex[1], true);
    extract_begin(dir, &run, err, sizeof err);
    write_file(run.infile, "x\n");
    extract_run(&run, &ex[1], out, NULL, pump, NULL, &res);
    got = slurp(out);
    check(res.status != EXTRACT_OK && (!got || !strstr(got, "hunter2")), "a file outside the sandbox cannot be read (cp of a .env fails)", res.message);
    free(got); extract_end(&run); remove(out);

    /* 5. a shell named explicitly still cannot reach the secret; no shell is implied otherwise */
    extract_trust(dir, &ex[2], true);
    extract_begin(dir, &run, err, sizeof err);
    write_file(run.infile, "x\n");
    extract_run(&run, &ex[2], out, NULL, pump, NULL, &res);
    got = slurp(out);
    check(res.status == EXTRACT_FAILED && !got && (strstr(res.message, "not permitted") || strstr(res.message, "No such file")), "sh -c runs sandboxed: the secret read is denied and the partial output is not delivered", res.message);
    free(got); extract_end(&run); remove(out);

    /* 6. timeout kills the process and Lume kept pumping */
    extract_trust(dir, &ex[3], true);
    extract_begin(dir, &run, err, sizeof err);
    write_file(run.infile, "x\n");
    pumps = 0; double t0 = now_s();
    extract_run(&run, &ex[3], out, NULL, pump, NULL, &res);
    double el = now_s() - t0;
    snprintf(detail, sizeof detail, "%s after %.1f s, %d pumps", res.message, el, pumps);
    check(res.status == EXTRACT_TIMEOUT && el < 4 && pumps > 10, "sleep 30 with timeout=1 is killed within seconds while the UI pump keeps running", detail);
    extract_end(&run);

    /* 7. cancel from the pump */
    extract_begin(dir, &run, err, sizeof err);
    write_file(run.infile, "x\n");
    pumps = 0;
    extract_run(&run, &ex[3], out, NULL, pump_cancel, NULL, &res);
    check(res.status == EXTRACT_CANCELLED, "cancel from the pump kills the process", res.message);
    extract_end(&run);

    /* 8. relative program names are refused (no PATH lookup) */
    extract_trust(dir, &ex[4], true);
    extract_begin(dir, &run, err, sizeof err);
    write_file(run.infile, "x\n");
    extract_run(&run, &ex[4], out, NULL, pump, NULL, &res);
    check(res.status == EXTRACT_BAD_COMMAND, "relative program name refused", res.message);
    extract_end(&run);

    /* 9. network denied */
    if (py) {
        extract_trust(dir, &ex[5], true);
        extract_begin(dir, &run, err, sizeof err);
        write_file(run.infile, "x\n");
        extract_run(&run, &ex[5], out, NULL, pump, NULL, &res);
        got = slurp(out);
        /* the proof is on the server side: no connection may have arrived */
        fcntl(lsock, F_SETFL, O_NONBLOCK);
        int conn = accept(lsock, NULL, NULL);
        if (conn >= 0) close(conn);
        check(res.status == EXTRACT_FAILED && !got && conn < 0 && strstr(res.message, "Traceback") && !strstr(res.message, "xcode"),
              "python runs but cannot reach a TCP server on the host's loopback: nothing arrived at the server (no network)", res.message);
        free(got); extract_end(&run); remove(out);
    } else printf("skip  network check (no python3)\n");

    /* 11. CPU limit (RLIMIT_CPU) kills a spinning program before the wall-clock limit */
    if (py) {
        FILE *lf = fopen(list, "a"); fprintf(lf, "spin\t%s -c \"while True: pass\"\ttimeout=30\n", py); fclose(lf);
        n = extract_load(dir, ex, EXTRACT_MAX);
        Extractor *spin = NULL; for (int i = 0; i < n; i++) if (strcmp(ex[i].name, "spin") == 0) spin = &ex[i];
        extract_trust(dir, spin, true);
        ExtractLimits lim = extract_default_limits(spin); lim.cpu_secs = 1;
        extract_begin(dir, &run, err, sizeof err);
        write_file(run.infile, "x\n");
        t0 = now_s();
        extract_run(&run, spin, out, &lim, pump, NULL, &res);
        el = now_s() - t0;
        snprintf(detail, sizeof detail, "%s after %.1f s", res.message, el);
        check(res.status == EXTRACT_FAILED && strstr(res.message, "CPU limit") && el < 5, "a CPU-bound program is killed by the CPU limit (SIGXCPU) well before the 30 s wall clock", detail);
        extract_end(&run);
        /* memory limit: RLIMIT_AS is enforced by Linux, not by macOS; the result is reported as it is */
        FILE *lf2 = fopen(list, "a"); fprintf(lf2, "hog\t%s -c \"import time; b = b'x' * (700 * 1024 * 1024); time.sleep(3); open('{out}', 'w').write(str(len(b)))\"\ttimeout=30\n", py); fclose(lf2);
        n = extract_load(dir, ex, EXTRACT_MAX);
        Extractor *hog = NULL; for (int i = 0; i < n; i++) if (strcmp(ex[i].name, "hog") == 0) hog = &ex[i];
        if (hog) {
            extract_trust(dir, hog, true);
            ExtractLimits lm = extract_default_limits(hog); lm.mem_mb = 256;
            extract_begin(dir, &run, err, sizeof err);
            write_file(run.infile, "x\n");
            extract_run(&run, hog, out, &lm, pump, NULL, &res);
            /* 700 MB of written bytes (not lazy zero pages) under a 256 MB limit: the kernel (Linux)
             * or Lume's sampler (macOS) must stop it; the program would otherwise finish OK */
            check(res.status == EXTRACT_FAILED && strcasestr(res.message, "memory") && access(out, F_OK) != 0, "memory limit: a 700 MB program under a 256 MB limit is killed and writes nothing", res.message);
            extract_end(&run); remove(out);
        }
    }

    /* 10. audit log: status words, no data, no secret */
    extract_log(dir, &ex[1], &res, 1, out);
    char *log = slurp(logp);
    check(log && strstr(log, "name=copy") && strstr(log, "status=ok") && !strstr(log, "Ana") && !strstr(log, "hunter2"), "audit log records name/status/exit/secs, never the rows", NULL);
    free(log);

    printf("%s (%d failures)\n", failures ? "EXTRACT: FAIL" : "EXTRACT: OK", failures);
    return failures ? 1 : 0;
}
