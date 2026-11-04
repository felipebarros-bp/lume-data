---
title: Object editor
sidebar_position: 1
---

# Object editor

The object editor changes the definition of views, materialized views, functions, procedures, triggers, packages and types. Like every change in Lume, it goes **draft → validation → review → apply → read back**, and nothing reaches the database before you confirm the review.

## Open an object

Double-click it in the Explorer, or right-click > **Open Definition**. Each object opens in its own tab, bound to the object's connection; opening it again focuses the existing tab. The header shows the environment, the engine and the object's state.

- Oracle packages and types have a specification and a body: **Open Specification**, **Open Body** or **Open Both**, each with its own tab and status.
- System objects, encrypted or wrapped sources, and special objects (extension members, aggregates, virtual tables...) open **read-only** and say why. You can still copy and export them.
- An object tab is not a console: **Run** is refused there. **Open in Query Console** runs a copy.

The right-click menu of an object type group in the Explorer (**Tables**, **Functions**, **Procedures**...) offers **New from template** (view, function, procedure, trigger, package) in the database's dialect, and **Refresh Objects**.

## Edit

- Highlighting follows the database's own rules (dollar-quoted bodies, `q'[...]'` strings, `GO`, `#` comments...).
- **Format ▾** > **Format Document** / **Format Selection** (a selection grows to whole statements). Strings, comments and dynamic SQL are never touched, and procedural blocks are left as written. The rules are in Settings > Editor.
- Completion also offers the routine's parameters and declared variables with their types. Typing `name(` shows each overload's arguments, the current one in `[ ]`, the return type and the comment.
- **F12** (**More > Go to Definition**) opens the object under the caret.
- Refactorings, each with a preview and as one undo step: **Rename Symbol...**, **Extract Subquery to CTE...**, **Use Parameter Names ($n)** (PostgreSQL), **Qualify Columns...** (only where exactly one table has the column).

## Problems and validation

- Problems are checked as you type and listed in the **Problems** pane: lexical errors, a `CREATE` header that does not match this object, unbalanced blocks, unknown tables, and inspections such as `SELECT *` in a view, `UPDATE` / `DELETE` without `WHERE`, or a parameter that shadows a column. Each says where it came from and how sure it is. **F8** / **Shift+F8** go to the next / previous one. Some come with a quick fix, shown as before / after.
- **Validate ▾** > **Validate (local)** checks without contacting the server ("Syntax OK (local)"). **Validate on Server (rolled back)** compiles it on the server and rolls back: a view is compiled as a temporary view and its query never runs.
- Dynamic SQL (`EXECUTE`, `EXECUTE IMMEDIATE`...) is reported as not analysed.

## Drafts and diff

- Your edits are saved as a local **draft** every few seconds and when you quit, and restored when you reopen the object. **Save Draft** never touches the database; **Discard Draft** drops it.
- **Diff** compares the original with your version side by side (with "Ignore whitespace" for viewing). **Original source** shows the definition as the server returned it.
- **More > Compare With Database...** compares your version with the server now, or with the same object on another open connection of the same engine.

## Apply

**Apply...** opens the review:

- **Generated SQL**: exactly the statements that will run, in order.
- The strategy (replace in place, or an explicit drop and re-create), whether it is atomic, the risk, and the **Dependencies** that could break.
- On PROD, you type the object's name to confirm.

Lume checks that the definition on the server has not changed since you opened it, right before running. If it did, a three-way view offers **Keep mine (review against the server)** or **Use the server version**.

After applying, Lume reads the definition back. On Oracle it also reads the status: a body that compiles with errors shows **INVALID** with the error on its line in Problems, and is never reported as success. Your edit is kept after any failure. If the connection drops mid-apply the outcome is shown as **unknown**, nothing is retried, and **Reconcile With Server...** compares again.

Engine notes:

- **PostgreSQL**: `CREATE OR REPLACE` in one transaction when allowed. A view must keep its columns; a function keeps its identity and return type. Otherwise an explicit drop and re-create is offered, with your consent, blocked by dependents, never `CASCADE`, restoring grants, owner, comment and indexes.
- **Oracle**: `CREATE OR REPLACE`; each DDL commits on its own. **Compile...** and **Show Compilation Errors** are available.
- **SQL Server**: `CREATE OR ALTER`.
- **MySQL**: a routine body change is a drop and re-create, with warnings (definer, grants, a short window without the routine) and your consent. **MariaDB** replaces routines and triggers in place.
- **SQLite**: drop and re-create inside a transaction; a broken view is rolled back.

Other operations, each reviewed the same way: **Enable Trigger...** / **Disable Trigger...**, **Refresh Materialized View Data...**, **Compile...** (Oracle).

## More

- **Review All Changes on This Connection...** applies drafts of several objects in order, stopping at the first failure (not on PROD).
- **Export Definition...** and **Generate Deployment SQL...** write a script (with `/`, `GO` or `DELIMITER` where the database needs them).
- **Submit for Review...** writes a review file with the diff, the deployment SQL, the target and a checksum, for approval outside Lume.
- Before each apply, the previous definition is saved as a backup script in Lume's preferences folder, and an audit line records the change (without SQL text or values).
- [Lume Intelligence](../intelligence/sql-assistant.md#objects-and-tables) can explain the object and **Document** it (comments for the object and its columns, as SQL in a new tab).
