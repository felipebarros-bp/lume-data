---
title: SQL assistant
---

# SQL assistant

In a query tab, the [Lume Intelligence](overview.md) panel works on that tab's SQL and connection. It writes in your database's dialect, using only tables and columns that exist.

## Ask

- **Cmd+K** in a query tab opens the assistant with the question box focused ("Ask about this query..."). **Enter** sends.
- The first time on a connection, a consent card asks whether to **Share the structure of** that connection. Without it, the assistant can still help with the SQL in front of it, and says when it needs the structure.
- The panel is a chat: earlier turns are part of the context. **New chat** starts over.

## Answers

The assistant answers with one of these:

- **A change to your SQL** (the selection, the statement or the whole tab), shown as a preview. **Apply to editor** replaces the text as one undo step. It is refused if the tab changed since you asked.
- **A new query**: **Open in new tab**.
- **An explanation**, or **a question for you** when the request is ambiguous.

**Lume never runs what the assistant writes in a query tab.** SQL that changes data is flagged in the preview. You read it, apply it, and run it yourself like any other statement, with the usual [PROD and read-only protections](../connections/environments.md).

## Fix an error

When a statement fails:

- **Fix with AI** next to the error in the [Output](../sql/output.md), or
- the command **Fix the Last Error with Lume Intelligence**.

It sends the error and the SQL, never rows, and proposes a corrected statement.

## Explain

**Explain This Query with Lume Intelligence** (or **Explain** in the panel) explains the query or the selection, including its risks: an `UPDATE` or `DELETE` without `WHERE`, a cartesian product, a comparison with `= NULL`, a conversion that prevents an index from being used.

## More things to ask

- **Test data**: "generate 20 test orders" comes back as `INSERT` statements with fake values, in a new tab, flagged as changing data and never run.
- **About a result**: "summarise this result" or "find outliers" uses a summary computed locally (rows, nulls, distinct values, min / max), sent only with its own consent.
- **Rewrite for performance**: with a plan open, see [Execution plans](../sql/execution-plans.md#lume-intelligence).

## Objects and tables

- **Design a table**: "create an orders table with customer, date and total" comes back as a draft in the [Table Designer](../explore/table-designer.md) (**Open in Table Designer**): columns, primary key, foreign keys and indexes, names and types checked first. Nothing is created until you review and apply it there.
- **Review Table with Lume Intelligence** (Table Designer menu: **Review with Lume Intelligence**) opens the table's DDL in a query tab and asks for a review: a foreign key without an index, poor types, inconsistent names.
- **Document This Object with Lume Intelligence** (or **Document** in an object tab) proposes comments for the object and its columns, as SQL in a new tab for you to review and run.
- In an object tab, explaining and editing work on the object's definition like on any query.
