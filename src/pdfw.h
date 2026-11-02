/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * PDF export of a rendered report page: one page holding the page image (RGB, /FlateDecode), sized
 * to the image's proportions with an A4-landscape width. Written to "<path>.tmp" and renamed only when
 * complete. The text is part of the image (not selectable): a vector PDF is BL-043.
 */
#include <stdbool.h>
#include <stdint.h>
bool pdf_write_rgba(const char *path, const char *title, const uint8_t *rgba, int w, int h, int stride, char *why, int n);
