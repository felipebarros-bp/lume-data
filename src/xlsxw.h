/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * XLSX workbook with one sheet per result set (report export: one sheet per card). A stored (not
 * compressed) ZIP of the minimal SpreadsheetML parts; numbers as numbers, everything else as inline
 * strings (never evaluated as formulas). Written to "<path>.tmp" and renamed only when complete.
 */
#include <stdbool.h>
#include "rs.h"
bool xlsx_write_sheets(const char *path, const char *const *names, const ResultSet *const *sets, int n, char *why, int wn);
/* CSV (RFC 4180, UTF-8 with BOM so spreadsheet apps read the accents) of one result set */
bool csv_write_rs(const char *path, const ResultSet *rs, char *why, int wn);
