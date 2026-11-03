---
title: Toolbar
---

# Toolbar

The main toolbar sits under the menu bar. Everything on it acts on **the active query tab** and its connection.

From left to right:

| Group | Items |
|---|---|
| Workspace | the **Workspace** menu (New, Open, recent workspaces, Settings, Close) and **New query tab** |
| Connection | the **datasource** button (the tab's connection: name, type, state, environment pill, **Read-only** pill), **Schema**, **Tx** (transaction mode) |
| Run | **Run** (shows "Running 00:03" while busy) and **Stop** (with a count when several executions run; its menu lists them) |
| Right side | **Search** (opens the command palette), **Panels**, and **More** (actions that do not fit, and the toolbar settings) |

Hover any item for its name, shortcut, what it acts on and, when disabled, why. A disabled command explains itself the same way in the menus and the palette.

The datasource button's menu offers **Connect**, **Reconnect**, **Disconnect**, **Cancel Connect**, the last failure, **Session Details...** and the connection chooser (search, recent first, grouped by type). See [Sessions, schema and transactions](../sql/transactions.md).

## Customize

Right-click the toolbar (or **View > Toolbar**):

- **Customize...**: show or hide each item, move it up or down within its group, add or remove actions, or pick a preset: **Minimal**, **Default**, **Advanced** (**Custom** once you change it). The bar behind is a live preview; **Cancel** restores, **Save** keeps. The datasource, **Run**, **Stop** and **More** cannot be removed.
- **Add Action...**: put any command on the bar.
- **Hide Toolbar** / **View > Toolbar > Main Toolbar**: with the toolbar hidden, every command is still in the menus, the palette and its shortcut.
- **Compact Mode** and **Show Labels**.
- **Reset to Default**: resets the toolbar only, never connections, queries or panel layouts.
- **Export Settings**: writes the toolbar settings to a file (no SQL, connection data or secrets), useful when reporting a problem.

Each [workspace](workspaces.md) keeps its own toolbar.

When the window is narrow, labels go first, then the datasource name shortens, then the least important items move into **More**. The datasource, **Run**, **Stop** and **More** always stay.

## Keyboard

**Ctrl+F5** (**View > Toolbar > Focus Toolbar**) moves the keyboard to the toolbar: **Tab** / **Shift+Tab** and arrows move, **Enter** or **Space** activates, **Down** opens a menu, **Esc** returns to where you were.
