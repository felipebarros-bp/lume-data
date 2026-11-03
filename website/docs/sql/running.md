---
title: Running SQL
---

# Running SQL

Every way of running SQL (the toolbar's **Run**, the **Query** menu, the command palette, the shortcuts) goes through the same path and runs on **the active tab's connection**. Statements run in the background: the window stays responsive and you can cancel.

## Run commands

| Command | Shortcut | Runs |
|---|---|---|
| **Run Statement** | **Cmd+Return** | the selection, or else the statement under the caret |
| **Run Selection** | | exactly the selected text; refused without a selection |
| **Run All** | **Shift+Cmd+Return** | every statement in the tab ("Run All in File (n statements)" on the toolbar) |
| **Run in New Tab** | **Option+Cmd+Return** | the statement, into a new result tab instead of replacing the last one |
| **Run Special > With Parameters...** | | the statement after you fill in its parameters |
| **Run Special > To File...** | | the statement, writing its first result straight to a file |
| **Run Special > Re-run Last...** | | the SQL exactly as it last ran on this connection (not the editor's current text) |
| **Cancel Execution** | **Cmd+.** | stops what this tab is running |

- A selection that cuts a statement in the middle asks "Run a partial selection?" and offers **Run exactly the selection**; Lume never widens it.
- While a statement runs, **Run** shows the elapsed time ("Running 00:03"). Another statement on the same connection waits until you stop the first.
- The Output records what ran: lines, connection, driver and schema.

## Scripts

**Run All** splits the tab into statements and runs them one by one. It understands PostgreSQL dollar-quoted bodies and Oracle PL/SQL blocks ended by a `/` on its own line.

- By default the script **stops at the first error**; the remaining statements are marked *not run*. **Query > Execution > Continue on Error (scripts)** (or Settings > Query Execution) keeps going.
- Statements that return rows open result tabs; `INSERT`, `UPDATE`, `DELETE` and DDL report their row counts in the [Output](output.md).
- On a PROD connection, a script of several statements is confirmed first ([Environments](../connections/environments.md)).

## Cancel

**Cmd+.**, **Stop** on the toolbar, or **Cancel** in the Output's running banner sends a real cancel to the server. It stops **only this tab's session**: statements running on other connections continue. When several executions run, **Stop** shows the count and its menu lists each one.

After a cancel the Output says *cancelled*, never success, and a partial result is labelled. A database that cannot interrupt a statement only stops the remaining ones, and the Output says so.

## Parameters

**Run Special > With Parameters...** finds `:name`, `$1` and `?` placeholders (outside strings, comments and `::` casts) and asks for their values in a dialog.

```sql
SELECT * FROM orders WHERE customer_id = :customer AND status = :status;
```

Values are sent as quoted literals. They are never shown in the Output and never kept in the history: only the statement with its placeholders is.

## Execute to File

**Run Special > To File...** asks for a file, runs the statement and writes its first result there. The format follows the extension you choose (`.csv`, `.xlsx`...). On PROD, SQL that may write is confirmed first.

## Fetch size and memory

A result first loads one batch of rows (500 by default). **Query > Results > Fetch Size** chooses 100, 500, 1,000 or 5,000; Settings > Query Execution accepts any value. **Query > Results > Memory Limit** caps the memory all result tabs may use together (64 MB, 256 MB or 1 GB; Settings also offers 4 GB): over it, the oldest unpinned tab gives its rows back and keeps a **Re-run** button. See [Results grid](results-grid.md).

**Query > Execution > Execution Settings...** sets the fetch size and continue-on-error in one place.

## History

- **Query > Execution History...** (**Cmd+Y**) lists this session's executions and earlier sessions (time, connection, status, a snippet). Choosing one only puts its SQL in the editor; nothing runs. When the statement changes data or had secrets masked, Lume says so first.
- The **Query History** panel (**Window > Show Panel > Query History**) filters by text, connection, status (OK / failed / cancelled) and time (last hour / today), with **Open**, **Copy** and **Re-run**. Re-run goes through the same checks as Run.

History keeps the SQL with password-like literals masked, never result values. Settings > Results & Format sets how many entries are kept (500 by default).
