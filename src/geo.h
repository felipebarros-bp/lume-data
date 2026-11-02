/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Geometry recognition for the value viewer (US-041): WKT / EWKT ("SRID=4326;POINT(1 2)")
 * and hex WKB / EWKB as PostGIS returns it. Coordinates are kept exactly as stored; no CRS
 * transformation ever happens here. Also small helpers for binary cells (US-021). */

/* Geometry kinds (Geo.type). Values 1..6 match the OGC WKB type codes. A WKB
 * GEOMETRYCOLLECTION (code 7) is accepted but reported as GEO_NONE. */
enum { GEO_NONE, GEO_POINT, GEO_LINESTRING, GEO_POLYGON, GEO_MULTIPOINT, GEO_MULTILINESTRING, GEO_MULTIPOLYGON };

/* One 2-D vertex in the geometry's own coordinate units (Z/M values are read but dropped). */
typedef struct GeoPt { double x, y; } GeoPt;
/* A parsed geometry: flat vertex list plus part boundaries and a bounding box. Heap arrays
 * are owned by the Geo and released by geo_free(). */
typedef struct Geo {
    int type;              /* GEO_* kind */
    int srid;              /* 0 = not stated */
    bool has_z, has_m;     /* source had Z and/or M ordinates (not kept in pts) */
    GeoPt *pts; int npts;  /* all vertices, in source order */
    int *part_start; int nparts;   /* rings / parts as index ranges into pts */
    bool from_wkb;         /* parsed from hex (E)WKB rather than (E)WKT text */
    bool truncated;        /* more than 200,000 points: the rest was not read */
    double minx, miny, maxx, maxy;   /* bounding box of pts (valid when npts > 0) */
} Geo;

/* Recognises WKT/EWKT text or hex WKB/EWKB in s[0..len) (s need not be NUL-terminated) and
 * fills g (always reset first). Returns false when s is not a geometry; g is then empty and
 * needs no free. On true, the caller must call geo_free(g). */
bool geo_parse(const char *s, size_t len, Geo *g);   /* false: not a geometry */
/* Frees the point and part arrays and zeroes g. Safe on a zeroed Geo. */
void geo_free(Geo *g);
/* Display name of a GEO_* kind ("POINT", ...); "none" for GEO_NONE, "?" if out of range.
 * Returns a static string. */
const char *geo_type_name(int type);
/* One-line caption for the viewer (US-059): type, SRID (or "not stated"), parts and points,
 * truncation, source form and what the axes mean; always says there is no reprojection. */
void geo_describe(const Geo *g, char *out, size_t cap);

/* "\x0102" hex text (PostgreSQL bytea / Lume blob form) -> bytes; returns the byte count or -1.
 * The "\x" prefix is optional. Writes at most cap bytes but returns the full decoded length,
 * so a result > cap means out was too small. -1 on a non-hex digit or odd digit count. */
long hex_decode(const char *s, size_t len, unsigned char *out, size_t cap);
/* True when s is a "\x" hex literal with an even total length of at least 4 and only hex
 * digits after the prefix (i.e. a bytea/blob cell rather than ordinary text). */
bool looks_like_hex_blob(const char *s);
/* base64 of n bytes into out (needs 4*ceil(n/3)+1); standard alphabet with '=' padding.
 * Stops early (still NUL-terminated) if cap is too small; cap must be > 0. */
void base64_encode(const unsigned char *p, size_t n, char *out, size_t cap);
