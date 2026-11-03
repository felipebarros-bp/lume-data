---
title: Workspaces
---

# Workspaces

A workspace keeps a set of work together: **its open query tabs** (their SQL and connection) and **its toolbar**. Use one per project or client database set. Lume starts in the default workspace, "Lume Data".

The **Workspace** menu on the toolbar, or **File > Workspace**:

- **New Workspace...**: give it a name; it starts with the default toolbar.
- **Open Workspace...**: switch to another one (recent ones first).
- **Workspace Settings...**: shows the workspace's folder and what it keeps.
- **Close Workspace**: back to the default workspace (which itself stays open).

Switching or closing with query tabs changed since the workspace was saved asks **Save and close**, **Discard and close** or **Cancel**.

## What a workspace keeps

| Kept per workspace | Global (every workspace) |
|---|---|
| the open query tabs and their connection | connections |
| the toolbar settings | panel layouts |
| ER diagrams (its `diagrams` folder) | settings |
| the report Brand Kit theme and the assistant's glossary, when you create them there | query history |

A workspace never keeps passwords, connection strings or parameter values. Workspaces are listed by name only.
