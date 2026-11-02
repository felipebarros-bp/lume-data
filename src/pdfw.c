/* SPDX-License-Identifier: AGPL-3.0-only */
/* PDF writer: see pdfw.h. */
#include "pdfw.h"
#include "pngw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* PDF literal string: parentheses and backslashes escaped, control characters dropped, ASCII only */
static void pdf_str(FILE *f, const char *s) {
    fputc('(', f);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '(' || c == ')' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c >= 32 && c < 127) fputc(c, f);
        else if (c >= 0x80) fputc('?', f);
    }
    fputc(')', f);
}
bool pdf_write_rgba(const char *path, const char *title, const uint8_t *rgba, int w, int h, int stride, char *why, int n) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { snprintf(why, (size_t)n, "the page image has an invalid size"); return false; }
    size_t rgb_n = (size_t)w * (size_t)h * 3;
    uint8_t *rgb = malloc(rgb_n);
    if (!rgb) { snprintf(why, (size_t)n, "not enough memory for the PDF"); return false; }
    for (int y = 0; y < h; y++) {
        const uint8_t *s = rgba + (size_t)y * (size_t)stride; uint8_t *d = rgb + (size_t)y * (size_t)w * 3;
        for (int x = 0; x < w; x++) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d += 3; s += 4; }
    }
    size_t zl = 0; unsigned char *z = zlib_compress(rgb, rgb_n, &zl);
    free(rgb);
    if (!z) { snprintf(why, (size_t)n, "the page image could not be compressed"); return false; }
    double pw = 842.0, ph = pw * (double)h / (double)w;   /* A4 landscape width, the page's own height */
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(z); snprintf(why, (size_t)n, "cannot write to the chosen folder"); return false; }
    long off[7] = { 0 };
    fputs("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n", f);
    off[1] = ftell(f); fputs("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n", f);
    off[2] = ftell(f); fputs("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n", f);
    off[3] = ftell(f); fprintf(f, "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>\nendobj\n", pw, ph);
    off[4] = ftell(f); fprintf(f, "4 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length %zu >>\nstream\n", w, h, zl);
    fwrite(z, 1, zl, f); fputs("\nendstream\nendobj\n", f);
    free(z);
    char content[128]; int cl = snprintf(content, sizeof content, "q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q\n", pw, ph);
    off[5] = ftell(f); fprintf(f, "5 0 obj\n<< /Length %d >>\nstream\n%sendstream\nendobj\n", cl, content);
    off[6] = ftell(f); fputs("6 0 obj\n<< /Producer (Lume Data) /Title ", f); pdf_str(f, title); fputs(" >>\nendobj\n", f);
    long xref = ftell(f);
    fputs("xref\n0 7\n0000000000 65535 f \n", f);
    for (int i = 1; i <= 6; i++) fprintf(f, "%010ld 00000 n \n", off[i]);
    fprintf(f, "trailer\n<< /Size 7 /Root 1 0 R /Info 6 0 R >>\nstartxref\n%ld\n%%%%EOF\n", xref);
    bool ok = !ferror(f); ok = (fflush(f) == 0) && ok; ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); snprintf(why, (size_t)n, "the export failed (disk full or no permission): no file was left behind"); return false; }
    return true;
}
