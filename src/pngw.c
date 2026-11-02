/* SPDX-License-Identifier: AGPL-3.0-only */
/* PNG writer: see pngw.h. */
#include "pngw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Out { unsigned char *b; size_t w, cap; bool oom; uint32_t bits; int nbits; } Out;
static void put(Out *o, const void *p, size_t n) {
    if (o->oom) return;
    if (o->w + n > o->cap) { size_t c = o->cap ? o->cap * 2 : 65536; while (c < o->w + n) c *= 2; unsigned char *nb = realloc(o->b, c); if (!nb) { o->oom = true; return; } o->b = nb; o->cap = c; }
    memcpy(o->b + o->w, p, n); o->w += n;
}
static void byte(Out *o, unsigned v) { unsigned char c = (unsigned char)v; put(o, &c, 1); }
static void be32(Out *o, uint32_t v) { unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v }; put(o, b, 4); }
/* LSB-first bit writer (deflate) */
static void bits(Out *o, uint32_t v, int n) { o->bits |= v << o->nbits; o->nbits += n; while (o->nbits >= 8) { byte(o, o->bits & 0xff); o->bits >>= 8; o->nbits -= 8; } }
static void flush_bits(Out *o) { if (o->nbits) { byte(o, o->bits & 0xff); o->bits = 0; o->nbits = 0; } }
static uint32_t rev(uint32_t v, int n) { uint32_t r = 0; for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
static void huff(Out *o, uint32_t code, int len) { bits(o, rev(code, len), len); }
static void lit(Out *o, int c) {
    if (c < 144) huff(o, 0x30 + (uint32_t)c, 8);
    else if (c < 256) huff(o, 0x190 + (uint32_t)(c - 144), 9);
    else if (c < 280) huff(o, (uint32_t)(c - 256), 7);
    else huff(o, 0xC0 + (uint32_t)(c - 280), 8);
}
static const int LBASE[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const int LEXT[] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const int DBASE[] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const int DEXT[] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static void match(Out *o, int len, int dist) {
    int i = 28; while (LBASE[i] > len) i--;
    lit(o, 257 + i); if (LEXT[i]) bits(o, (uint32_t)(len - LBASE[i]), LEXT[i]);
    int d = 29; while (DBASE[d] > dist) d--;
    huff(o, (uint32_t)d, 5); if (DEXT[d]) bits(o, (uint32_t)(dist - DBASE[d]), DEXT[d]);
}
#define HBITS 15
static void deflate_fixed(Out *o, const unsigned char *s, size_t n) {
    bits(o, 1, 1); bits(o, 1, 2);   /* final block, fixed Huffman */
    int *head = malloc(sizeof(int) << HBITS);
    if (!head) { o->oom = true; return; }
    for (int i = 0; i < (1 << HBITS); i++) head[i] = -1;
    size_t i = 0;
    while (i < n) {
        int best = 0, bd = 0;
        if (i + 3 <= n) {
            uint32_t h = ((uint32_t)s[i] * 2654435761u ^ (uint32_t)s[i + 1] * 40503u ^ (uint32_t)s[i + 2]) & ((1u << HBITS) - 1);
            int c = head[h]; head[h] = (int)i;
            /* the candidate, plus the previous pixel / row-start distances that flat images repeat */
            int cand[2] = { c, (int)i - 4 };
            for (int k = 0; k < 2; k++) {
                int p = cand[k];
                if (p < 0 || (size_t)p >= i || i - (size_t)p > 32768) continue;
                int L = 0; size_t mx = n - i < 258 ? n - i : 258;
                while ((size_t)L < mx && s[p + L] == s[i + L]) L++;
                if (L > best) { best = L; bd = (int)(i - (size_t)p); }
            }
        }
        if (best >= 3) { match(o, best, bd); i += (size_t)best; }
        else { lit(o, s[i]); i++; }
    }
    lit(o, 256);
    flush_bits(o);
    free(head);
}
static uint32_t crc_table[256]; static bool crc_ready;
static uint32_t crc(uint32_t c, const unsigned char *p, size_t n) {
    if (!crc_ready) { for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xEDB88320u ^ (x >> 1) : x >> 1; crc_table[i] = x; } crc_ready = true; }
    c ^= 0xffffffffu; for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8); return c ^ 0xffffffffu;
}
static void chunk(Out *o, const char *type, const unsigned char *d, size_t n) {
    be32(o, (uint32_t)n); size_t at = o->w; put(o, type, 4); if (n) put(o, d, n);
    if (o->oom) return;
    be32(o, crc(0, o->b + at, n + 4));
}
unsigned char *zlib_compress(const uint8_t *data, size_t n, size_t *len) {
    Out z = { 0 };
    byte(&z, 0x78); byte(&z, 0x01);
    deflate_fixed(&z, data, n);
    uint32_t a = 1, b = 0; for (size_t i = 0; i < n; i++) { a = (a + data[i]) % 65521; b = (b + a) % 65521; }
    be32(&z, (b << 16) | a);
    if (z.oom) { free(z.b); return NULL; }
    *len = z.w;
    return z.b;
}
unsigned char *png_encode_rgba(const uint8_t *rgba, int w, int h, int stride, size_t *len) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return NULL;
    size_t row = (size_t)w * 4, raw_n = (row + 1) * (size_t)h;
    unsigned char *raw = malloc(raw_n); if (!raw) return NULL;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = rgba + (size_t)y * (size_t)stride, *up = y ? rgba + (size_t)(y - 1) * (size_t)stride : NULL;
        unsigned char *d = raw + (size_t)y * (row + 1);
        /* Up for rows like the one above (backgrounds, bars), else Sub */
        bool same = up && !memcmp(src, up, row);
        d[0] = same ? 2 : 1;
        for (size_t x = 0; x < row; x++) d[1 + x] = same ? (unsigned char)(src[x] - up[x]) : (unsigned char)(src[x] - (x >= 4 ? src[x - 4] : 0));
    }
    Out z = { 0 };
    byte(&z, 0x78); byte(&z, 0x01);
    deflate_fixed(&z, raw, raw_n);
    uint32_t a = 1, b = 0; for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    be32(&z, (b << 16) | a);
    free(raw);
    if (z.oom) { free(z.b); return NULL; }
    Out o = { 0 };
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    put(&o, sig, 8);
    unsigned char ih[13] = { (unsigned char)(w >> 24), (unsigned char)(w >> 16), (unsigned char)(w >> 8), (unsigned char)w, (unsigned char)(h >> 24), (unsigned char)(h >> 16), (unsigned char)(h >> 8), (unsigned char)h, 8, 6, 0, 0, 0 };
    chunk(&o, "IHDR", ih, 13);
    chunk(&o, "IDAT", z.b, z.w);
    chunk(&o, "IEND", NULL, 0);
    free(z.b);
    if (o.oom) { free(o.b); return NULL; }
    *len = o.w;
    return o.b;
}
bool png_write_rgba(const char *path, const uint8_t *rgba, int w, int h, int stride, char *why, int n) {
    size_t len = 0;
    unsigned char *png = png_encode_rgba(rgba, w, h, stride, &len);
    if (!png) { snprintf(why, (size_t)n, "the image could not be encoded (size or memory)"); return false; }
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(png); snprintf(why, (size_t)n, "cannot write to the chosen folder"); return false; }
    bool ok = fwrite(png, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok; ok = (fclose(f) == 0) && ok;
    free(png);
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); snprintf(why, (size_t)n, "the export failed (disk full or no permission): no file was left behind"); return false; }
    return true;
}
