---
title: Execution Output
---

# Execution Output

The **Output** panel (**Cmd+3**, or **Query > Show Execution Output**, **Shift+Cmd+O**) records what happened during every run on the active tab's connection: what started, rows returned or affected, server notices, errors, transaction changes, cancels and timings. It sits next to Results and is never replaced by a new run.

## Reading it

- Each execution is a **group** with its time, status, statement counts and duration ("failed · 2 of 4 ok · 1 failed · 1 skipped"). Click it to fold or unfold; **Expand all** / **Collapse all** in the menu.
- Each event has a time, a level mark and word (STARTED, SUCCESS, AFFECTED ROWS, RESULT SET, NOTICE, WARNING, ERROR, TRANSACTION, CANCELLED, NOT RUN, FINISHED), the message and, on the right, its duration and error code.
- States are written in words, not only colours. A cancelled execution is never shown as success.
- While something runs, a banner shows the elapsed time and **Cancel**.

## Errors

An error shows the server's message and, when the database provides them, its code (SQLSTATE on PostgreSQL, ORA-nnnnn on Oracle, the extended code on SQLite), detail, hint and line / column. Next to it:

- **Go to SQL** selects the statement in the editor. If you edited the tab since, Lume finds the same text where it is now ("Go to SQL (changed)"), or shows the SQL as it ran in the event details. It never runs anything again.
- **Open results** jumps to the result tab of a statement.
- **Fix with AI** sends the error and the SQL (never rows) to [Lume Intelligence](../intelligence/sql-assistant.md#fix-an-error), when it is set up.

By default the Output comes forward when a statement fails. Turn this off with **Open Output on statement errors** in the Output's menu, or in Settings > Results & Format. Warnings never steal the focus: they only add a badge to the Output's tab ("Output · 1 warning").

## Toolbar

| Control | Does |
|---|---|
| Search (**Cmd+F**) | searches messages, SQL, codes and details; **Enter** / **Shift+Enter** (or **Cmd+G** / **Shift+Cmd+G**, **F3**) step through matches; the field shows "3 of 25" |
| Levels | show Info / Success / Warning / Error. Hidden events are never dropped; the footer says how many are hidden |
| **All executions** | limit the view to one execution |
| **Wrap** | wrap long messages |
| **Follow tail** | keep the newest event in view. Scrolling up pauses it |
| **Copy** | the selected rows, in order, with secrets masked |
| **Export...** | the log as text or JSON |
| **Clear output** | clears the view only (the history is kept) |

Select rows with **Shift+click**, **Shift+arrows** or **Cmd+A**. **Cmd+C** copies the messages, **Shift+Cmd+C** their details. In the text view you can also drag to select, double-click a word or triple-click a line.

The Output's menu also has **Pause view** / **Resume view** (freezes the list while events keep being recorded), **Clear filters** and **Reset Output layout**.

## Privacy

Password-like literals and `key=value` secrets are masked in everything you copy or export from the Output and in the history. Connection strings never reach the log.

## Limits

The Output keeps the last 20,000 events per connection; the oldest executions go first (never the running one) and the footer says so. Events live in memory for the session; the [execution history](running.md#history) is what persists.
