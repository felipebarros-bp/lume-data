/* SPDX-License-Identifier: AGPL-3.0-only */
/* XLSX / CSV writers for report exports: see xlsxw.h. */
#include "xlsxw.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t crc_tab[256];
static void crc_init(void) { if (crc_tab[1]) return; for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; } }
static uint32_t crc32_of(const unsigned char *p, size_t n) { uint32_t c = 0xFFFFFFFFu; for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFu; }
typedef struct { char name[64]; uint32_t crc, size, off; } ZEnt;
typedef struct { FILE *f; ZEnt e[64]; int n; bool failed; char *buf; size_t w, cap; } Zip;
static void le16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void le32(FILE *f, uint32_t v) { le16(f, v & 0xFFFF); le16(f, v >> 16); }
static void zput(Zip *z, const char *s, size_t n) {
    if (z->w + n + 1 > z->cap) { size_t c = z->cap ? z->cap * 2 : 65536; while (c < z->w + n + 1) c *= 2; char *nb = realloc(z->buf, c); if (!nb) { z->failed = true; return; } z->buf = nb; z->cap = c; }
    memcpy(z->buf + z->w, s, n); z->w += n;
}
static void zputs(Zip *z, const char *s) { zput(z, s, strlen(s)); }
static void zesc(Zip *z, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '&') zputs(z, "&amp;"); else if (c == '<') zputs(z, "&lt;"); else if (c == '>') zputs(z, "&gt;"); else if (c == '"') zputs(z, "&quot;");
        else if (c < 32 && c != '\t' && c != '\n' && c != '\r') continue;   /* not allowed in XML 1.0 */
        else zput(z, (const char *)&c, 1);
    }
}
static void zbegin(Zip *z) { z->w = 0; }
static void zend(Zip *z, const char *name) {
    if (z->failed || z->n >= 64) { z->failed = true; return; }
    ZEnt *e = &z->e[z->n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->crc = crc32_of((const unsigned char *)z->buf, z->w); e->size = (uint32_t)z->w; e->off = (uint32_t)ftell(z->f);
    size_t nl = strlen(name);
    le32(z->f, 0x04034b50); le16(z->f, 20); le16(z->f, 0); le16(z->f, 0); le16(z->f, 0); le16(z->f, 0x21);
    le32(z->f, e->crc); le32(z->f, e->size); le32(z->f, e->size); le16(z->f, (unsigned)nl); le16(z->f, 0);
    fwrite(name, 1, nl, z->f); fwrite(z->buf, 1, z->w, z->f);
}
static void zfinish(Zip *z) {
    uint32_t cd = (uint32_t)ftell(z->f);
    for (int i = 0; i < z->n; i++) {
        ZEnt *e = &z->e[i]; size_t nl = strlen(e->name);
        le32(z->f, 0x02014b50); le16(z->f, 20); le16(z->f, 20); le16(z->f, 0); le16(z->f, 0); le16(z->f, 0); le16(z->f, 0x21);
        le32(z->f, e->crc); le32(z->f, e->size); le32(z->f, e->size); le16(z->f, (unsigned)nl); le16(z->f, 0); le16(z->f, 0); le16(z->f, 0); le16(z->f, 0); le32(z->f, 0); le32(z->f, e->off);
        fwrite(e->name, 1, nl, z->f);
    }
    uint32_t cds = (uint32_t)ftell(z->f) - cd;
    le32(z->f, 0x06054b50); le16(z->f, 0); le16(z->f, 0); le16(z->f, (unsigned)z->n); le16(z->f, (unsigned)z->n); le32(z->f, cds); le32(z->f, cd); le16(z->f, 0);
}
static bool is_number(const char *v) {
    if (!v || !*v || strlen(v) > 30) return false;
    char *end; strtod(v, &end);
    if (*end) return false;
    if (v[0] == '0' && v[1] && v[1] != '.') return false;   /* codes such as 0123 stay text */
    for (const char *p = v; *p; p++) if (*p == 'x' || *p == 'X' || *p == 'n' || *p == 'N' || *p == 'i' || *p == 'I') return false;   /* hex, nan, inf */
    return true;
}
static void col_ref(int c, int r, char *out) {
    char l[8]; int k = 0; c++; while (c > 0) { int m = (c - 1) % 26; l[k++] = (char)('A' + m); c = (c - 1) / 26; }
    int j = 0; while (k) out[j++] = l[--k]; sprintf(out + j, "%d", r);
}
bool xlsx_write_sheets(const char *path, const char *const *names, const ResultSet *const *sets, int n, char *why, int wn) {
    if (n < 1 || n > 50) { snprintf(why, (size_t)wn, "nothing to export"); return false; }
    crc_init();
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    Zip z; memset(&z, 0, sizeof z);
    z.f = fopen(tmp, "wb");
    if (!z.f) { snprintf(why, (size_t)wn, "cannot write to the chosen folder"); return false; }
    char nm[64][32];   /* sheet names: at most 31 characters, no []:*?/\, unique */
    for (int i = 0; i < n; i++) {
        size_t k = 0; const char *s = names && names[i] && names[i][0] ? names[i] : "Sheet";
        for (; *s && k < 28; s++) { unsigned char c = (unsigned char)*s; if (strchr("[]:*?/\\", c)) continue; if (c >= 0x80 && k >= 25) break; nm[i][k++] = (char)c; }
        while (k && ((unsigned char)nm[i][k - 1] & 0xC0) == 0x80) k--;   /* never cut a UTF-8 sequence */
        if (k && ((unsigned char)nm[i][k - 1] & 0xC0) == 0xC0) k--;
        nm[i][k] = 0; if (!k) snprintf(nm[i], sizeof nm[i], "Sheet%d", i + 1);
        for (int j = 0; j < i; j++) if (!strcmp(nm[i], nm[j])) { char b[32]; snprintf(b, sizeof b, "%.24s %d", nm[i], i + 1); snprintf(nm[i], sizeof nm[i], "%s", b); }
    }
    zbegin(&z);
    zputs(&z, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
              "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>"
              "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>");
    for (int i = 0; i < n; i++) { char b[200]; snprintf(b, sizeof b, "<Override PartName=\"/xl/worksheets/sheet%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>", i + 1); zputs(&z, b); }
    zputs(&z, "</Types>"); zend(&z, "[Content_Types].xml");
    zbegin(&z);
    zputs(&z, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
              "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>");
    zend(&z, "_rels/.rels");
    zbegin(&z);
    zputs(&z, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>");
    for (int i = 0; i < n; i++) { char b[80]; zputs(&z, "<sheet name=\""); zesc(&z, nm[i]); snprintf(b, sizeof b, "\" sheetId=\"%d\" r:id=\"rId%d\"/>", i + 1, i + 1); zputs(&z, b); }
    zputs(&z, "</sheets></workbook>"); zend(&z, "xl/workbook.xml");
    zbegin(&z);
    zputs(&z, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
    for (int i = 0; i < n; i++) { char b[220]; snprintf(b, sizeof b, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet%d.xml\"/>", i + 1, i + 1); zputs(&z, b); }
    zputs(&z, "</Relationships>"); zend(&z, "xl/_rels/workbook.xml.rels");
    for (int i = 0; i < n; i++) {
        const ResultSet *rs = sets[i];
        zbegin(&z);
        zputs(&z, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>");
        char ref[24];
        zputs(&z, "<row r=\"1\">");
        for (int c = 0; rs && c < rs->ncols; c++) { col_ref(c, 1, ref); zputs(&z, "<c r=\""); zputs(&z, ref); zputs(&z, "\" t=\"inlineStr\"><is><t>"); zesc(&z, rs->cols[c] ? rs->cols[c] : ""); zputs(&z, "</t></is></c>"); }
        zputs(&z, "</row>");
        for (int r = 0; rs && r < rs->nrows && r < 1048575; r++) {
            char rb[32]; snprintf(rb, sizeof rb, "<row r=\"%d\">", r + 2); zputs(&z, rb);
            for (int c = 0; c < rs->ncols; c++) {
                const char *v = rs_cell(rs, r, c); if (!v) continue;
                col_ref(c, r + 2, ref);
                if (is_number(v)) { zputs(&z, "<c r=\""); zputs(&z, ref); zputs(&z, "\"><v>"); zputs(&z, v); zputs(&z, "</v></c>"); }
                else { zputs(&z, "<c r=\""); zputs(&z, ref); zputs(&z, "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">"); zesc(&z, v); zputs(&z, "</t></is></c>"); }
            }
            zputs(&z, "</row>");
        }
        zputs(&z, "</sheetData></worksheet>");
        char pn[64]; snprintf(pn, sizeof pn, "xl/worksheets/sheet%d.xml", i + 1); zend(&z, pn);
    }
    zfinish(&z);
    free(z.buf);
    bool ok = !z.failed && !ferror(z.f); ok = (fflush(z.f) == 0) && ok; ok = (fclose(z.f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); snprintf(why, (size_t)wn, "the export failed (disk full or no permission): no file was left behind"); return false; }
    return true;
}
bool csv_write_rs(const char *path, const ResultSet *rs, char *why, int wn) {
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { snprintf(why, (size_t)wn, "cannot write to the chosen folder"); return false; }
    fputs("\xEF\xBB\xBF", f);
    for (int r = -1; rs && r < rs->nrows; r++) {
        for (int c = 0; c < rs->ncols; c++) {
            const char *v = r < 0 ? rs->cols[c] : rs_cell(rs, r, c);
            if (c) fputc(',', f);
            if (!v) continue;
            /* a cell that a spreadsheet would read as a formula gets a leading apostrophe (numbers stay numbers) */
            bool formula = r >= 0 && (v[0] == '=' || v[0] == '+' || v[0] == '@' || (v[0] == '-' && !is_number(v)));
            bool q = formula || strpbrk(v, ",\"\r\n") != NULL;
            if (formula) { fputc('"', f); fputc('\'', f); q = false; }
            if (q) fputc('"', f);
            for (const char *p = v; *p; p++) { if (*p == '"') fputc('"', f); fputc(*p, f); }
            if (q || formula) fputc('"', f);
        }
        fputs("\r\n", f);
    }
    bool ok = !ferror(f); ok = (fflush(f) == 0) && ok; ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); snprintf(why, (size_t)wn, "the export failed (disk full or no permission): no file was left behind"); return false; }
    return true;
}
