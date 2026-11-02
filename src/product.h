/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Product identity and build information (About, Copy Info, log; spec "About Lume Data")
 * ======================================================================================
 *
 * LumeProduct: what a product of the Lume family is (name, tagline, description, icon, links,
 * license, copyright). One instance per product; the About component takes it as data, so
 * another Lume IDE reuses the dialog by giving its own (ABOUT-032). Links / license / copyright
 * are NULL until they are really published or confirmed: the About then says "not configured"
 * and never opens an example address (ABOUT-014..021).
 *
 * LumeBuildInfo: what this binary and process really are: version (CMake project VERSION, the
 * single source), build id (git or the build system; "Development build" otherwise), channel,
 * platform and process architecture, the engine stack, and the database drivers that are
 * available here (their client library loads), with versions. Nothing is a sample value; what
 * cannot be known is "Not available" (ABOUT-007..013, 024).
 *
 * Copy Info is built from an allowlist of these fields only: no connection, credential, SQL,
 * path, host name, user name or machine identifier can reach it (ABOUT-022 / 023).
 *
 * User-facing strings live in one table (product_text) so a translation replaces the table,
 * not scattered literals (ABOUT-004 / 029). English only today.
 */
#include <stdbool.h>
#include <stddef.h>

typedef struct LumeProduct {
    const char *name;            /* "Lume Data" */
    const char *family;          /* "Lume" */
    const char *tagline, *description;
    const char *icon_asset;      /* file prefix under assets/brand/, "-<px>.png" appended */
    const char *website_url, *docs_url, *issues_url, *release_notes_url;   /* NULL = not configured */
    const char *license_name;    /* NULL = not yet published */
    const char *copyright;       /* NULL = rights holder not confirmed */
} LumeProduct;

typedef struct LumeDriverInfo { char name[32]; char version[64]; bool available; } LumeDriverInfo;

typedef struct LumeBuildInfo {
    char version[32];        /* "0.1.0" or "Not available" */
    char build[64];          /* "a1b2c3d" or "Development build" */
    char channel[32];        /* Development / Early Preview / Beta / Stable */
    char platform[32];       /* SDL_GetPlatform(): "macOS" */
    char arch[16];           /* the process: "arm64", "x86_64" */
    char engine[32];         /* "C · SDL3" */
    char sdl[32], sqlite[32];
    char configured[40];     /* when the build was configured (UTC), "" unknown */
    char compiler[96];
    LumeDriverInfo drivers[8]; int ndrivers;
    char drivers_text[200];  /* available ones only: "SQLite · PostgreSQL" */
} LumeBuildInfo;

const LumeProduct *product_current(void);
/* Fills info. probe_drivers: ask each driver whether its client library loads (may dlopen;
 * never connects; call when the About opens, not every frame). */
void product_build_info(LumeBuildInfo *info, bool probe_drivers);
/* Copy Info text (allowlist): product, version, build, channel, platform, engine, drivers. */
int product_copy_info(const LumeProduct *p, const LumeBuildInfo *b, char *out, int n);
/* "0.1.0 (build a1b2c3d)" / "0.1.0 (Development build)" (ABOUT-026). */
void product_version_line(const LumeBuildInfo *b, char *out, int n);

/* Links: only https URLs with a host are allowed (no javascript:, file:, data:, shell). */
bool url_allowed(const char *url, char *why, int n);
/* Opens an allowed URL with the platform's browser (SDL_OpenURL). false + why otherwise. */
bool url_open(const char *url, char *why, int n);

/* User-facing strings of the About (one table: translations replace it). */
enum { PT_ABOUT_TITLE, PT_APP_INFO, PT_VERSION, PT_BUILD, PT_CHANNEL, PT_PLATFORM, PT_ENGINE, PT_DRIVERS, PT_WEBSITE, PT_DOCS,
       PT_ISSUE, PT_LICENSES, PT_RELEASE_NOTES, PT_COPY_INFO, PT_COPY_VERSION, PT_CLOSE, PT_DETAILS, PT_NOT_CONFIGURED,
       PT_LICENSE_UNPUBLISHED, PT_NOT_AVAILABLE, PT_DEV_BUILD, PT_COPIED, PT_THIRD_PARTY, PT_BACK, PT_LUME_LICENSE, PT_COUNT };
const char *product_text(int id);
