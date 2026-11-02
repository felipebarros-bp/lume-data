/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Safe PNG / baseline JPEG decoding for the value viewer (US-057). Everything is bounds
 * checked before any allocation: dimensions are capped, the expected output size is known
 * from the header (so a decompression bomb stops at the cap, never at the heap), and any
 * malformed byte returns an error instead of a guess. No external resources are ever read.
 * Output is RGBA, 8 bits per channel, row-major. Not supported (reported, not crashed):
 * interlaced PNG, progressive / arithmetic / 12-bit / CMYK JPEG. */

/* Hard limits checked against the header before decoding: pixels per side and total pixels
 * (16 MP, i.e. at most 64 MB of RGBA output). */
#define IMAGE_MAX_DIM 4096
#define IMAGE_MAX_PIXELS (16u << 20)

/* A decoded image: w x h pixels, rgba is w * h * 4 bytes, heap owned (free with image_free). */
typedef struct Image { int w, h; unsigned char *rgba; } Image;

/* True when the bytes start with a PNG or JPEG signature; *kind receives a static name.
 * Cheap: only looks at the first 8 bytes. */
bool image_sniff(const unsigned char *p, size_t n, const char **kind);   /* "PNG" / "JPEG" */
/* Decodes a PNG or baseline JPEG held in memory. On success fills *out; on failure returns
 * false, leaves *out empty and writes a user-facing reason to err. Pure function of its
 * input; safe on untrusted data. */
bool image_decode(const unsigned char *p, size_t n, Image *out, char *err, size_t errn);
/* Frees the pixels and resets the Image to empty. */
void image_free(Image *im);
