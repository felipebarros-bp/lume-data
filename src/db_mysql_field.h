/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/* The stable leading part of MYSQL_FIELD (mysql.h / mariadb_com.h): name ... type. The driver
 * loads the client library at run time and never includes its header; tests/mysql_layout.c checks
 * these offsets against the real header when it is installed. */
/* the stable leading part of MYSQL_FIELD (mysql.h / mariadb_com.h) */
typedef struct LumeMyField {
    char *name, *org_name, *table, *org_table, *db, *catalog, *def;
    unsigned long length, max_length;
    unsigned int name_length, org_name_length, table_length, org_table_length, db_length, catalog_length, def_length;
    unsigned int flags, decimals, charsetnr;
    int type;   /* enum enum_field_types */
} LumeMyField;
