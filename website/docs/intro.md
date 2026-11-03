---
slug: /
title: Lume Data
sidebar_label: Introduction
---

# Lume Data

Lume Data is a fast, lightweight, native database IDE. It is written in C on SDL3, so it starts quickly, stays at about 0 % CPU while idle and remains responsive on catalogs with tens of thousands of tables. It works offline and sends nothing unless you turn it on.

## Databases

| Database | Connect | Browse & edit objects | Table designer | ER diagrams | Execution plans |
|---|---|---|---|---|---|
| SQLite | yes | yes | yes | yes | yes |
| PostgreSQL | yes | yes | yes | yes | yes |
| MySQL / MariaDB | yes | yes | yes | yes | yes |
| SQL Server | yes | yes | yes | yes | yes |
| Oracle | yes | yes | yes | yes | yes |

SQLite is built in; the others use the client library installed on your computer ([Install](getting-started/install.md)).

## What is in it

- **[Connections](connections/managing.md)** with passwords in your system's secret store, and **[DEV / STG / PROD environments and read-only](connections/environments.md)** that guard production.
- **[SQL editor](sql/editor.md)** with completion that knows your schema, including whole `JOIN ... ON` conditions from foreign keys.
- **[Running SQL](sql/running.md)** in the background with a real cancel, scripts, parameters and history; **[sessions and transactions](sql/transactions.md)** that never end by accident.
- **[Results grid](sql/results-grid.md)** that streams large results, with filters, sorting, charts and a value viewer; **[data editing](sql/editing-data.md)** reviewed as SQL; **[export](sql/exporting.md)** to CSV, TSV, JSON, Markdown, SQL and XLSX.
- **[Execution Output](sql/output.md)**: every run as events, with errors you can jump to.
- **[Execution plans](sql/execution-plans.md)** as graphs, with findings and index candidates.
- **[Schema Explorer](explore/schema-explorer.md)** built for servers with thousands of schemas.
- **[Object editor](explore/object-editor.md)** and **[Table Designer](explore/table-designer.md)**: every change is reviewed as SQL before it runs.
- **[ER diagrams](explore/er-diagrams.md)** of real foreign keys, for databases of any size.
- **[Report Studio](reports/studio.md)**: KPI, chart and table reports with themes, filters and HTML export.
- **[Lume Intelligence](intelligence/overview.md)**, an optional AI assistant that only sees what you consent to.
- **[Source Control](source-control/git.md)**: Git status, staging by hunk, commits, branches, pull and push.
- **[Panels and layouts](workspace/panels-and-layouts.md)**, a **[customizable toolbar](workspace/toolbar.md)**, the **[command palette](workspace/navigation.md)** and **[workspaces](workspace/workspaces.md)**.

Start with [Install](getting-started/install.md), then [Your first connection](getting-started/first-connection.md). The [keyboard shortcuts](keyboard-shortcuts.md) and [troubleshooting](troubleshooting.md) pages are there when you need them.
