/* SPDX-License-Identifier: AGPL-3.0-only */
/* About Lume Data, headless (ABOUT-007..024, 031): build metadata, honest "Not available",
 * drivers from the registry (an absent one is not listed), Copy Info allowlist with secrets in the
 * environment, version line, URL validation. Built twice: with the build's metadata and with none. */
#include "conn.h"
#include "db.h"
#include "product.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, oks;
#define CHECK(c, ...) do { if (c) { oks++; printf("ok    "); } else { fails++; printf("FAIL  "); } printf(__VA_ARGS__); printf("\n"); } while (0)

/* a registry with SQLite (loads), PostgreSQL (client library missing) and MySQL (no driver) */
static bool probe_yes(char *v, size_t n) { snprintf(v, n, "3.99.0"); return true; }
static bool probe_no(char *v, size_t n) { snprintf(v, n, "client library not found"); return false; }
static DbDriver d_sqlite = { .name = "SQLite", .probe = probe_yes }, d_pg = { .name = "PostgreSQL", .probe = probe_no };
static DriverInfo reg[] = { { .id = "sqlite", .name = "SQLite", .drv = &d_sqlite }, { .id = "postgres", .name = "PostgreSQL", .drv = &d_pg }, { .id = "mysql", .name = "MySQL", .drv = NULL } };
const DriverInfo *driver_infos(int *count) { *count = 3; return reg; }

int main(void) {
    /* secrets a careless implementation could pick up */
    setenv("LUME_PG_CONN", "host=db.internal.example user=alice password=hunter2", 1);
    setenv("PGPASSWORD", "hunter2", 1);
    LumeBuildInfo b; product_build_info(&b, true);
    const LumeProduct *p = product_current();
#ifdef WITH_METADATA
    CHECK(!strcmp(b.version, "0.1.0") && !strcmp(b.channel, "Early Preview"), "ABOUT-007 / 009: version and channel come from the build (%s, %s)", b.version, b.channel);
    CHECK(!strcmp(b.build, "Development build"), "ABOUT-008: no build id -> \"Development build\", never a fake hash");
    CHECK(p->website_url && !strcmp(p->website_url, "https://docs.lume.test/start"), "ABOUT-014: a configured website comes from the build");
    char why[200]; CHECK(url_allowed(p->website_url, why, sizeof why), "ABOUT-018: the configured https link is allowed");
    CHECK(!p->docs_url && !url_allowed(p->docs_url, why, sizeof why) && strstr(why, "Not configured"), "ABOUT-015: an unconfigured link is refused with the reason (%s)", why);
#else
    CHECK(!strcmp(b.version, "Not available") && !strcmp(b.channel, "Not available"), "ABOUT-024: no metadata -> Not available (version %s, channel %s), never assumed Stable", b.version, b.channel);
    CHECK(!p->website_url && !p->docs_url && !p->issues_url && !p->release_notes_url && !p->license_name && !p->copyright, "ABOUT-014 / 019 / 021: no URL, license or copyright invented");
#endif
    CHECK(!strcmp(b.drivers_text, "SQLite"), "ABOUT-012 / scenario 7: only drivers whose library loads are listed (%s)", b.drivers_text);
    CHECK(b.ndrivers == 3 && !b.drivers[1].available && !b.drivers[2].available, "ABOUT-012: missing client library and missing driver are told apart");
    CHECK(b.platform[0] && b.arch[0] && strcmp(b.arch, "Not available"), "ABOUT-010: platform and process architecture detected (%s, %s)", b.platform, b.arch);
    CHECK(!strcmp(b.engine, "C \xC2\xB7 SDL3"), "ABOUT-011: engine label C · SDL3 (SQLite is a driver, not the UI engine)");
    char t[1024]; product_copy_info(p, &b, t, sizeof t);
    int lines = 0; for (char *s = t; *s; s++) lines += *s == '\n';
    CHECK(lines == 7 && !strncmp(t, "Lume Data\nVersion: ", 19) && strstr(t, "Database drivers: SQLite\n"), "ABOUT-022: Copy Info = the 7 allowlisted lines");
    const char *home = getenv("HOME"), *user = getenv("USER");
    bool leak = strstr(t, "hunter2") || strstr(t, "alice") || strstr(t, "db.internal") || strstr(t, "password") || strstr(t, "host=") ||
                (home && home[0] && strstr(t, home)) || (user && user[0] && strstr(t, user));
    CHECK(!leak, "ABOUT-023 / scenario 3: no password, user, host, home path or user name in Copy Info");
    char v[160]; product_version_line(&b, v, sizeof v);
    CHECK(strstr(v, b.version) && strstr(v, "Development build"), "ABOUT-026: version line \"%s\" matches what is shown", v);
    char why2[200];
    const char *bad[] = { "javascript:alert(1)", "file:///etc/passwd", "http://lume.test", "https://", "https:///x", "https://a b", "https://x;rm -rf ~", "data:text/html,x", "HTTPS://ok.test\"", NULL };
    bool all_refused = true; for (int i = 0; bad[i]; i++) if (url_allowed(bad[i], why2, sizeof why2)) { all_refused = false; printf("      allowed: %s\n", bad[i]); }
    CHECK(all_refused, "ABOUT-018: javascript:, file:, data:, http:, empty host, spaces and shell characters refused");
    CHECK(url_allowed("https://lume.test/docs?v=0.1.0#about", why2, sizeof why2), "ABOUT-018: a plain https link with path / query passes");
    bool texts = true; for (int i = 0; i < PT_COUNT; i++) if (!product_text(i)[0]) texts = false;
    CHECK(texts, "ABOUT-004 / 029: every About string comes from one table (no empty entry)");
    printf("%s: %d ok, %d failed\n", fails ? "ABOUT CHECK FAILED" : "ABOUT CHECK OK", oks, fails);
    return fails != 0;
}
