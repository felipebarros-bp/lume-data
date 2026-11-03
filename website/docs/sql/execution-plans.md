---
title: Execution plans
sidebar_position: 2
---

# Execution plans

**Query > Explain Plan** shows how the database intends to run a statement, as a graph next to Results: each operator with its estimated rows and cost, the most expensive ones highlighted, plus the operators as a table, the raw plan and findings (such as a missing index candidate).

- **An estimated plan never runs the statement.**
- **Explain Analyze** (the actual plan) runs it, so Lume asks first. On PostgreSQL it runs inside `BEGIN ... ROLLBACK`; writes are refused where nothing could roll them back, and always on PROD or read-only connections.
- Plans of the same statement can be compared side by side.
- With Lume Intelligence configured, **Analyze** explains the plan. What is sent is shown first: by default literals are masked and names aliased; SQL, names and the raw plan are opt-in. Its suggestions open as a copy labelled UNVERIFIED.

Available for SQLite, PostgreSQL, MySQL, MariaDB, SQL Server and Oracle.
