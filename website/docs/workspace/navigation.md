---
title: Command palette and Quick Open
---

# Command palette and Quick Open

## Command palette

**Shift+Cmd+P** (**Window > Command Palette...**, or **Search** on the toolbar) finds any action by name: every menu item, panel, toolbar command and the commands of each feature (Diagram, Object, Git, Lume Intelligence...).

- It also matches synonyms ("execute" finds **Run**) and spelled shortcuts ("cmd enter").
- Each entry shows its shortcut. Commands that cannot run right now stay listed, dimmed, with the reason.
- **Up / Down** move, **Enter** runs, **Esc** closes.

Some commands live only in the palette, for example **Open File...**, **Save File**, **Show Source Control**, **Git: Commit Staged**, **Git: Pull**, **Git: Push**, and the **Object:** and **Diagram:** commands for the active tab.

## Quick Open

**Cmd+P** (**File > Quick Open...**) finds tables and views, schemas and connections in the catalogs already loaded, without a query per keystroke. Choosing a table opens a query tab for it, without running anything. It does not search files on disk.

## Switch Tab

**Ctrl+Tab** (**Window > Switch Tab...**) lists the open query tabs, with their connection and environment, and the result tabs (pinned ones marked). The editor keeps each tab's caret, selection and connection.

## Custom shortcuts

The panel and window shortcuts can be changed in `keymap.txt` in Lume's preferences folder (Settings > General shows where), one `name=Chord` per line:

```
palette=Cmd+Shift+P
focus_mode=Cmd+Shift+K
split_right=Cmd+Backslash
```

Names: `palette`, `explorer`, `results`, `output`, `inspector`, `focus_mode`, `maximize_panel`, `collapse_panel`, `hide_panel`, `undo_layout`, `grow_panel`, `shrink_panel`, `last_panel`, `new_tab`, `split_right`, `split_down`, `panels`. Modifiers: `Cmd`, `Ctrl`, `Alt`, `Shift`. When two names share a shortcut, the first wins and the log says so. There is no settings screen for shortcuts yet.
