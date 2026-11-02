/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Small JSON parser for the value viewer (US-020). Builds a flat node table over the source
 * text (no copies of strings): each node points at its slice. Strict RFC 8259 grammar, so a
 * broken cell reports the offset of the first unexpected character.
 *
 * Ownership: the Json table borrows `src`; the caller must keep the source text alive (and
 * unchanged) for as long as the nodes are used. Only the node array is heap-allocated, and
 * json_free() releases it. Not thread-safe per Json instance; separate instances are
 * independent. Node 0 is always the root value after a successful parse. */

/* Node kinds (JsonNode.type). */
enum { JN_OBJECT, JN_ARRAY, JN_STRING, JN_NUMBER, JN_TRUE, JN_FALSE, JN_NULL };

/* One parsed value. Children of a container are linked through first_child / next_sibling
 * in source order; nodes are stored in pre-order (a parent always precedes its children). */
typedef struct JsonNode {
    uint8_t type;                  /* JN_* kind */
    int depth;                     /* nesting level, 0 = root */
    uint32_t key_off, key_len;     /* object member name (raw, between the quotes); key_len 0 = none */
    uint32_t val_off, val_len;     /* scalar text (strings: raw, between the quotes); containers: the whole {...} / [...] span */
    int first_child, next_sibling; /* -1 = none */
    int nchildren;                 /* direct children of an object/array */
} JsonNode;

/* A parse result: the borrowed source plus the flat node table. */
typedef struct Json {
    const char *src; size_t len;   /* borrowed source text (not owned, not NUL-terminated necessarily) */
    JsonNode *nodes; int n, cap;   /* heap node array: n used, cap allocated */
    char err[96]; size_t err_off;  /* error message and byte offset of the failure (valid after a failed parse) */
} Json;

/* Hard cap on nodes per document, so a huge cell cannot exhaust memory. */
#define JSON_MAX_NODES 400000

/* Parses len bytes of src into j (j is reset first). Returns true on success; on false,
 * j->err / err_off describe the first problem. Call json_free() in both cases. */
bool json_parse(Json *j, const char *src, size_t len);   /* false: j->err / err_off set */
/* Frees the node array and zeroes j. Safe on a zeroed or already-freed Json. */
void json_free(Json *j);
/* Decodes a raw string slice (escapes resolved) into buf; truncates with "…" when needed.
 * raw/len is a slice such as src + key_off / val_off; buf has n bytes and is always
 * NUL-terminated (n must be > 0). \uXXXX escapes, including surrogate pairs, become UTF-8. */
void json_unescape(const char *raw, size_t len, char *buf, size_t n);
