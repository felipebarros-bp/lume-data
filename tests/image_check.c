/* SPDX-License-Identifier: AGPL-3.0-only */
/* Headless checks of the PNG / JPEG decoder (US-057): valid files of several kinds decode to
 * the expected pixels; truncated, oversized ("bomb" headers), corrupt and mislabelled data
 * are refused with a message and never crash or allocate the claimed size. */
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static void check(bool ok, const char *what, const char *detail) {
    printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) failures++;
}
static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) { *n = 0; return NULL; }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(l > 0 ? l : 1); *n = fread(b, 1, l, f); fclose(f); return b;
}
static bool px_is(Image *im, int x, int y, int r, int g, int b, int a, int tol) {
    unsigned char *p = im->rgba + ((size_t)y * im->w + x) * 4;
    return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol && abs(p[3] - a) <= tol;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[600], err[200]; size_t n; Image im; bool ok;
    #define LOAD(name) (snprintf(path, sizeof path, "%s/%s", dir, name), slurp(path, &n))

    unsigned char *d = LOAD("rgb.png");
    ok = d && image_decode(d, n, &im, err, sizeof err);
    check(ok && im.w == 4 && im.h == 3 && px_is(&im, 0, 0, 255, 0, 0, 255, 0) && px_is(&im, 3, 2, 10, 20, 30, 255, 0), "RGB 8-bit PNG with Sub/Up/Paeth filters decodes exactly", ok ? "" : err);
    if (ok) image_free(&im); free(d);

    d = LOAD("rgba16.png");
    ok = d && image_decode(d, n, &im, err, sizeof err);
    check(ok && im.w == 2 && im.h == 2 && px_is(&im, 1, 1, 0, 0, 255, 128, 1), "RGBA 16-bit PNG: high bytes taken, alpha kept", ok ? "" : err);
    if (ok) image_free(&im); free(d);

    d = LOAD("pal.png");
    ok = d && image_decode(d, n, &im, err, sizeof err);
    check(ok && im.w == 8 && im.h == 1 && px_is(&im, 0, 0, 0, 255, 0, 255, 0) && px_is(&im, 1, 0, 255, 255, 0, 0, 0), "2-bit palette PNG with tRNS: colours and transparency", ok ? "" : err);
    if (ok) image_free(&im); free(d);

    d = LOAD("gray1.png");
    ok = d && image_decode(d, n, &im, err, sizeof err);
    check(ok && im.w == 10 && px_is(&im, 0, 0, 255, 255, 255, 255, 0) && px_is(&im, 1, 0, 0, 0, 0, 255, 0), "1-bit greyscale PNG", ok ? "" : err);
    if (ok) image_free(&im); free(d);

    d = LOAD("big.png");
    ok = d && image_decode(d, n, &im, err, sizeof err);
    check(ok && im.w == 300 && im.h == 200 && px_is(&im, 150, 100, 150, 100, 250, 255, 0), "300x200 PNG with a dynamic Huffman block", ok ? "" : err);
    if (ok) image_free(&im); free(d);

    d = LOAD("truncated.png");
    ok = d && image_decode(d, n, &im, err, sizeof err);
    check(!ok && err[0], "truncated PNG is refused with a message", err);
    free(d);

    d = LOAD("bomb.png");
    ok = d && image_decode(d, n, &im, err, sizeof err);
    check(!ok && strstr(err, "exceeds the preview limit"), "100000x100000 header is refused before any allocation", err);
    free(d);

    d = LOAD("lying.png");
    ok = d && image_decode(d, n, &im, err, sizeof err);
    check(!ok && strstr(err, "longer than its header"), "IDAT that inflates past the declared size stops at the cap", err);
    free(d);

    d = LOAD("garbage.png");
    ok = d && image_decode(d, n, &im, err, sizeof err);
    check(!ok && err[0], "PNG signature followed by random bytes is refused", err);
    free(d);

    d = LOAD("interlaced.png");
    ok = d && image_decode(d, n, &im, err, sizeof err);
    check(!ok && strstr(err, "interlaced"), "interlaced PNG reported as unsupported", err);
    free(d);

    d = LOAD("photo.jpg");
    if (d && n) {
        ok = image_decode(d, n, &im, err, sizeof err);
        check(ok && im.w == 64 && im.h == 48 && px_is(&im, 10, 10, 200, 30, 30, 255, 12) && px_is(&im, 50, 40, 30, 30, 200, 255, 12), "baseline JPEG (4:2:0 from sips) decodes to the expected colours", ok ? "" : err);
        if (ok) image_free(&im);
        size_t half = n / 2;
        ok = image_decode(d, half, &im, err, sizeof err);
        check(!ok && err[0], "truncated JPEG is refused with a message", err);
        unsigned char *bad = malloc(n); memcpy(bad, d, n);
        for (size_t i = n / 3; i < n / 3 + 40 && i < n; i++) bad[i] ^= 0x5A;
        ok = image_decode(bad, n, &im, err, sizeof err);
        check(true, ok ? "corrupted JPEG still decodes without crashing (garbage pixels)" : "corrupted JPEG refused without crashing", ok ? "" : err);
        if (ok) image_free(&im);
        free(bad);
    } else printf("skip  JPEG checks (no photo.jpg: sips unavailable)\n");
    free(d);

    d = LOAD("gray.jpg");
    if (d && n) {
        ok = image_decode(d, n, &im, err, sizeof err);
        check(ok && im.w == 16 && px_is(&im, 2, 2, 128, 128, 128, 255, 6), "greyscale JPEG", ok ? "" : err);
        if (ok) image_free(&im);
    }
    free(d);

    unsigned char fake[16] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1 };
    ok = image_decode(fake, sizeof fake, &im, err, sizeof err);
    check(!ok && err[0], "JPEG signature with no frame is refused", err);

    printf("%s (%d failures)\n", failures ? "IMAGE: FAIL" : "IMAGE: OK", failures);
    return failures ? 1 : 0;
}
