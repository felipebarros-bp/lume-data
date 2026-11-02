---
title: Schema Explorer
sidebar_position: 0
---

# Schema Explorer

The Explorer lists your connections and, under each, the database's objects. It is built for servers with thousands of schemas and tens of thousands of tables: names are read a page at a time and only when you open them.

## Schemas and Others

Under a connection, **Schemas** (MySQL / MariaDB: **Databases**) shows the schemas that matter to you: the current one, the ones you pinned, the ones you used recently and a few with objects you can see. **Others** holds the rest; it shows a count only when the count is exact and cheap. Right-click a schema to **Pin** it, **Find Objects...**, open a **SQL Console**, create a table or start an **ER diagram**.

## Search

**Cmd+F** in the Explorer searches schema names on the server, by prefix (or "Contains" on request), across pages never loaded.

## Tables

- Double-click a table to preview its rows.
- Right-click for **Modify Table...**, **Edit Data**, **Generate SQL > CREATE**, **Duplicate Structure...**, **Dependencies...**, **Show ER Diagram** and **Drop Table...** (always confirmed).
- **Cmd+click** several tables, or **Shift+click** a range, then **Show ER Diagram (N tables)**.
