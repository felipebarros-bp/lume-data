---
title: ER diagrams
sidebar_position: 1
---

# ER diagrams

An ER diagram in Lume shows **a part of your database**: the tables you are working with and the foreign keys between them. It is built for large databases: the whole catalog stays searchable, but only what you choose is drawn.

## Open a diagram

| From | What opens |
|---|---|
| Right-click a table > **Show ER Diagram** | that table and the tables linked to it by one foreign key |
| Select several tables (**Cmd+click** each, or **Shift+click** a range), right-click > **Show ER Diagram (N tables)** | those tables, laid out together |
| Right-click a connection, its **Tables** group or a schema > **New ER Diagram...** | a picker: the most connected tables, the ones you pick, an empty diagram, or the whole schema when it is small |
| **Tools > New ER Diagram...** / **Open ER Diagram...** | the same, for the active connection / a saved diagram |

Lume reads the structure in a few bulk queries (names, types, keys, comments: **never rows**), caches it, and never runs anything else on the connection. Diagrams are read-only: changing a table goes through the Table Designer, which shows the SQL before it runs.

## What you see

- **Cards**: a key icon marks the primary key, **FK** a foreign key column; nullable columns are dimmer. **Keys / Name / All** on the toolbar chooses how much each card shows (per card too: right-click > Card).
- **Lines**: a solid line is a foreign key in the database. Crow's foot = *many*, bar = *one*, circle = *optional* (the column accepts NULL). Cardinality comes only from the database's keys; when it cannot be known, the end is neutral.
- **+N** on a card: N related tables are not on the diagram yet. Click it to add them.
- A dashed card is a table **outside the diagram's scope** (another schema, or one your login cannot read): a reference, not a missing relation.

## Find

**Cmd+F** searches the **whole scope**, not only the cards drawn: tables, columns, constraints (foreign key names) and notes, with Contains / Prefix / Exact / Glob and Match case.

- **Enter / Shift+Enter** go to the next / previous match and centre it.
- Going to a table that is **not on the diagram yet adds exactly that table**.
- `from:orders to:customers` finds the relations between two tables.

## Move around

- **Pan**: drag the background, or scroll with two fingers.
- **Zoom**: pinch on the trackpad, Cmd+scroll, double-click the background (Shift+double-click zooms out), or **– / 100% / +** on the toolbar.
- **Fit** (Cmd+1) frames everything; **Fit Selection** (Cmd+2) the selection.
- **Arrow keys** move between cards; **Cmd+[ / Cmd+]** go back / forward through the places you visited.
- Far away, cards become boxes with their names; hover one to read it. **More > View > Minimap** shows where you are.

## Organise

- **Layout** arranges the cards around the table you started from (deterministic: the same diagram always lays out the same way). Drag cards to place them; **Pin** keeps one in place. **Cmd+Z** undoes a move.
- **Notes** and **group areas** (More > Add Note / Add Group Area) document the diagram.
- **Virtual relations**: right-click a card > **Draw Virtual Relation...**, then click the other card. They are dashed, labelled "virtual", stored only in the diagram, and can be marked as verified with their evidence. They never create a foreign key.
- **Business names**: right-click > Business Name... and the **Business names** toggle show the names your business uses, with the table name beside them.
- **Logical entities** (More > Add Logical Entity) sketch a concept that is not a table yet.

The diagram is saved automatically in your workspace's `diagrams` folder (a readable `.lume-erd` file you can keep in Git). When a table changed since the diagram was saved, its card says **changed**.

## Export

**Export...** on the toolbar: PNG, SVG, PDF, Mermaid, GraphML, draw.io or a self-contained HTML page (search, pan and zoom, no external requests), for the selection, the visible part or the whole diagram. **Copy as Image** puts the selection on the clipboard.

## Lume Intelligence

More > Lume Intelligence can **explain** the selected tables, **suggest domain groups** or **suggest a conceptual map**. Nothing is sent before a dialog shows exactly what would go (structure only: names, types, keys, comments; never rows). Suggestions are proposals you accept or reject one by one; a suggested relation is drawn dotted and never becomes a foreign key.
