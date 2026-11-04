---
title: SQL editor and completion
sidebar_position: 1
---

# SQL editor and completion

## Query tabs

- **New query tab**: **Cmd+T** (or **Cmd+N**, **File > New Query**). It is bound to the connection in use; with none, the tab opens unbound and the connection chooser opens.
- Each tab keeps **its own connection**, shown on the toolbar. Switching tabs switches the target; nothing you click in the Explorer redirects a query.
- **Split the editor**: **Cmd+\\** (right) or **Shift+Cmd+\\** (down). Each group has its own tabs.
- **Switch Tab** (**Ctrl+Tab**) lists the query tabs with their connection and environment, plus the result tabs.
- Results belong to the tab that ran them: switching tabs shows that tab's last run.

## SQL files

**File > Open...** (**Cmd+O**) opens a file by what it is: a `.sql`, `.txt` or `.md` file as a tab bound to the file, a `.lrpt` as a [report](../reports/studio.md), a SQLite database file as a connection. Text files up to 8 MB open; binary files are refused. File tabs save with **Cmd+S**. A dot marks unsaved edits. When another program (or a Git checkout) changes the file, a clean tab reloads; a tab with edits offers **Compare**, **Keep mine** or **Reload** and is never overwritten.

Query tabs that are not files are kept by your [workspace](../workspace/workspaces.md).

## Completion

Completion pops up as you type identifiers or `.`, and on demand with **Ctrl+Space** (Control, not Command: on macOS **Cmd+Space** opens Spotlight). **Enter** or **Tab** accepts, **Esc** closes, **Up / Down** move. Accepting is one undo step.

What it proposes depends on where the caret is:

| After | Proposed |
|---|---|
| `alias.`, `table.` or `schema.table.` | the columns of that table only |
| `schema.` | that schema's objects (after `FROM` / `JOIN`: only what can be queried) |
| `FROM`, `JOIN`, `INTO`, `UPDATE`, `TABLE`, a comma in a `FROM` list | tables, views, materialized views and synonyms; schemas; other schemas' objects as `SCHEMA.NAME` |
| `INSERT INTO t` and a space | first, **all N columns, then VALUES (**, which inserts `(A, B, C) VALUES (`; then the single columns |
| `INSERT INTO t (` | first, every column: `A, B, C)`; then the single columns |
| `SELECT`, `WHERE`, `ON`, `SET`, `BY`, `AND`, `OR`, `HAVING`, `(` | the columns of the tables already in the statement, with nothing typed |
| anywhere else | columns of the statement's tables first, then aliases, tables, functions and keywords |
| inside a string or a comment | nothing |

- Aliases are read from the statement: `FROM t a`, `FROM t AS a`, `JOIN t a`, `UPDATE t`, `INSERT INTO t`. Without an alias, the table's name works as qualifier (`FROM sales.orders WHERE orders.`).
- The tables of the statement count wherever they are written, so **Ctrl+Space between `SELECT` and `FROM`** proposes their columns. With one table they are plain names; with several each one is written with its alias (`o.ID`, `c.NAME`), so the result is never ambiguous. Typing the column's own name finds it (`na` finds `c.NAME`).
- The **all N columns** proposal of an `INSERT` leaves out generated and identity (always) columns, which the database refuses values for; its detail previews the first columns and says how many were left out, when any.
- Keywords follow the case you type: `sel` gives `select`, `SEL` gives `SELECT`. Functions are inserted with their opening parenthesis.
- **Ranking**: the best text match always comes first (prefix, then word start, then substring). Among equal matches, what you use more often ranks higher, and after `FROM orders JOIN` the tables you have joined with `orders` before come first.
- **What it learns** stays on your computer, per connection: object names and counts only, never values or results. It learns from statements you write, statements you run and proposals you accept.

### Every object you can see

On PostgreSQL, Oracle, SQL Server and MySQL / MariaDB, completion knows every object your user is allowed to see, in every schema: tables, views, materialized views, synonyms (with their target), procedures, functions, packages, sequences and types.

- Your current schema's objects and public synonyms are proposed by name; the others as `SCHEMA.NAME`. Schema names are proposed too: pick one, type `.`, and its objects follow.
- The list is read in a **second session** that Lume opens for the connection, so the editor is never kept waiting, even when the database takes minutes to answer. Only the names are kept on disk; the next connect proposes them at once and refreshes them in the background.
- Columns of another schema's table, or of a synonym's target, are read in that session the first time they are needed. Meanwhile the popup shows **Loading columns of ...**, and it fills in by itself when they arrive (as long as you have not moved on). Expanding such a table in the Explorer works the same way.
- Turn it off in **Settings > Connections > Completion** if your database limits the sessions per user. Completion then proposes your own schema's tables only.
- Completion never queries the editor's session while you type. **View > Refresh Explorer** (**Cmd+R**) reads the structure again after you change the schema.

## JOIN conditions

Right after `JOIN <table> ON`, Lume proposes **the whole condition**:

```sql
SELECT *
FROM shipments s
JOIN order_items oi ON oi.order_id = s.order_id AND oi.line = s.item_line
--                    ^ proposed from the foreign key, both columns of the composite key
```

1. **Foreign keys first**, between the joined table and each table before it, in either direction, with the aliases of your statement and every pair of a composite key.
2. **Then same-named columns** that are a primary key on one side (`p.customer_id = o.customer_id`), labelled "same column name". Generic columns such as `created_at`, and each table's own `id`, are never proposed.

Foreign keys are read once per table, the first time a JOIN needs them.

## Ask the assistant

With [Lume Intelligence](../intelligence/overview.md) set up, **Cmd+K** in a query tab opens the assistant about that tab: describe a change in words and it comes back as a proposal you apply or reject. See [SQL assistant](../intelligence/sql-assistant.md).
