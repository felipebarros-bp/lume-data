---
title: Panels and layouts
---

# Panels and layouts

Everything around the editor is a **panel** you can show, hide, move, group, split or take out into its own window. Moving a panel never reloads it: a running query, a result or a scroll position stays as it was.

## The panels

| Panel | Show it |
|---|---|
| Explorer | **Cmd+1** (**Cmd+B** shows / hides) |
| Results | **Cmd+2** (**Cmd+J** shows / hides) |
| Output | **Cmd+3** |
| Inspector (Record View) | **Cmd+4** |
| Query History | **Window > Show Panel > Query History** |
| Lume Intelligence, Source Control, Execution Plan | **Window > Show Panel** |

**Cmd+1** to **Cmd+4** open and focus a panel; pressed again while it has the focus, they hide it. **F12** (**Back to Last Panel / Editor**) returns to where you were.

**Window > Panels...** (**Option+Cmd+P**) lists every panel with its state: visible, hidden, collapsed, auto-hide, floating or separate window.

After a run, Results comes forward on its own (shown, unfolded and selected), and a failure brings the Output forward, while the keyboard stays in the editor.

## Arrange panels

- **Drag a panel's tab** to another place: a preview shows where it lands. Drop on a group's tab row to join it as a tab, on a side to split. **Esc** cancels.
- **Right-click a panel's tab** (or its header's menu) for **Hide (content kept)**, **Move to**, **Split** (**Side by side, on the left / right**, **Stacked, above / below**), **Mode**, **Maximize**, **Reset panel size**.
- **Mode**: **Docked, pinned** (default), **Auto-hide overlay** (shows over the content when you open it, then tucks away), **Floating window, above Lume**, **Separate window, any monitor**. Closing a floating or separate window docks the panel back. A window left on a disconnected display is moved back on screen.
- Drag the **splitters** between panels to resize. **Grow** / **Shrink** (**Ctrl+Option+Right / Left**) resize the focused panel from the keyboard.
- **Collapse / Expand** (**Option+Cmd+J**) folds a group to the edge, keeping its content and size. **Hide** (**Shift+Esc**) hides it.
- **Maximize / Restore** (**Option+Cmd+M**) gives the panel the whole window for a moment, without changing the saved layout.
- **Window > Panel > Allow Top Edge** lets panels dock at the top too.

The **panel bar** on the window's edge holds quick buttons for panels, with badges (tab counts, unsaved rows, errors). Right-click a button for **Remove from panel bar**; **More panels** lists the rest, with **Add to panel bar**.

## Editor groups

**Window > Editor > Split Editor Right** (**Cmd+\\**) or **Split Editor Down** (**Shift+Cmd+\\**) puts query tabs side by side, each with its own connection. Dragging an editor tab moves it between groups.

## Focus mode

**Focus Mode** (**Shift+Cmd+F12**) hides every panel and leaves the editor; pressing it again restores them where they were.

## Layouts

The layout is saved automatically and restored at the next launch. A layout file that cannot be read is set aside and the default layout is used.

**Window > Layout**:

- **Default (Reset)**, **Query Focus**, **Data Exploration**: ready-made arrangements.
- **Save Layout As...**: name the current arrangement.
- **Manage Layouts...**: restore, rename, replace or delete saved layouts, and bind one to a connection so it is used whenever you work on it.
- **Export Layout...** / **Import Layout...**: share an arrangement as a file. It holds no SQL, data or credentials; importing asks first.
- **Undo Layout Change** (**Option+Shift+Cmd+Z**) undoes the last move. Editor text and data are not affected.

## Appearance

**View > Compact Panel Headers** makes headers smaller for small screens. **View > Reduce Motion** turns off smooth scrolling and animation (Lume also follows the system setting on macOS). Both are in Settings > Appearance.
