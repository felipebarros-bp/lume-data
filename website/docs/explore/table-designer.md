---
title: Table Designer
sidebar_position: 2
---

# Table Designer

The Table Designer edits a table's **structure** (columns, keys, checks, indexes). Rows are edited in the result grid. Nothing touches the database until you choose **Review & Apply**:

**draft → validation → SQL preview → review → apply → read back**

- Open it with right-click on a table > **Modify Table...**, or **New > Table...** on a schema.
- Right-click a group (**Columns**, **Primary Key**, **Unique Keys**, **Foreign Keys**, **Checks**, **Indexes**) to add one; right-click an item to move, duplicate or remove it.
- The **SQL preview** at the bottom always shows the exact statements that would run, in order, with a warning on each step that may lose data, lock the table or rewrite it.
- **Review & Apply** shows the plan, the risk and what can be rolled back, then runs it. Lume claims a rollback only when the server confirms it, and reads the table back afterwards.
- Unfinished work is kept as a **draft**; **Save as Migration** writes the SQL to a file without running it.
- On SQLite, changes SQLite cannot do in place are made by rebuilding the table, only after you agree.
