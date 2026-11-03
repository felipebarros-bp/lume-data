---
title: A five-minute tour
sidebar_position: 3
---

# A five-minute tour

Shortcuts are written for macOS. On Windows and Linux, **Cmd** is **Ctrl** and **Option** is **Alt**.

![The SQL editor with results](/img/screens/query-results.png)

## The window

- **Explorer** (left, **Cmd+1**): your connections and, under each, the database's schemas and objects.
- **Query tabs** (centre): each tab is bound to its own connection, shown on the toolbar.
- **Results** (bottom, **Cmd+2**) and **Output** (**Cmd+3**): the rows of the last run and what happened during it.
- **Toolbar**: workspace, new query, the tab's connection, schema and transaction mode, **Run** and **Stop**.

## Try it

1. **Run SQL**: type a statement and press **Cmd+Return** to run the statement under the caret. **Shift+Cmd+Return** runs the whole tab.
2. **Completion**: after `JOIN orders o ON`, Lume proposes the join conditions from the foreign keys, every column of a composite key included.
3. **Results**: click a column header to sort, right-click a cell for filters, copy formats and charts.
4. **Explorer**: double-click a table to preview its rows; right-click it for **Edit Data**, **Modify Table...** and **Show ER Diagram**.
5. **Explain**: **Option+Cmd+E** shows the execution plan of the statement under the caret, without running it.
6. **Command palette**: **Shift+Cmd+P** finds any action by name, with its shortcut.
7. **Settings**: **Cmd+,**.

## Where to go next

- [SQL editor and completion](../sql/editor.md)
- [Running SQL](../sql/running.md)
- [Results grid](../sql/results-grid.md)
- [Keyboard shortcuts](../keyboard-shortcuts.md)
