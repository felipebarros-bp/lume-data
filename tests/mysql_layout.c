/* SPDX-License-Identifier: AGPL-3.0-only */
/* MYSQL_FIELD layout: the driver's mirror (db_mysql_field.h) against the installed mysql.h. */
#include <mysql.h>
#include <stddef.h>
#include <stdio.h>
#include "db_mysql_field.h"
#define SAME(f) (offsetof(MYSQL_FIELD, f) == offsetof(LumeMyField, f))
int main(void) {
    int ok = SAME(name) && SAME(org_name) && SAME(table) && SAME(db) && SAME(def) && SAME(length) && SAME(max_length) && SAME(name_length) &&
             SAME(def_length) && SAME(flags) && SAME(decimals) && SAME(charsetnr) && SAME(type);
    printf("%s  MYSQL_FIELD layout: the driver's mirror matches the installed mysql.h (type at offset %zu)\n", ok ? "ok   " : "FAIL ", offsetof(MYSQL_FIELD, type));
    return !ok;
}
