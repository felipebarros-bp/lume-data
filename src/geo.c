/* SPDX-License-Identifier: AGPL-3.0-only */
/* geo.c - geometry recognition for the cell value viewer (US-041 / US-059) and small binary
 * helpers for blob cells (US-021).
 *
 * geo_parse() tries two forms:
 *   - hex WKB / EWKB, as PostGIS returns geometry columns: decoded to bytes, then walked by
 *     a small recursive reader (wkb_*) that honours per-geometry byte order, the EWKB
 *     Z/M/SRID flag bits and the ISO 1000/2000/3000 type offsets;
 *   - WKT / EWKT text ("SRID=4326;POINT(1 2)"), parsed by wkt_parse() on a NUL-terminated copy.
 * Both fill a Geo (see geo.h): a flat vertex array, part start indices (one per ring /
 * linestring / point group) and a running bounding box. Only x/y are kept; Z and M values
 * are consumed and dropped. Coordinates are never reprojected.
 *
 * Limits: at most GEO_MAX_POINTS vertices are stored (the rest are still parsed and the Geo
 * is flagged truncated), WKB input above 4 MiB is refused, and nesting is capped, so a
 * huge or hostile cell cannot stall the UI thread.
 *
 * Ownership: Geo arrays are heap-allocated and freed by geo_free(). No global state; all
 * functions are reentrant. Callers are the value viewer and the map preview. */
#include "geo.h"
/* Maximum vertices stored per geometry (see Geo.truncated). */
#define GEO_MAX_POINTS 200000
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* See geo.h. Static name table indexed by GEO_* kind. */
const char *geo_type_name(int type) {
    static const char *const names[] = { "none", "POINT", "LINESTRING", "POLYGON", "MULTIPOINT", "MULTILINESTRING", "MULTIPOLYGON" };
    return type >= 0 && type <= GEO_MULTIPOLYGON ? names[type] : "?";
}

/* See geo.h: releases the arrays and zeroes the struct. */
void geo_free(Geo *g) { free(g->pts); free(g->part_start); memset(g, 0, sizeof *g); }

/* Appends one vertex (growing pts in blocks of 256) and widens the bounding box. Past
 * GEO_MAX_POINTS the vertex is dropped and the geometry marked truncated. */
static void add_pt(Geo *g, double x, double y) {
    if (g->npts >= GEO_MAX_POINTS) { g->truncated = true; return; }   /* huge geometries never stall the UI (US-059) */
    if ((g->npts & 255) == 0) g->pts = realloc(g->pts, sizeof *g->pts * (g->npts + 256));
    g->pts[g->npts].x = x; g->pts[g->npts].y = y; g->npts++;
    if (g->npts == 1) { g->minx = g->maxx = x; g->miny = g->maxy = y; }
    if (x < g->minx) g->minx = x; if (x > g->maxx) g->maxx = x;
    if (y < g->miny) g->miny = y; if (y > g->maxy) g->maxy = y;
}
/* Starts a new part (ring, linestring or point group) at the current vertex index; part
 * starts grow in blocks of 16. */
static void add_part(Geo *g) {
    if ((g->nparts & 15) == 0) g->part_start = realloc(g->part_start, sizeof *g->part_start * (g->nparts + 16));
    g->part_start[g->nparts++] = g->npts;
}

/* ---- WKT ------------------------------------------------------------------ */

/* Skips spaces, tabs and line breaks in WKT text. */
static void skip_ws(const char **p) { while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++; }

/* reads "x y [z] [m]" coordinate tuples until ')' */
/* `dims` is the number of ordinates per tuple (2-4); only x and y are stored. On success
 * *pp is left just past the closing ')'. Each tuple is appended with add_pt(). */
static bool wkt_coords(const char **pp, Geo *g, int dims) {
    const char *p = *pp;
    for (;;) {
        skip_ws(&p);
        char *end;
        double x = strtod(p, &end);
        if (end == p) return false;
        p = end; skip_ws(&p);
        double y = strtod(p, &end);
        if (end == p) return false;
        p = end;
        for (int d = 2; d < dims; d++) { skip_ws(&p); strtod(p, &end); if (end == p) return false; p = end; }
        add_pt(g, x, y);
        skip_ws(&p);
        if (*p == ',') { p++; continue; }
        if (*p == ')') { p++; break; }
        return false;
    }
    *pp = p;
    return true;
}

/* Parses NUL-terminated WKT/EWKT into g: optional "SRID=n;" prefix, the type keyword,
 * optional Z / M / ZM, then EMPTY or the parenthesised body. Returns true only if the
 * whole string was consumed. "MULTIPOINT(1 2, 3 4)" and "MULTIPOINT((1 2),(3 4))" are
 * both accepted. Longer keywords are tried first so POINT does not shadow MULTIPOINT. */
static bool wkt_parse(const char *s, Geo *g) {
    const char *p = s;
    skip_ws(&p);
    /* EWKT prefix "SRID=<digits>;": the SRID must be a positive integer, nothing else before ';' */
    if (strncasecmp(p, "SRID=", 5) == 0) {
        p += 5;
        if (*p < '1' || *p > '9') return false;
        long srid = 0;
        while (*p >= '0' && *p <= '9') { srid = srid * 10 + (*p - '0'); if (srid > 999999) return false; p++; }
        if (*p != ';') return false;
        g->srid = (int)srid;
        p++; skip_ws(&p);
    }
    static const struct { const char *w; int t; } kinds[] = {
        { "MULTIPOLYGON", GEO_MULTIPOLYGON }, { "MULTILINESTRING", GEO_MULTILINESTRING }, { "MULTIPOINT", GEO_MULTIPOINT },
        { "POLYGON", GEO_POLYGON }, { "LINESTRING", GEO_LINESTRING }, { "POINT", GEO_POINT },
    };
    int type = GEO_NONE;
    for (int i = 0; i < 6 && !type; i++) if (strncasecmp(p, kinds[i].w, strlen(kinds[i].w)) == 0) { type = kinds[i].t; p += strlen(kinds[i].w); }
    if (!type) return false;
    g->type = type;
    /* Dimension suffix: Z, M or ZM decides how many ordinates each tuple carries. */
    skip_ws(&p);
    int dims = 2;
    if (toupper((unsigned char)*p) == 'Z' && !isalpha((unsigned char)p[1])) { g->has_z = true; dims++; p++; skip_ws(&p); }
    if (toupper((unsigned char)*p) == 'M' && !isalpha((unsigned char)p[1])) { g->has_m = true; dims++; p++; skip_ws(&p); }
    if (strncasecmp(p, "ZM", 2) == 0) { g->has_z = g->has_m = true; dims = 4; p += 2; skip_ws(&p); }
    if (strncasecmp(p, "EMPTY", 5) == 0) return true;
    if (*p != '(') return false;
    p++;
    /* Body, by geometry kind: one coordinate list, a point list, a list of lists, or a list of
     * polygons (list of lists of lists). */
    if (type == GEO_POINT || type == GEO_LINESTRING) { add_part(g); if (!wkt_coords(&p, g, dims)) return false; }
    else if (type == GEO_MULTIPOINT) {
        add_part(g);
        for (;;) {
            skip_ws(&p);
            bool paren = *p == '(';
            if (paren) p++;
            skip_ws(&p);
            char *end;
            double x = strtod(p, &end); if (end == p) return false; p = end; skip_ws(&p);
            double y = strtod(p, &end); if (end == p) return false; p = end;
            for (int d = 2; d < dims; d++) { skip_ws(&p); strtod(p, &end); if (end == p) return false; p = end; }
            add_pt(g, x, y);
            skip_ws(&p);
            if (paren) { if (*p != ')') return false; p++; skip_ws(&p); }
            if (*p == ',') { p++; continue; }
            if (*p == ')') { p++; break; }
            return false;
        }
    } else if (type == GEO_POLYGON || type == GEO_MULTILINESTRING) {
        for (;;) {
            skip_ws(&p);
            if (*p != '(') return false;
            p++;
            add_part(g);
            if (!wkt_coords(&p, g, dims)) return false;
            skip_ws(&p);
            if (*p == ',') { p++; continue; }
            if (*p == ')') { p++; break; }
            return false;
        }
    } else {   /* MULTIPOLYGON: ((ring),(ring)),((ring)) */
        for (;;) {
            skip_ws(&p);
            if (*p != '(') return false;
            p++;
            for (;;) {
                skip_ws(&p);
                if (*p != '(') return false;
                p++;
                add_part(g);
                if (!wkt_coords(&p, g, dims)) return false;
                skip_ws(&p);
                if (*p == ',') { p++; continue; }
                if (*p == ')') { p++; break; }
                return false;
            }
            skip_ws(&p);
            if (*p == ',') { p++; continue; }
            if (*p == ')') { p++; break; }
            return false;
        }
    }
    skip_ws(&p);
    return *p == 0;
}

/* ---- WKB / EWKB (hex) ------------------------------------------------------ */

/* WKB reader cursor: current byte, end of buffer, and the byte order of the geometry
 * currently being read (each nested geometry carries its own order byte). */
typedef struct { const unsigned char *p, *end; bool little; } Wkb;

/* Reads a uint32 in the current byte order; false if fewer than 4 bytes remain. */
static bool rd_u32(Wkb *w, uint32_t *v) {
    if (w->end - w->p < 4) return false;
    *v = w->little ? (uint32_t)w->p[0] | (uint32_t)w->p[1] << 8 | (uint32_t)w->p[2] << 16 | (uint32_t)w->p[3] << 24
                   : (uint32_t)w->p[3] | (uint32_t)w->p[2] << 8 | (uint32_t)w->p[1] << 16 | (uint32_t)w->p[0] << 24;
    w->p += 4;
    return true;
}
/* Reads an IEEE-754 double in the current byte order; false if fewer than 8 bytes remain. */
static bool rd_f64(Wkb *w, double *v) {
    if (w->end - w->p < 8) return false;
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u |= (uint64_t)w->p[w->little ? i : 7 - i] << (8 * i);
    memcpy(v, &u, 8);
    w->p += 8;
    return true;
}

/* Reads n points of `dims` doubles each, storing only x/y. False on short input. */
static bool wkb_points(Wkb *w, Geo *g, int dims, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        double x, y, z;
        if (!rd_f64(w, &x) || !rd_f64(w, &y)) return false;
        for (int d = 2; d < dims; d++) if (!rd_f64(w, &z)) return false;
        add_pt(g, x, y);
    }
    return true;
}

/* Forward declaration: multi-geometries recurse into wkb_geom for each member. */
static bool wkb_geom(Wkb *w, Geo *g, int depth);

/* Reads a geometry header: byte-order byte, type word with EWKB flag bits (Z 0x80000000,
 * M 0x40000000, SRID 0x20000000) or ISO offsets (+1000 Z, +2000 M, +3000 ZM), and the
 * optional SRID. Returns the base type (1..7) and the ordinate count; sets has_z/has_m/srid. */
static bool wkb_header(Wkb *w, uint32_t *type, int *dims, Geo *g) {
    if (w->p >= w->end) return false;
    w->little = *w->p++ == 1;
    uint32_t t;
    if (!rd_u32(w, &t)) return false;
    *dims = 2;
    if (t & 0x80000000u) { g->has_z = true; (*dims)++; }
    if (t & 0x40000000u) { g->has_m = true; (*dims)++; }
    bool srid = (t & 0x20000000u) != 0;
    t &= 0x0FFFFFFFu;
    if (t >= 1000 && t < 2000) { t -= 1000; g->has_z = true; (*dims)++; }        /* ISO Z */
    else if (t >= 2000 && t < 3000) { t -= 2000; g->has_m = true; (*dims)++; }
    else if (t >= 3000 && t < 4000) { t -= 3000; g->has_z = g->has_m = true; *dims += 2; }
    if (srid) { uint32_t s; if (!rd_u32(w, &s)) return false; g->srid = (int)s; }
    *type = t;
    return true;
}

/* Reads one geometry (and, for multi-types / collections, its members up to depth 4) into
 * g, adding a part per point / linestring / ring. Counts are bounded to reject garbage. */
static bool wkb_geom(Wkb *w, Geo *g, int depth) {
    uint32_t type; int dims;
    if (depth > 4 || !wkb_header(w, &type, &dims, g)) return false;
    uint32_t n;
    switch (type) {
    case 1: add_part(g); return wkb_points(w, g, dims, 1);
    case 2: add_part(g); return rd_u32(w, &n) && n < 10000000 && wkb_points(w, g, dims, n);
    case 3:
        if (!rd_u32(w, &n) || n > 100000) return false;
        for (uint32_t r = 0; r < n; r++) { uint32_t m; add_part(g); if (!rd_u32(w, &m) || m > 10000000 || !wkb_points(w, g, dims, m)) return false; }
        return true;
    case 4: case 5: case 6: case 7:
        if (!rd_u32(w, &n) || n > 100000) return false;
        for (uint32_t i = 0; i < n; i++) { Geo sub = { 0 }; if (!wkb_geom(w, g, depth + 1)) return false; (void)sub; }
        return true;
    default: return false;
    }
}

/* See geo.h. Hex input is tried as WKB first (even length, starts with a 00/01 byte-order
 * byte, only hex digits); anything else is parsed as WKT. */
bool geo_parse(const char *s, size_t len, Geo *g) {
    memset(g, 0, sizeof *g);
    if (!s || len < 5) return false;
    /* hex WKB: only hex digits, even length, starts with 00 or 01 */
    bool hex = len >= 10 && (len % 2) == 0 && (s[0] == '0') && (s[1] == '0' || s[1] == '1');
    for (size_t i = 0; hex && i < len; i++) if (!isxdigit((unsigned char)s[i])) hex = false;
    if (hex) {
        /* WKB path: decode hex, peek the header to learn the top-level type, then read it all and
         * require that every byte was consumed and at least one point was found. */
        size_t n = len / 2;
        if (n > 4u << 20) return false;
        unsigned char *bytes = malloc(n);
        if (hex_decode(s, len, bytes, n) < 0) { free(bytes); return false; }
        Wkb w = { bytes, bytes + n, true };
        uint32_t type; int dims;
        Wkb peek = w;
        Geo tmp = { 0 };
        bool ok = wkb_header(&peek, &type, &dims, &tmp) && type >= 1 && type <= 7;
        if (ok) {
            g->type = type == 7 ? GEO_NONE : (int)type;
            ok = wkb_geom(&w, g, 0) && w.p == w.end;
        }
        free(bytes);
        if (!ok || g->npts == 0) { geo_free(g); return false; }
        g->from_wkb = true;
        return true;
    }
    /* WKT path: parse a NUL-terminated copy of the slice. */
    char *copy = malloc(len + 1);
    memcpy(copy, s, len); copy[len] = 0;
    bool ok = wkt_parse(copy, g);
    free(copy);
    if (!ok) { geo_free(g); return false; }
    return true;
}

/* ---- binary helpers ---------------------------------------------------------- */

/* Value of one hex digit, or -1 if c is not a hex digit. */
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* See geo.h: optional "\x" prefix, then pairs of hex digits. */
long hex_decode(const char *s, size_t len, unsigned char *out, size_t cap) {
    size_t i = 0;
    if (len >= 2 && s[0] == '\\' && (s[1] == 'x' || s[1] == 'X')) i = 2;
    long n = 0;
    for (; i + 1 < len; i += 2) {
        int a = hexval(s[i]), b = hexval(s[i + 1]);
        if (a < 0 || b < 0) return -1;
        if ((size_t)n < cap) out[n] = (unsigned char)(a * 16 + b);
        n++;
    }
    if (i != len) return -1;
    return n;
}

/* See geo.h. */
bool looks_like_hex_blob(const char *s) {
    if (!s || s[0] != '\\' || (s[1] != 'x' && s[1] != 'X')) return false;
    size_t n = strlen(s);
    if (n < 4 || (n % 2) != 0) return false;
    for (size_t i = 2; i < n; i++) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

/* See geo.h. Encodes 3 input bytes into 4 output characters per step. */
void base64_encode(const unsigned char *p, size_t n, char *out, size_t cap) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n && o + 5 < cap; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[o++] = tbl[(v >> 18) & 63]; out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    out[o] = 0;
}

/* See geo.h. Builds e.g. "POINT Z  ·  SRID 4326  ·  1 part, 1 point  ·  EWKB  ·  axes: ...".
 * The axes hint depends on the SRID: 4326 means lon/lat, other SRIDs mean CRS units. */
void geo_describe(const Geo *g, char *out, size_t cap) {
    char srid[32];
    if (g->srid) snprintf(srid, sizeof srid, "SRID %d", g->srid); else snprintf(srid, sizeof srid, "SRID not stated");
    const char *axes = g->srid == 4326 ? "x = lon, y = lat (EPSG:4326)" : g->srid ? "x / y in CRS units, not lat/lon" : "x / y as stored, CRS unknown";
    snprintf(out, cap, "%s%s%s  \xC2\xB7  %s  \xC2\xB7  %d part%s, %d point%s%s  \xC2\xB7  %s  \xC2\xB7  axes: %s, no reprojection",
             geo_type_name(g->type), g->has_z ? " Z" : "", g->has_m ? " M" : "", srid, g->nparts, g->nparts == 1 ? "" : "s",
             g->npts, g->npts == 1 ? "" : "s", g->truncated ? " (truncated at 200,000)" : "", g->from_wkb ? "EWKB" : "WKT", axes);
}
