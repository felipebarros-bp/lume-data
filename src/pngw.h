/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * PNG writer for report exports (LI-070): 8-bit RGBA, filter Sub/Up per row, deflate with fixed
 * Huffman codes and a small LZ77 matcher (flat chart colours compress well). The file is written
 * to "<path>.tmp" and renamed only when complete; on any failure the temporary file is removed and
 * nothing is left that looks like a finished export (R05). Pure C.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* the PNG bytes in memory (malloc'd, *len set) */
unsigned char *png_encode_rgba(const uint8_t *rgba, int w, int h, int stride, size_t *len);
bool png_write_rgba(const char *path, const uint8_t *rgba, int w, int h, int stride, char *why, int n);

/* zlib stream (RFC 1950) of n bytes, fixed-Huffman deflate as for the PNGs (PDF /FlateDecode); malloc'd */
unsigned char *zlib_compress(const uint8_t *data, size_t n, size_t *len);
