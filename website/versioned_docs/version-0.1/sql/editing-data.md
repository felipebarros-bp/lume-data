---
title: Editing data
---

# Editing data

Rows are edited in the results grid of a **table preview**, and nothing reaches the database until you review the generated SQL and apply it.

## When a result is editable

Open the table with **Edit Data** (right-click a table in the Explorer) or double-click it to preview. The preview is editable when:

- the table has a primary key, and every key column is in the result;
- the connection is open and not [read-only](../connections/environments.md).

Query results, views and derived tabs (charts, compare) stay read-only; the cell menu says why ("query results are read-only; open a table preview to edit", "no primary key", "views are read-only"...). Generated, identity and binary columns are read-only too.

## Make changes

| To | Do |
|---|---|
| Edit a cell | double-click it, press **Enter** or **F2**, or just start typing (the first character replaces the value). **Tab** confirms and moves on, **Esc** cancels |
| Set NULL | cell menu > **Set NULL** |
| Use the default | cell menu > **Set DEFAULT (new rows only)** |
| Add a row | **Add Row**: every column starts as DEFAULT until you type |
| Duplicate a row | **Duplicate Row (keys take DEFAULT)** |
| Delete a row | **Delete Row** or **Cmd+Delete**; **Undelete Row** brings it back |
| Undo a cell | **Revert Cell** |

Edited cells are marked yellow, new rows green, deleted rows red and struck through. Numbers and booleans are checked before anything is sent. The tab and the toolbar banner say "N unsaved changes, not in the database yet".

## Review and apply

**Review and apply...** lists every statement Lume will run (UPDATE, INSERT, DELETE, colour-coded, with before → after), then:

- **Apply** runs them **in one transaction** and commits only if every UPDATE and DELETE touched exactly one row.
- **Discard all** drops the pending changes. **Close** keeps them.

Each UPDATE and DELETE finds the row by its primary key **and** the old values of the changed columns. If someone else changed or deleted the row meanwhile, the statement matches nothing, the whole batch rolls back and Lume tells you which row changed. A few column kinds cannot be compared exactly (binary, floating point on some databases); the review lists them.

If the connection drops right after COMMIT was sent, Lume reports the result as **unknown** and keeps your changes pending, rather than claiming success.

## Closing with unsaved changes

Closing a tab with pending changes asks first: **Review changes**, **Keep open** or **Discard and close**. **Close Other Tabs** and **Close All Tabs** skip such tabs. Exporting is refused while changes are pending, and fetching more rows while new rows are pending.
