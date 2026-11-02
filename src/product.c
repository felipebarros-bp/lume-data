/* SPDX-License-Identifier: AGPL-3.0-only */
/* Product identity, build information, Copy Info and the link opener: see product.h. */
#include "product.h"
#include "conn.h"
#include "db.h"
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* build metadata from CMake; a build without them shows "Not available" (ABOUT-024) */
#ifndef LUME_VERSION
#define LUME_VERSION ""
#endif
#ifndef LUME_CHANNEL
#define LUME_CHANNEL ""
#endif
#ifndef LUME_BUILD_ID
#define LUME_BUILD_ID ""
#endif
#ifndef LUME_CONFIGURED_UTC
#define LUME_CONFIGURED_UTC ""
#endif
#ifndef LUME_WEBSITE_URL
#define LUME_WEBSITE_URL ""
#endif
#ifndef LUME_DOCS_URL
#define LUME_DOCS_URL ""
#endif
#ifndef LUME_ISSUES_URL
#define LUME_ISSUES_URL ""
#endif
#ifndef LUME_RELEASE_NOTES_URL
#define LUME_RELEASE_NOTES_URL ""
#endif
#ifndef LUME_LICENSE_NAME
#define LUME_LICENSE_NAME ""
#endif
#ifndef LUME_COPYRIGHT
#define LUME_COPYRIGHT ""
#endif

static const char *or_null(const char *s) { return s && s[0] ? s : NULL; }

const LumeProduct *product_current(void) {
    static LumeProduct p;
    static bool init;
    if (!init) {
        init = true;
        p.name = "Lume Data";
        p.family = "Lume";
        p.tagline = "Powerful tools. Minimal overhead.";
        p.description = "A fast, lightweight, native database IDE for developers.";
        p.icon_asset = "lume-data-symbol-transparent";
        p.website_url = or_null(LUME_WEBSITE_URL);
        p.docs_url = or_null(LUME_DOCS_URL);
        p.issues_url = or_null(LUME_ISSUES_URL);
        p.release_notes_url = or_null(LUME_RELEASE_NOTES_URL);
        p.license_name = or_null(LUME_LICENSE_NAME);
        p.copyright = or_null(LUME_COPYRIGHT);
    }
    return &p;
}

static const char *const text_en[PT_COUNT] = {
    [PT_ABOUT_TITLE] = "About Lume Data", [PT_APP_INFO] = "APPLICATION INFORMATION", [PT_VERSION] = "Version", [PT_BUILD] = "Build",
    [PT_CHANNEL] = "Channel", [PT_PLATFORM] = "Platform", [PT_ENGINE] = "Engine", [PT_DRIVERS] = "Database drivers",
    [PT_WEBSITE] = "Website", [PT_DOCS] = "Documentation", [PT_ISSUE] = "Report an Issue", [PT_LICENSES] = "Licenses",
    [PT_RELEASE_NOTES] = "Release Notes", [PT_COPY_INFO] = "Copy Info", [PT_COPY_VERSION] = "Copy Version", [PT_CLOSE] = "Close",
    [PT_DETAILS] = "Technical Details", [PT_NOT_CONFIGURED] = "Not configured for this build yet",
    [PT_LICENSE_UNPUBLISHED] = "License information not yet published", [PT_NOT_AVAILABLE] = "Not available",
    [PT_DEV_BUILD] = "Development build", [PT_COPIED] = "Copied to the clipboard", [PT_THIRD_PARTY] = "Third-party Licenses",
    [PT_BACK] = "Back", [PT_LUME_LICENSE] = "Lume Data license",
};
const char *product_text(int id) { return id >= 0 && id < PT_COUNT && text_en[id] ? text_en[id] : ""; }

void product_build_info(LumeBuildInfo *b, bool probe) {
    memset(b, 0, sizeof *b);
    const char *na = product_text(PT_NOT_AVAILABLE);
    snprintf(b->version, sizeof b->version, "%s", LUME_VERSION[0] ? LUME_VERSION : na);
    snprintf(b->build, sizeof b->build, "%s", LUME_BUILD_ID[0] ? LUME_BUILD_ID : product_text(PT_DEV_BUILD));
    /* the channel comes from the build; an empty one is not assumed Stable (ABOUT-009) */
    snprintf(b->channel, sizeof b->channel, "%s", LUME_CHANNEL[0] ? LUME_CHANNEL : na);
    const char *plat = SDL_GetPlatform();
    snprintf(b->platform, sizeof b->platform, "%s", plat && plat[0] ? plat : na);
#if defined(__aarch64__) || defined(_M_ARM64)
    snprintf(b->arch, sizeof b->arch, "arm64");
#elif defined(__x86_64__) || defined(_M_X64)
    snprintf(b->arch, sizeof b->arch, "x86_64");
#elif defined(__i386__) || defined(_M_IX86)
    snprintf(b->arch, sizeof b->arch, "x86");
#else
    snprintf(b->arch, sizeof b->arch, "%s", na);
#endif
    snprintf(b->engine, sizeof b->engine, "C \xC2\xB7 SDL3");
    int v = SDL_GetVersion();
    snprintf(b->sdl, sizeof b->sdl, "%d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    snprintf(b->configured, sizeof b->configured, "%s", LUME_CONFIGURED_UTC);
#if defined(__clang__)
    snprintf(b->compiler, sizeof b->compiler, "Clang %d.%d.%d", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    snprintf(b->compiler, sizeof b->compiler, "GCC %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    snprintf(b->compiler, sizeof b->compiler, "MSVC %d", _MSC_VER);
#else
    snprintf(b->compiler, sizeof b->compiler, "%s", na);
#endif
    /* drivers from the registry, available ones first (ABOUT-012) */
    int nd = 0; const DriverInfo *di = driver_infos(&nd);
    int o = 0;
    /* a probe loads the client library (Oracle Instant Client: about a second of CPU); the result
     * cannot change while the process runs (a loaded library stays loaded), so it is kept */
    static struct { bool done, available; char version[64]; } cache[8];
    for (int i = 0; i < nd && b->ndrivers < 8; i++) {
        LumeDriverInfo *d = &b->drivers[b->ndrivers++];
        snprintf(d->name, sizeof d->name, "%s", di[i].name);
        if (!di[i].drv) { d->available = false; snprintf(d->version, sizeof d->version, "no driver in this build"); continue; }
        if (di[i].drv->probe && probe && i < 8 && cache[i].done) { d->available = cache[i].available; snprintf(d->version, sizeof d->version, "%s", cache[i].version); }
        else if (di[i].drv->probe && probe) {
            d->available = di[i].drv->probe(d->version, sizeof d->version);
            if (i < 8) { cache[i].done = true; cache[i].available = d->available; snprintf(cache[i].version, sizeof cache[i].version, "%s", d->version); }
        }
        else if (di[i].drv->probe) { d->available = !strcmp(di[i].id, "sqlite"); if (d->available) di[i].drv->probe(d->version, sizeof d->version); else snprintf(d->version, sizeof d->version, "not checked"); }
        else d->available = true;
        if (!strcmp(di[i].id, "sqlite")) snprintf(b->sqlite, sizeof b->sqlite, "%s", d->version);
        if (d->available) o += snprintf(b->drivers_text + o, sizeof b->drivers_text - (size_t)o, "%s%s", o ? " \xC2\xB7 " : "", d->name);
    }
    if (!b->drivers_text[0]) snprintf(b->drivers_text, sizeof b->drivers_text, "%s", na);
}

void product_version_line(const LumeBuildInfo *b, char *out, int n) {
    if (!strcmp(b->build, product_text(PT_DEV_BUILD))) snprintf(out, (size_t)n, "%s (%s)", b->version, b->build);
    else snprintf(out, (size_t)n, "%s (build %s)", b->version, b->build);
}

int product_copy_info(const LumeProduct *p, const LumeBuildInfo *b, char *out, int n) {
    /* allowlist: these fields and nothing else (spec 5.3) */
    char drivers[200]; snprintf(drivers, sizeof drivers, "%s", b->drivers_text);
    for (char *s = drivers; (s = strstr(s, " \xC2\xB7 ")); ) { s[0] = ','; memmove(s + 1, s + 3, strlen(s + 3) + 1); s[1] = ' '; }
    return snprintf(out, (size_t)n, "%s\nVersion: %s\nBuild: %s\nChannel: %s\nPlatform: %s (%s)\nEngine: C / SDL3\nDatabase drivers: %s\n",
                    p->name, b->version, b->build, b->channel, b->platform, b->arch, drivers);
}

bool url_allowed(const char *url, char *why, int n) {
    if (!url || !url[0]) { snprintf(why, (size_t)n, "%s", product_text(PT_NOT_CONFIGURED)); return false; }
    if (strncasecmp(url, "https://", 8)) { snprintf(why, (size_t)n, "Only https links are opened"); return false; }
    const char *h = url + 8;
    if (!*h || *h == '/' || *h == '@' || *h == ':') { snprintf(why, (size_t)n, "The link has no host"); return false; }
    for (const char *s = url; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= 0x20 || c == 0x7f || c == '"' || c == '\'' || c == '`' || c == '\\' || c == '<' || c == '>' || c == '|' || c == ';') {
            snprintf(why, (size_t)n, "The link holds characters that are not allowed"); return false;
        }
    }
    why[0] = 0;
    return true;
}

bool url_open(const char *url, char *why, int n) {
    if (!url_allowed(url, why, n)) return false;
    /* the platform's own opener: no shell, no command line; it returns once the request is
     * handed to the system (the browser starts on its own) */
    if (!SDL_OpenURL(url)) { snprintf(why, (size_t)n, "The browser could not be opened: %s", SDL_GetError()); return false; }
    return true;
}
