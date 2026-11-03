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

Completion pops up as you type identifiers or `.`, and on demand with **Ctrl+Space**. **Enter** or **Tab** accepts, **Esc** closes, **Up / Down** move. Accepting is one undo step.

What it proposes depends on where the caret is:

| After | Proposed |
|---|---|
| `alias.` or `table.` | the columns of that table only |
| `FROM`, `JOIN`, `INTO`, `UPDATE`, `TABLE`, a comma in a `FROM` list | tables and views |
| `SELECT`, `WHERE`, `ON`, `SET`, `BY`, `AND`, `OR`, `HAVING`, `(` | the columns of the tables already in the statement, with nothing typed |
| anywhere else | columns of the statement's tables first, then aliases, tables, functions and keywords |
| inside a string or a comment | nothing |

- Aliases are read from the statement: `FROM t a`, `FROM t AS a`, `JOIN t a`, `UPDATE t`, `INSERT INTO t`.
- Keywords follow the case you type: `sel` gives `select`, `SEL` gives `SELECT`. Functions are inserted with their opening parenthesis.
- **Ranking**: the best text match always comes first (prefix, then word start, then substring). Among equal matches, what you use more often ranks higher, and after `FROM orders JOIN` the tables you have joined with `orders` before come first.
- **What it learns** stays on your computer, per connection: object names and counts only, never values or results. It learns from statements you write, statements you run and proposals you accept.
- Completion never queries the database while you type: it uses the structure Lume has already read. **View > Refresh Explorer** (**Cmd+R**) reads it again after you change the schema.

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
