/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Self-contained PNG and baseline JPEG decoders for the value viewer: see image.h for the
 * safety contract.
 *
 * Structure:
 *   - inflate: a small RFC 1951 decoder (stored, fixed and dynamic Huffman blocks) that
 *     writes into a buffer of exactly the size the PNG header announces, so oversized
 *     streams fail instead of growing memory;
 *   - PNG: chunk walk (IHDR, PLTE, tRNS, IDAT, IEND), inflate, per-row unfiltering, then
 *     conversion of every colour type / bit depth to RGBA;
 *   - JPEG: marker walk (DQT, DHT, SOF0/1, DRI, SOS), Huffman decoding of the single scan into
 *     one plane per component, float IDCT, then YCbCr to RGB with nearest-neighbour chroma
 *     upsampling.
 *
 * No global mutable state except the IDCT cosine table, which is filled on first use.
 * Called from the UI thread when a value is opened in the viewer.
 */
#include "image.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Writes msg into err and returns false, so error paths are one expression. */
static bool fail(char *err, size_t n, const char *msg) { snprintf(err, n, "%s", msg); return false; }

/* See image.h. */
bool image_sniff(const unsigned char *p, size_t n, const char **kind) {
    if (n >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) { *kind = "PNG"; return true; }
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) { *kind = "JPEG"; return true; }
    return false;
}

/* See image.h. */
void image_free(Image *im) { free(im->rgba); im->rgba = NULL; im->w = im->h = 0; }

/* ---- inflate (RFC 1951) ---------------------------------------------------- */

/* LSB-first bit reader over a byte buffer (deflate bit order). `bad` is set, and zeros
 * returned, once a read goes past the end. */
typedef struct { const unsigned char *p; size_t n, pos; uint32_t bitbuf; int bitcnt; bool bad; } BitIn;
/* Reads `need` bits (0..16), least significant first. */
static int bits(BitIn *b, int need) {
    while (b->bitcnt < need) {
        if (b->pos >= b->n) { b->bad = true; return 0; }
        b->bitbuf |= (uint32_t)b->p[b->pos++] << b->bitcnt;
        b->bitcnt += 8;
    }
    int v = (int)(b->bitbuf & ((1u << need) - 1));
    b->bitbuf >>= need; b->bitcnt -= need;
    return v;
}

/* Canonical Huffman table in the zlib "puff" style: number of codes per length and the
 * symbols sorted by code. */
typedef struct { short count[16]; short symbol[288]; } Huff;
/* Builds a table from n code lengths. Returns 0 for a complete code, > 0 for an incomplete
 * one (allowed only for single-code tables) and < 0 for an over-subscribed (invalid) one. */
static int huff_build(Huff *h, const short *lengths, int n) {
    short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return -1; }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++) if (lengths[i]) h->symbol[offs[lengths[i]]++] = (short)i;
    return left;
}
/* Decodes one symbol bit by bit; -1 on a bad code or end of input. */
static int huff_decode(BitIn *b, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(b, 1);
        if (b->bad) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

/* Fixed-capacity output buffer; writes past cap are refused (the bomb guard). */
typedef struct { unsigned char *out; size_t len, cap; } OutBuf;
/* Appends one byte; false when the buffer is full. */
static bool out_put(OutBuf *o, unsigned char c) { if (o->len >= o->cap) return false; o->out[o->len++] = c; return true; }

/* RFC 1951 length / distance base values and extra-bit counts. */
static const short LBASE[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short LEXT[] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short DBASE[] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short DEXT[] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* Decodes one Huffman-compressed block: literals, back references (checked against what
 * has been written) and the end-of-block symbol. False on any malformed code or overflow. */
static bool inflate_codes(BitIn *b, OutBuf *o, const Huff *lc, const Huff *dc) {
    for (;;) {
        int sym = huff_decode(b, lc);
        if (sym < 0) return false;
        if (sym < 256) { if (!out_put(o, (unsigned char)sym)) return false; }
        else if (sym == 256) return true;
        else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = LBASE[sym] + bits(b, LEXT[sym]);
            int ds = huff_decode(b, dc);
            if (ds < 0 || ds >= 30) return false;
            size_t dist = (size_t)DBASE[ds] + (size_t)bits(b, DEXT[ds]);
            if (b->bad || dist > o->len) return false;
            for (int i = 0; i < len; i++) if (!out_put(o, o->out[o->len - dist])) return false;
        }
    }
}

/* decompresses into out (exactly cap bytes expected); false on any malformed input or when
 * the stream would exceed the cap (decompression bomb) */
static bool inflate_zlib(const unsigned char *p, size_t n, unsigned char *out, size_t cap, char *err, size_t errn) {
    if (n < 2 || (p[0] & 0x0F) != 8 || ((p[0] << 8) | p[1]) % 31 != 0) return fail(err, errn, "bad zlib header");
    BitIn b = { p, n, 2, 0, 0, false };
    OutBuf o = { out, 0, cap };
    int last;
    do {
        last = bits(&b, 1);
        int type = bits(&b, 2);
        if (b.bad) return fail(err, errn, "truncated deflate stream");
        if (type == 0) {
            b.bitbuf = 0; b.bitcnt = 0;
            if (b.pos + 4 > b.n) return fail(err, errn, "truncated stored block");
            unsigned len = p[b.pos] | (p[b.pos + 1] << 8), nlen = p[b.pos + 2] | (p[b.pos + 3] << 8);
            b.pos += 4;
            if ((len ^ 0xFFFF) != nlen || b.pos + len > b.n) return fail(err, errn, "bad stored block");
            for (unsigned i = 0; i < len; i++) if (!out_put(&o, p[b.pos++])) return fail(err, errn, "image data longer than its header says");
        } else if (type == 1 || type == 2) {
            Huff lc, dc;
            short lengths[320];
            if (type == 1) {
                int i = 0;
                for (; i < 144; i++) lengths[i] = 8;
                for (; i < 256; i++) lengths[i] = 9;
                for (; i < 280; i++) lengths[i] = 7;
                for (; i < 288; i++) lengths[i] = 8;
                huff_build(&lc, lengths, 288);
                for (i = 0; i < 30; i++) lengths[i] = 5;
                huff_build(&dc, lengths, 30);
            } else {
                int nlen = bits(&b, 5) + 257, ndist = bits(&b, 5) + 1, ncode = bits(&b, 4) + 4;
                if (b.bad || nlen > 286 || ndist > 30) return fail(err, errn, "bad dynamic block header");
                static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                short cl[19] = { 0 };
                for (int i = 0; i < ncode; i++) cl[order[i]] = (short)bits(&b, 3);
                Huff ch;
                if (b.bad || huff_build(&ch, cl, 19) != 0) return fail(err, errn, "bad code-length code");
                int idx = 0;
                while (idx < nlen + ndist) {
                    int sym = huff_decode(&b, &ch);
                    if (sym < 0) return fail(err, errn, "bad code lengths");
                    if (sym < 16) lengths[idx++] = (short)sym;
                    else {
                        int rep, val = 0;
                        if (sym == 16) { if (idx == 0) return fail(err, errn, "bad repeat"); val = lengths[idx - 1]; rep = 3 + bits(&b, 2); }
                        else if (sym == 17) rep = 3 + bits(&b, 3);
                        else rep = 11 + bits(&b, 7);
                        if (b.bad || idx + rep > nlen + ndist) return fail(err, errn, "bad repeat");
                        while (rep--) lengths[idx++] = (short)val;
                    }
                }
                if (lengths[256] == 0) return fail(err, errn, "no end-of-block code");
                int e1 = huff_build(&lc, lengths, nlen), e2 = huff_build(&dc, lengths + nlen, ndist);
                if (e1 < 0 || (e1 > 0 && nlen - lc.count[0] != 1) || e2 < 0 || (e2 > 0 && ndist - dc.count[0] != 1)) return fail(err, errn, "over-subscribed Huffman code");
            }
            if (!inflate_codes(&b, &o, &lc, &dc)) return fail(err, errn, b.bad ? "truncated image data" : o.len >= o.cap ? "image data longer than its header says" : "corrupt deflate data");
        } else return fail(err, errn, "invalid deflate block type");
    } while (!last);
    if (o.len != cap) return fail(err, errn, "image data shorter than its header says");
    return true;
}

/* ---- PNG ----------------------------------------------------------------- */

/* Big-endian 32-bit read. */
static uint32_t be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
/* PNG Paeth predictor: whichever of left, up, upper-left is closest to left + up - upper-left. */
static int paeth(int a, int b, int c) { int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c); return pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }

/* Decodes a PNG (signature already checked). Steps: walk the chunks collecting header,
 * palette, transparency and the concatenated IDAT data (capped at 64 MB); inflate into a
 * buffer of the exact expected size; undo the per-row filters; expand to RGBA. CRCs are not
 * verified; ancillary chunks are skipped. */
static bool png_decode(const unsigned char *p, size_t n, Image *out, char *err, size_t errn) {
    size_t pos = 8;
    uint32_t w = 0, h = 0; int depth = 0, ctype = 0, interlace = 0;
    unsigned char pal[256][3]; int npal = 0; unsigned char trns[256]; int ntrns = 0; bool has_ihdr = false;
    unsigned char *idat = NULL; size_t idat_len = 0, idat_cap = 0;
    const size_t IDAT_MAX = 64u << 20;
    while (pos + 8 <= n) {
        uint32_t len = be32(p + pos); const unsigned char *type = p + pos + 4;
        if (len > n - pos - 8) { free(idat); return fail(err, errn, "chunk length past the end of the data (truncated?)"); }
        const unsigned char *d = p + pos + 8;
        if (memcmp(type, "IHDR", 4) == 0) {
            if (len != 13) { free(idat); return fail(err, errn, "bad IHDR"); }
            w = be32(d); h = be32(d + 4); depth = d[8]; ctype = d[9]; interlace = d[12];
            if (w == 0 || h == 0 || w > IMAGE_MAX_DIM || h > IMAGE_MAX_DIM || (uint64_t)w * h > IMAGE_MAX_PIXELS) { free(idat); snprintf(err, errn, "%ux%u exceeds the preview limit (%d px per side, 16 MP)", w, h, IMAGE_MAX_DIM); return false; }
            if (interlace) { free(idat); return fail(err, errn, "interlaced (Adam7) PNG is not supported"); }
            if (d[10] != 0 || d[11] != 0) { free(idat); return fail(err, errn, "unknown compression or filter method"); }
            bool ok = (ctype == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) || ((ctype == 2 || ctype == 4 || ctype == 6) && (depth == 8 || depth == 16)) || (ctype == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8));
            if (!ok) { free(idat); return fail(err, errn, "invalid colour type / bit depth"); }
            has_ihdr = true;
        } else if (!has_ihdr) { free(idat); return fail(err, errn, "IHDR is not the first chunk"); }
        else if (memcmp(type, "PLTE", 4) == 0) { if (len % 3 || len > 768) { free(idat); return fail(err, errn, "bad palette"); } npal = (int)len / 3; memcpy(pal, d, len); }
        else if (memcmp(type, "tRNS", 4) == 0) { if (ctype == 3) { ntrns = len > 256 ? 256 : (int)len; memcpy(trns, d, ntrns); } }
        else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_len + len > IDAT_MAX) { free(idat); return fail(err, errn, "compressed data over 64 MB"); }
            if (idat_len + len > idat_cap) { idat_cap = (idat_len + len) * 2; idat = realloc(idat, idat_cap); }
            memcpy(idat + idat_len, d, len); idat_len += len;
        } else if (memcmp(type, "IEND", 4) == 0) break;
        pos += 12 + len;
    }
    if (!has_ihdr || !idat) { free(idat); return fail(err, errn, "no image header or no image data"); }
    /* decompress: each row is a filter byte followed by rowbytes of samples */
    int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
    size_t bpp = (size_t)(channels * depth + 7) / 8;                 /* bytes per complete pixel (>= 1) */
    size_t rowbytes = ((size_t)w * channels * depth + 7) / 8;
    size_t raw_len = (rowbytes + 1) * h;
    unsigned char *raw = malloc(raw_len);
    if (!raw) { free(idat); return fail(err, errn, "out of memory"); }
    if (!inflate_zlib(idat, idat_len, raw, raw_len, err, errn)) { free(idat); free(raw); return false; }
    free(idat);
    /* unfilter in place */
    unsigned char *prev = NULL;
    for (uint32_t y = 0; y < h; y++) {
        unsigned char *row = raw + y * (rowbytes + 1);
        int ft = row[0]; unsigned char *cur = row + 1;
        if (ft > 4) { free(raw); return fail(err, errn, "invalid filter type"); }
        for (size_t x = 0; x < rowbytes; x++) {
            int a = x >= bpp ? cur[x - bpp] : 0, b = prev ? prev[x] : 0, c = prev && x >= bpp ? prev[x - bpp] : 0;
            int v = cur[x];
            switch (ft) { case 1: v += a; break; case 2: v += b; break; case 3: v += (a + b) / 2; break; case 4: v += paeth(a, b, c); break; }
            cur[x] = (unsigned char)v;
        }
        prev = cur;
    }
    /* expand every colour type and bit depth to 8-bit RGBA (16-bit samples keep the high byte) */
    out->w = (int)w; out->h = (int)h;
    out->rgba = malloc((size_t)w * h * 4);
    if (!out->rgba) { free(raw); return fail(err, errn, "out of memory"); }
    for (uint32_t y = 0; y < h; y++) {
        const unsigned char *cur = raw + y * (rowbytes + 1) + 1;
        unsigned char *dst = out->rgba + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, dst += 4) {
            unsigned r = 0, g = 0, b = 0, a = 255;
            if (depth == 8 || depth == 16) {
                int step = depth / 8;
                const unsigned char *px = cur + (size_t)x * channels * step;
                #define CH(i) (px[(i) * step])
                if (ctype == 0) r = g = b = CH(0);
                else if (ctype == 2) { r = CH(0); g = CH(1); b = CH(2); }
                else if (ctype == 3) { int i = CH(0); if (i < npal) { r = pal[i][0]; g = pal[i][1]; b = pal[i][2]; } if (i < ntrns) a = trns[i]; }
                else if (ctype == 4) { r = g = b = CH(0); a = CH(1); }
                else { r = CH(0); g = CH(1); b = CH(2); a = CH(3); }
                #undef CH
            } else {
                /* 1 / 2 / 4-bit grey or palette index */
                size_t bit = (size_t)x * depth;
                int v = (cur[bit / 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1);
                if (ctype == 3) { if (v < npal) { r = pal[v][0]; g = pal[v][1]; b = pal[v][2]; } if (v < ntrns) a = trns[v]; }
                else r = g = b = v * 255 / ((1 << depth) - 1);
            }
            dst[0] = (unsigned char)r; dst[1] = (unsigned char)g; dst[2] = (unsigned char)b; dst[3] = (unsigned char)a;
        }
    }
    free(raw);
    return true;
}

/* ---- baseline JPEG --------------------------------------------------------- */

/* JPEG Huffman table (bits = code count per length, vals = symbols) with the JPEG spec's
 * decoding tables (mincode / maxcode / valptr); ok once defined by a DHT segment. */
typedef struct { unsigned char bits[17]; unsigned char vals[256]; int mincode[17], maxcode[18], valptr[17]; bool ok; } JHuff;
/* A colour component: id, sampling factors, quantisation / DC / AC table ids, running DC
 * predictor and its decoded plane (pw x ph, padded to whole MCUs). */
typedef struct { int id, h, v, tq, td, ta; int dc_pred; unsigned char *plane; int pw, ph; } JComp;
/* MSB-first bit reader over entropy-coded JPEG data; removes 0xFF00 stuffing and stops (pads
 * with zeros) at the first marker. */
typedef struct { const unsigned char *p; size_t n, pos; uint32_t buf; int cnt; bool marker_hit; } JBits;

/* Derives the decoding tables from bits[] (JPEG spec, Annex F.2.2.3). */
static void jhuff_build(JHuff *h) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k; h->mincode[l] = code;
        code += h->bits[l]; k += h->bits[l];
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    h->ok = true;
}
/* Next bit of the scan; zeros once the data or the scan ends. */
static int jbit(JBits *b) {
    if (b->cnt == 0) {
        if (b->pos >= b->n || b->marker_hit) { b->buf = 0; b->cnt = 8; }   /* pad with zeros past the end */
        else {
            unsigned char c = b->p[b->pos++];
            if (c == 0xFF) { unsigned char c2 = b->pos < b->n ? b->p[b->pos] : 0; if (c2 == 0) b->pos++; else { b->marker_hit = true; c = 0; } }
            b->buf = c; b->cnt = 8;
        }
    }
    b->cnt--;
    return (b->buf >> b->cnt) & 1;
}
/* Next n bits as an unsigned number. */
static int jbits(JBits *b, int n) { int v = 0; while (n--) v = (v << 1) | jbit(b); return v; }
/* Decodes one Huffman symbol; -1 when no code of up to 16 bits matches. */
static int jhuff_decode(JBits *b, const JHuff *h) {
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | jbit(b);
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l]) return h->vals[h->valptr[l] + code - h->mincode[l]];
    }
    return -1;
}
/* Turns a t-bit magnitude into its signed value (JPEG EXTEND procedure). ZIGZAG maps
 * coefficient order to row-major position in the 8x8 block. */
static int jextend(int v, int t) { return t == 0 ? 0 : v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }
static const int ZIGZAG[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

/* Separable float inverse DCT of one dequantised block, level-shifted by 128 and clamped,
 * written into an 8x8 area of a plane with the given stride. The cosine table is built on
 * the first call. */
static void idct8x8(const int *in, unsigned char *out, int stride) {
    static float c[8][8]; static bool init;
    if (!init) { for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++) c[x][u] = (float)((u == 0 ? sqrt(0.5) : 1.0) * cos((2 * x + 1) * u * M_PI / 16) / 2); init = true; }
    float tmp[64];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { float s = 0; for (int u = 0; u < 8; u++) s += c[x][u] * in[y * 8 + u]; tmp[y * 8 + x] = s; }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) {
        float s = 0; for (int v = 0; v < 8; v++) s += c[y][v] * tmp[v * 8 + x];
        int val = (int)lrintf(s + 128); out[y * stride + x] = (unsigned char)(val < 0 ? 0 : val > 255 ? 255 : val);
    }
}

/* Decodes a baseline (or extended sequential, Huffman) JPEG with 1 or 3 components and
 * sampling factors up to 2. Walks the markers, rejects unsupported variants with a precise
 * message, and decodes the first scan, which must contain every component. */
static bool jpeg_decode(const unsigned char *p, size_t n, Image *out, char *err, size_t errn) {
    unsigned char qt[4][64]; bool has_qt[4] = { false };
    JHuff hdc[4], hac[4]; memset(hdc, 0, sizeof hdc); memset(hac, 0, sizeof hac);
    JComp comp[4]; int ncomp = 0, w = 0, h = 0, restart = 0;
    size_t pos = 2;
    bool frame = false;
    while (pos + 4 <= n) {
        if (p[pos] != 0xFF) { pos++; continue; }
        int m = p[pos + 1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { pos += 2; continue; }
        if (m == 0xFF) { pos++; continue; }
        if (m == 0xD9) break;
        size_t len = ((size_t)p[pos + 2] << 8) | p[pos + 3];
        if (len < 2 || pos + 2 + len > n) return fail(err, errn, "segment length past the end of the data (truncated?)");
        const unsigned char *d = p + pos + 4; size_t dl = len - 2;
        /* DQT: quantisation tables, stored in natural (row-major) order */
        if (m == 0xDB) {
            size_t i = 0;
            while (i < dl) { int pq = d[i] >> 4, tq = d[i] & 15; if (tq > 3 || pq > 1 || i + 1 + 64 * (pq + 1) > dl) return fail(err, errn, "bad quantisation table"); for (int k = 0; k < 64; k++) qt[tq][ZIGZAG[k]] = pq ? d[i + 1 + 2 * k + 1] : d[i + 1 + k]; has_qt[tq] = true; i += 1 + 64 * (pq + 1); }
        /* DHT: Huffman tables */
        } else if (m == 0xC4) {
            size_t i = 0;
            while (i + 17 <= dl) {
                int tc = d[i] >> 4, th = d[i] & 15; if (th > 3 || tc > 1) return fail(err, errn, "bad Huffman table id");
                JHuff *hh = tc ? &hac[th] : &hdc[th]; int total = 0;
                hh->bits[0] = 0; for (int l = 1; l <= 16; l++) { hh->bits[l] = d[i + l]; total += d[i + l]; }
                if (total > 256 || i + 17 + total > dl) return fail(err, errn, "bad Huffman table");
                memcpy(hh->vals, d + i + 17, total); jhuff_build(hh); i += 17 + total;
            }
        /* SOF0 / SOF1: image size and components */
        } else if (m == 0xC0 || m == 0xC1) {
            if (dl < 6) return fail(err, errn, "bad frame header");
            if (d[0] != 8) return fail(err, errn, "only 8-bit samples are supported");
            h = (d[1] << 8) | d[2]; w = (d[3] << 8) | d[4]; ncomp = d[5];
            if (w <= 0 || h <= 0 || w > IMAGE_MAX_DIM || h > IMAGE_MAX_DIM || (uint64_t)w * h > IMAGE_MAX_PIXELS) { snprintf(err, errn, "%dx%d exceeds the preview limit (%d px per side, 16 MP)", w, h, IMAGE_MAX_DIM); return false; }
            if (ncomp != 1 && ncomp != 3) return fail(err, errn, ncomp == 4 ? "CMYK JPEG is not supported" : "unsupported component count");
            if (dl < 6 + 3 * (size_t)ncomp) return fail(err, errn, "bad frame header");
            for (int i = 0; i < ncomp; i++) { comp[i].id = d[6 + i * 3]; comp[i].h = d[7 + i * 3] >> 4; comp[i].v = d[7 + i * 3] & 15; comp[i].tq = d[8 + i * 3]; if (comp[i].h < 1 || comp[i].h > 2 || comp[i].v < 1 || comp[i].v > 2 || comp[i].tq > 3) return fail(err, errn, "unsupported sampling factors"); comp[i].plane = NULL; }
            frame = true;
        } else if (m == 0xC2 || m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) return fail(err, errn, m == 0xC2 ? "progressive JPEG is not supported" : "only baseline (sequential Huffman) JPEG is supported");
        /* DRI: restart interval in MCUs */
        else if (m == 0xDD) { if (dl < 2) return fail(err, errn, "bad DRI"); restart = (d[0] << 8) | d[1]; }
        /* SOS: validate the scan header, then decode the whole image */
        else if (m == 0xDA) {
            if (!frame) return fail(err, errn, "scan before frame header");
            if (dl < 1) return fail(err, errn, "bad SOS");   /* d[0] is past the segment otherwise */
            int ns = d[0];
            if (ns != ncomp || dl < 1 + 2 * (size_t)ns + 3) return fail(err, errn, "only single-scan (baseline) JPEG is supported");
            for (int i = 0; i < ns; i++) {
                int id = d[1 + i * 2], tt = d[2 + i * 2]; JComp *c = NULL;
                for (int k = 0; k < ncomp; k++) if (comp[k].id == id) c = &comp[k];
                if (!c) return fail(err, errn, "scan names an unknown component");
                c->td = tt >> 4; c->ta = tt & 15;
                if (c->td > 3 || c->ta > 3 || !hdc[c->td].ok || !hac[c->ta].ok || !has_qt[c->tq]) return fail(err, errn, "scan uses a table that was not defined");
            }
            pos += 2 + len;
            /* decode the entropy-coded segment */
            int hmax = 1, vmax = 1;
            for (int i = 0; i < ncomp; i++) { if (comp[i].h > hmax) hmax = comp[i].h; if (comp[i].v > vmax) vmax = comp[i].v; }
            int mcuw = 8 * hmax, mcuh = 8 * vmax, mcux = (w + mcuw - 1) / mcuw, mcuy = (h + mcuh - 1) / mcuh;
            for (int i = 0; i < ncomp; i++) {
                comp[i].pw = mcux * comp[i].h * 8; comp[i].ph = mcuy * comp[i].v * 8;
                comp[i].plane = calloc((size_t)comp[i].pw * comp[i].ph, 1);
                if (!comp[i].plane) { for (int k = 0; k < i; k++) free(comp[k].plane); return fail(err, errn, "out of memory"); }
                comp[i].dc_pred = 0;
            }
            JBits b = { p, n, pos, 0, 0, false };
            int blk[64];
            bool bad = false;
            for (int my = 0; my < mcuy && !bad; my++) for (int mx = 0; mx < mcux && !bad; mx++) {
                if (restart && (my * mcux + mx) && ((my * mcux + mx) % restart) == 0) {
                    /* byte-align and expect RSTn */
                    b.cnt = 0;
                    if (b.marker_hit) { b.marker_hit = false; b.pos--; }
                    while (b.pos + 1 < n && !(p[b.pos] == 0xFF && p[b.pos + 1] >= 0xD0 && p[b.pos + 1] <= 0xD7)) b.pos++;
                    if (b.pos + 1 < n) b.pos += 2;
                    for (int i = 0; i < ncomp; i++) comp[i].dc_pred = 0;
                }
                for (int ci = 0; ci < ncomp; ci++) {
                    JComp *c = &comp[ci];
                    for (int by = 0; by < c->v; by++) for (int bx = 0; bx < c->h; bx++) {
                        memset(blk, 0, sizeof blk);
                        int t = jhuff_decode(&b, &hdc[c->td]);
                        if (t < 0 || t > 11) { bad = true; break; }
                        int diff = jextend(jbits(&b, t), t);
                        c->dc_pred += diff;
                        blk[0] = c->dc_pred * qt[c->tq][0];
                        for (int k = 1; k < 64;) {
                            int rs = jhuff_decode(&b, &hac[c->ta]);
                            if (rs < 0) { bad = true; break; }
                            int r = rs >> 4, s = rs & 15;
                            if (s == 0) { if (r == 15) { k += 16; continue; } break; }
                            k += r;
                            if (k > 63) { bad = true; break; }
                            blk[ZIGZAG[k]] = jextend(jbits(&b, s), s) * qt[c->tq][ZIGZAG[k]];
                            k++;
                        }
                        if (bad) break;
                        int px = (mx * c->h + bx) * 8, py = (my * c->v + by) * 8;
                        idct8x8(blk, c->plane + (size_t)py * c->pw + px, c->pw);
                    }
                    if (bad) break;
                }
                if (b.pos >= n && !b.marker_hit && (my * mcux + mx) < mcux * mcuy - 1) { bad = true; }
            }
            if (bad) { for (int i = 0; i < ncomp; i++) free(comp[i].plane); return fail(err, errn, "corrupt or truncated scan data"); }
            /* colour conversion with nearest-neighbour chroma upsampling */
            out->w = w; out->h = h;
            out->rgba = malloc((size_t)w * h * 4);
            if (!out->rgba) { for (int i = 0; i < ncomp; i++) free(comp[i].plane); return fail(err, errn, "out of memory"); }
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                unsigned char *dst = out->rgba + ((size_t)y * w + x) * 4;
                if (ncomp == 1) { unsigned char g = comp[0].plane[(size_t)y * comp[0].pw + x]; dst[0] = dst[1] = dst[2] = g; dst[3] = 255; continue; }
                float Y = comp[0].plane[(size_t)(y * comp[0].v / vmax) * comp[0].pw + x * comp[0].h / hmax];
                float Cb = comp[1].plane[(size_t)(y * comp[1].v / vmax) * comp[1].pw + x * comp[1].h / hmax] - 128.0f;
                float Cr = comp[2].plane[(size_t)(y * comp[2].v / vmax) * comp[2].pw + x * comp[2].h / hmax] - 128.0f;
                int r = (int)lrintf(Y + 1.402f * Cr), g = (int)lrintf(Y - 0.344136f * Cb - 0.714136f * Cr), bl = (int)lrintf(Y + 1.772f * Cb);
                dst[0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r); dst[1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g); dst[2] = (unsigned char)(bl < 0 ? 0 : bl > 255 ? 255 : bl); dst[3] = 255;
            }
            for (int i = 0; i < ncomp; i++) free(comp[i].plane);
            return true;
        }
        pos += 2 + len;
    }
    return fail(err, errn, frame ? "no scan data (truncated?)" : "no frame header");
}

/* See image.h. Dispatches on the signature. */
bool image_decode(const unsigned char *p, size_t n, Image *out, char *err, size_t errn) {
    const char *kind;
    memset(out, 0, sizeof *out);
    err[0] = 0;
    if (!image_sniff(p, n, &kind)) return fail(err, errn, "not a PNG or JPEG signature");
    bool ok = kind[0] == 'P' ? png_decode(p, n, out, err, errn) : jpeg_decode(p, n, out, err, errn);
    if (!ok) image_free(out);
    return ok;
}
