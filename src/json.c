/* SPDX-License-Identifier: AGPL-3.0-only */
/* json.c - strict RFC 8259 JSON parser used by the cell value viewer (US-020).
 *
 * A recursive-descent parser that never copies text: it fills a flat JsonNode table (see
 * json.h) whose entries hold offsets/lengths into the caller's source buffer. The viewer
 * walks the table to render a collapsible tree, and calls json_unescape() to turn a raw
 * string slice into displayable UTF-8.
 *
 * Limits: JSON_MAX_NODES nodes and 512 levels of nesting, so a pathological cell fails with
 * a message instead of exhausting memory or the C stack. The first error wins and records
 * its byte offset in Json.err_off.
 *
 * Ownership/threading: the source text is borrowed, the node array is owned by the Json and
 * released by json_free(). No global state; one Json per thread at a time. */
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parser state: target table, cursor and end of input, and a sticky failure flag. */
typedef struct { Json *j; const char *p, *end; bool fail; } P;

/* Appends a zeroed node of the given type/depth (links set to -1) and returns its index,
 * growing the array by doubling. Returns -1 and fails the parse past JSON_MAX_NODES.
 * Callers must re-index j->nodes after this call because the array may move. */
static int add_node(P *s, int type, int depth) {
    Json *j = s->j;
    if (j->n >= JSON_MAX_NODES) { s->fail = true; snprintf(j->err, sizeof j->err, "too many nodes (limit %d)", JSON_MAX_NODES); return -1; }
    if (j->n == j->cap) { j->cap = j->cap ? j->cap * 2 : 64; j->nodes = realloc(j->nodes, sizeof *j->nodes * j->cap); }
    JsonNode *nd = &j->nodes[j->n];
    memset(nd, 0, sizeof *nd);
    nd->type = (uint8_t)type; nd->depth = depth; nd->first_child = nd->next_sibling = -1;
    return j->n++;
}

/* Records the first parse error ("msg at offset N") at the current cursor; later errors are
 * ignored so the report points at the original problem. */
static void fail_at(P *s, const char *msg) {
    if (s->fail) return;
    s->fail = true;
    s->j->err_off = (size_t)(s->p - s->j->src);
    snprintf(s->j->err, sizeof s->j->err, "%s at offset %zu", msg, s->j->err_off);
}

/* Skips the four JSON whitespace characters. */
static void skip_ws(P *s) { while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++; }

/* Validates a quoted string at the cursor and returns its raw body (between the quotes,
 * escapes untouched) as offset/length into src. Rejects control characters and bad escapes. */
static bool parse_string(P *s, uint32_t *off, uint32_t *len) {
    if (s->p >= s->end || *s->p != '"') { fail_at(s, "expected string"); return false; }
    s->p++;
    const char *start = s->p;
    while (s->p < s->end && *s->p != '"') {
        if ((unsigned char)*s->p < 0x20) { fail_at(s, "control character in string"); return false; }
        if (*s->p == '\\') {
            s->p++;
            if (s->p >= s->end) break;
            if (strchr("\"\\/bfnrt", *s->p)) s->p++;
            else if (*s->p == 'u') {
                s->p++;
                for (int i = 0; i < 4; i++, s->p++) {
                    if (s->p >= s->end || !strchr("0123456789abcdefABCDEF", *s->p)) { fail_at(s, "bad \\u escape"); return false; }
                }
            } else { fail_at(s, "bad escape"); return false; }
        } else s->p++;
    }
    if (s->p >= s->end) { fail_at(s, "unterminated string"); return false; }
    *off = (uint32_t)(start - s->j->src); *len = (uint32_t)(s->p - start);
    s->p++;
    return true;
}

/* Forward declaration: values and containers are mutually recursive. */
static int parse_value(P *s, int depth);

/* Parses an object (object=true) or array starting at '{' / '['. Children are linked in
 * order via first_child / next_sibling; object members get key_off/key_len set. The node's
 * val_off/val_len span the whole bracketed text. Returns the node index or -1. */
static int parse_container(P *s, int depth, bool object) {
    int me = add_node(s, object ? JN_OBJECT : JN_ARRAY, depth);
    if (me < 0) return -1;
    s->j->nodes[me].val_off = (uint32_t)(s->p - s->j->src);
    s->p++;                                              /* { or [ */
    skip_ws(s);
    int last = -1, count = 0;
    /* Empty container: close immediately. */
    if (s->p < s->end && *s->p == (object ? '}' : ']')) { s->p++; s->j->nodes[me].val_len = (uint32_t)(s->p - s->j->src) - s->j->nodes[me].val_off; return me; }
    /* One member/element per iteration: optional "key": then a value, then ',' or the closer. */
    for (;;) {
        skip_ws(s);
        uint32_t koff = 0, klen = 0;
        if (object) {
            if (!parse_string(s, &koff, &klen)) return -1;
            skip_ws(s);
            if (s->p >= s->end || *s->p != ':') { fail_at(s, "expected ':'"); return -1; }
            s->p++;
            skip_ws(s);
        }
        int child = parse_value(s, depth + 1);
        if (child < 0) return -1;
        s->j->nodes[child].key_off = koff; s->j->nodes[child].key_len = klen;
        if (last < 0) s->j->nodes[me].first_child = child; else s->j->nodes[last].next_sibling = child;
        last = child; count++;
        skip_ws(s);
        if (s->p < s->end && *s->p == ',') { s->p++; continue; }
        if (s->p < s->end && *s->p == (object ? '}' : ']')) { s->p++; break; }
        fail_at(s, object ? "expected ',' or '}'" : "expected ',' or ']'");
        return -1;
    }
    s->j->nodes[me].nchildren = count;
    s->j->nodes[me].val_len = (uint32_t)(s->p - s->j->src) - s->j->nodes[me].val_off;
    return me;
}

/* Parses any value at the cursor (after whitespace) at the given nesting depth and returns
 * its node index, or -1 on error. Dispatches to containers, strings, numbers and literals. */
static int parse_value(P *s, int depth) {
    skip_ws(s);
    if (s->p >= s->end) { fail_at(s, "unexpected end"); return -1; }
    if (depth > 512) { fail_at(s, "nesting too deep"); return -1; }
    char c = *s->p;
    if (c == '{') return parse_container(s, depth, true);
    if (c == '[') return parse_container(s, depth, false);
    if (c == '"') {
        uint32_t off, len;
        if (!parse_string(s, &off, &len)) return -1;
        int me = add_node(s, JN_STRING, depth);
        if (me < 0) return -1;
        s->j->nodes[me].val_off = off; s->j->nodes[me].val_len = len;
        return me;
    }
    /* Number: -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)? per the RFC grammar. */
    const char *start = s->p;
    if (c == '-' || (c >= '0' && c <= '9')) {
        if (c == '-') s->p++;
        if (s->p < s->end && *s->p == '0') s->p++;
        else if (s->p < s->end && *s->p >= '1' && *s->p <= '9') { while (s->p < s->end && *s->p >= '0' && *s->p <= '9') s->p++; }
        else { fail_at(s, "bad number"); return -1; }
        if (s->p < s->end && *s->p == '.') { s->p++; if (s->p >= s->end || *s->p < '0' || *s->p > '9') { fail_at(s, "bad number"); return -1; } while (s->p < s->end && *s->p >= '0' && *s->p <= '9') s->p++; }
        if (s->p < s->end && (*s->p == 'e' || *s->p == 'E')) {
            s->p++;
            if (s->p < s->end && (*s->p == '+' || *s->p == '-')) s->p++;
            if (s->p >= s->end || *s->p < '0' || *s->p > '9') { fail_at(s, "bad number"); return -1; }
            while (s->p < s->end && *s->p >= '0' && *s->p <= '9') s->p++;
        }
        int me = add_node(s, JN_NUMBER, depth);
        if (me < 0) return -1;
        s->j->nodes[me].val_off = (uint32_t)(start - s->j->src); s->j->nodes[me].val_len = (uint32_t)(s->p - start);
        return me;
    }
    /* Literals true / false / null. */
    static const struct { const char *word; int type; } lits[] = { { "true", JN_TRUE }, { "false", JN_FALSE }, { "null", JN_NULL } };
    for (int i = 0; i < 3; i++) {
        size_t wl = strlen(lits[i].word);
        if ((size_t)(s->end - s->p) >= wl && strncmp(s->p, lits[i].word, wl) == 0) {
            int me = add_node(s, lits[i].type, depth);
            if (me < 0) return -1;
            s->j->nodes[me].val_off = (uint32_t)(s->p - s->j->src); s->j->nodes[me].val_len = (uint32_t)wl;
            s->p += wl;
            return me;
        }
    }
    fail_at(s, "unexpected character");
    return -1;
}

/* Public entry: parses the whole buffer, requiring exactly one value plus optional
 * whitespace. On failure j->err always holds a message. See json.h. */
bool json_parse(Json *j, const char *src, size_t len) {
    memset(j, 0, sizeof *j);
    j->src = src; j->len = len;
    P s = { j, src, src + len, false };
    int root = parse_value(&s, 0);
    if (root >= 0 && !s.fail) {
        skip_ws(&s);
        if (s.p != s.end) fail_at(&s, "trailing characters");
    }
    if (s.fail) { if (!j->err[0]) snprintf(j->err, sizeof j->err, "invalid JSON"); return false; }
    return true;
}

/* Releases the node table and zeroes the struct. */
void json_free(Json *j) { free(j->nodes); memset(j, 0, sizeof *j); }

/* Encodes code point cp as UTF-8 into buf at *p, advancing *p; drops the character when it
 * would not fit (keeps room for the terminator). */
static void put_utf8(char *buf, size_t n, size_t *p, uint32_t cp) {
    char tmp[4]; int k = 0;
    if (cp < 0x80) tmp[k++] = (char)cp;
    else if (cp < 0x800) { tmp[k++] = (char)(0xC0 | (cp >> 6)); tmp[k++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { tmp[k++] = (char)(0xE0 | (cp >> 12)); tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[k++] = (char)(0x80 | (cp & 0x3F)); }
    else { tmp[k++] = (char)(0xF0 | (cp >> 18)); tmp[k++] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[k++] = (char)(0x80 | (cp & 0x3F)); }
    if (*p + k < n) { memcpy(buf + *p, tmp, k); *p += k; }
}

/* Resolves JSON escapes in a raw string slice into buf (always NUL-terminated). Stops with
 * an ellipsis when fewer than ~8 bytes remain; unknown escapes keep the escaped character. */
void json_unescape(const char *raw, size_t len, char *buf, size_t n) {
    size_t p = 0;
    bool cut = false;
    for (size_t i = 0; i < len; i++) {
        if (p + 8 >= n) { cut = true; break; }
        if (raw[i] != '\\' || i + 1 >= len) { buf[p++] = raw[i]; continue; }
        i++;
        switch (raw[i]) {
        case 'n': buf[p++] = '\n'; break;
        case 't': buf[p++] = '\t'; break;
        case 'r': buf[p++] = '\r'; break;
        case 'b': buf[p++] = '\b'; break;
        case 'f': buf[p++] = '\f'; break;
        /* \uXXXX, combining a following low surrogate into one code point when present. */
        case 'u': {
            if (i + 4 < len) {
                char hex[5] = { raw[i + 1], raw[i + 2], raw[i + 3], raw[i + 4], 0 };
                uint32_t cp = (uint32_t)strtoul(hex, NULL, 16);
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < len && raw[i + 1] == '\\' && raw[i + 2] == 'u') {
                    char hex2[5] = { raw[i + 3], raw[i + 4], raw[i + 5], raw[i + 6], 0 };
                    uint32_t lo = (uint32_t)strtoul(hex2, NULL, 16);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                }
                put_utf8(buf, n, &p, cp);
            }
            break;
        }
        default: buf[p++] = raw[i]; break;
        }
    }
    if (cut && p + 4 < n) { memcpy(buf + p, "\xE2\x80\xA6", 3); p += 3; }
    buf[p] = 0;
}
