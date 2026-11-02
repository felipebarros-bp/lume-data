# Lume

A lightweight SQL editor written in C. One window, three panels, no runtime, no Electron.

- **Small**: ~150 KB binary, ~6 MB of memory on top of the window itself, 0% CPU when idle
  (the loop blocks on events and only redraws when something changes).
- **Modern UI**: everything is custom drawn on the GPU through SDL3 (Metal / Vulkan / D3D),
  HiDPI aware, with Inter for the UI and JetBrains Mono for SQL.
- **Portable**: macOS, Linux and Windows through SDL3. The only dependencies are SDL3 and SQLite.

## Features (0.1)

- Connections (`docs/connections.md`): saved data sources with a driver and
  the fields that driver declares. SQLite (built in), PostgreSQL (`libpq` loaded at runtime)
  and Oracle (OCI loaded at runtime; validated by `tests/oracle_check.sh` against Oracle Free in Docker). Create, test, edit,
  connect, disconnect and delete from a dialog or the explorer's context menu; passwords go
  to a separate 0600 file. Adding a database is one driver descriptor plus one `DbDriver`.
- Schema explorer: connections at the top; tables, views, columns with types and primary keys,
  grouped by schema when the database has several. Double-click a table to preview it,
  double-click a column to insert its name in the editor. Big schemas list names first and
  load columns on demand.
- SQL editor with syntax highlighting, selection, clipboard, word movement, and
  *run statement under cursor* (`⌘↩` / `Ctrl+Enter`). The statement that will run is marked
  in the gutter. `⇧⌘↩` runs the whole buffer.
- Autocomplete with the rules in `docs/autocomplete-rules.md`: it reads only
  the in-memory schema cache (never the database while typing), resolves aliases (`c.` →
  columns of `customers`), suggests tables after `FROM`/`JOIN`, and ranks by text match first,
  with per-connection usage (frecency, tables used together, token n-grams) only breaking
  ties. Opens while typing or with `⌃Space`.
- Undo/redo (`⌘Z` / `⇧⌘Z`) with typing grouped into words.
- Result grid (`docs/result-grid.md`, built from the user stories in that document): batches
  with a real cursor (fetch size 100–5,000, "Fetch more" / "Fetch all"; never an invented
  total), types from driver metadata, virtualised rows and columns, selection of cells /
  rows / columns / rectangles with aggregates in the footer, copy as TSV / CSV / JSON /
  Markdown / SQL INSERT, ⌘F search, local filters per column (chips), multi-column sort with
  priorities, value viewer, autofit, export to CSV / TSV / JSON / Markdown / SQL with a
  formula-injection guard. Every local operation says so ("sorted locally", "N of M rows").
- Result tabs: one per execution with id, time and connection (pin, rename, close, re-run,
  open the SQL in the editor, ⌘Y history), a memory budget across tabs (64 MB / 256 MB /
  1 GB; the oldest unpinned tab is discarded with a notice), stale marking when the
  connection closes.
- Column layout: hide, drag headers to reorder, freeze leading columns, wrap text, Columns…
  panel (⇧⌘C), reset; remembered per connection and column signature.
- Server-side sort and filter for table previews (header menu), with quoted identifiers and
  escaped literals; the grid says "sorted on server" / "filtered on server".
- Display formats (Format menu): thousands separator, fixed decimals, DD/MM/YYYY or
  MM/DD/YYYY, timestamps with an offset shown in UTC or local time. Display only: exports
  stay raw unless "Displayed values" is chosen.
- Value viewer with a JSON tree (fold/unfold, lazy for big documents, JSON null vs SQL NULL).
- Export dialog: delimiter, quotes, line ending, NULL policy, header, raw vs displayed values,
  XLSX (native writer, inline strings so no cell becomes a formula), custom extractors from
  `extractors.txt`, and "Full result" streamed through the open cursor block by block with
  row / size / time limits (Esc cancels).
- Table editing on previews with a primary key: edit in place, Set NULL / DEFAULT, add,
  duplicate and delete rows; pending changes stay local (yellow / green / red) until "Review
  and apply" shows the SQL and runs it in one transaction with optimistic checks (a row
  changed by someone else rolls the batch back). Query results stay read-only and say why.
- Foreign-key navigation (SQLite, PostgreSQL, Oracle; composite keys), Bars / Lines / Pie
  charts from the selection, transposed and compare tabs, binary viewer (hex, base64, BMP /
  PNG / JPEG preview through Lume's own bounded decoder), WKT / EWKB geometry preview with
  explicit axes and SRID, "Open Link in Browser".
- Execution Output (`docs/execution-output.md`): editor runs execute on a worker thread with a
  real cancel (⌘.) that reaches the driver; a persistent Output tab per connection records
  every execution as structured events (started, completed, failed with SQLSTATE / code /
  position, rows affected, notices, transaction state as the driver reports it, cancelled),
  grouped per execution with search, severity and execution filters, Go to SQL (editor
  revision aware), Open Results, redacted copy and TXT / JSON export, retention by whole
  executions. Scripts run statement by statement with stop / continue on error.
- Hardening (`docs/hardening-status.md`): extractors approved per line and run shell-free in
  an OS sandbox with limits and an audit log; exports through `.part` files with distinct
  statuses (cancel releases the cursor, a full disk is an error, never a short file); the
  editor checks a full row snapshot, masks sensitive values in the review, reports "result
  unknown" when the connection drops at COMMIT and asks before closing a tab with unsaved
  changes; toolbar with tooltips, regrouped menus with keyboard navigation; cross-driver test
  runner `tests/run_all.sh` writing `build/test-report.md`.
- Drag-and-drop a `.db`/`.sqlite` file onto the window, or `File → Open Database…`.
- Starts with an in-memory demo database so there is something to play with.

## Build

Requires CMake ≥ 3.20, a C11 compiler, SDL3 and SQLite development files.

```sh
# macOS
brew install sdl3 cmake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/lume [path/to/database.db]
```

On Linux install `libsdl3-dev` and `libsqlite3-dev` (or build SDL3 from source);
on Windows use vcpkg (`sdl3`, `sqlite3`) with the CMake toolchain file.

## Shortcuts

| Action | macOS | Others |
|---|---|---|
| Run statement under cursor / selection | ⌘↩ | Ctrl+Enter |
| Run all | ⇧⌘↩ | Ctrl+Shift+Enter |
| Open SQLite file | ⌘O | Ctrl+O |
| New connection / edit connection | ⇧⌘N / ⌘E | Ctrl+Shift+N / Ctrl+E |
| Connect or disconnect (selected or active) | ⌘D | Ctrl+D |
| New query | ⌘N | Ctrl+N |
| Toggle explorer / results | ⌘B / ⌘J | Ctrl+B / Ctrl+J |
| Editor zoom | ⌘= ⌘- ⌘0 | Ctrl+= Ctrl+- Ctrl+0 |
| Refresh explorer | ⌘R | Ctrl+R |
| Undo / redo | ⌘Z / ⇧⌘Z | Ctrl+Z / Ctrl+Shift+Z |
| Grid: find / fetch more / export | ⌘F / ⇧⌘M / ⇧⌘E | Ctrl+F / Ctrl+Shift+M / Ctrl+Shift+E |
| Run in a new result tab | ⌥⌘↩ | Ctrl+Alt+Enter |
| Next / previous / close result tab | ⌥⌘] / ⌥⌘[ / ⌘W | Ctrl+Alt+] / Ctrl+Alt+[ / Ctrl+W |
| Columns panel / execution history | ⇧⌘C / ⌘Y | Ctrl+Shift+C / Ctrl+Y |
| Complete (popup on demand) | ⌃Space | Ctrl+Space |
| Popup: accept / close | ↩ or Tab / Esc | Enter or Tab / Esc |

## Layout of the code

```
src/main.c      window, event loop, layout, shortcuts, status bar
src/menu.c      menu bar and dropdowns
src/explorer.c  schema tree
src/editor.c    text buffer, SQL tokenizer, statement detection, rendering
src/rs.c        ResultSet model (arena storage, cursor, type kinds)
src/grid.c      grid view model: sort, filter, search, selection, copy/export (no UI)
src/results.c   result tabs and grid view: drawing, input, layout, viewer, dialogs, menus
src/json.c      JSON parser for the value viewer tree
src/complete.c  autocomplete: lexer, alias scope, ranking, usage store
src/db.c        driver interface + SQLite driver + helpers shared by drivers
src/db_pg.c     PostgreSQL driver (libpq via dlopen)
src/db_oracle.c Oracle driver (OCI via dlopen)
src/conn.c      saved connections, driver registry, store (connections.txt / secrets.txt)
src/dialog.c    new / edit connection dialog
src/ui.c        text field, button, segmented control
src/draw.c      rectangles, rounded rects, icons, clipping, text helpers
src/font.c      stb_truetype atlas → GPU texture, UTF-8 text drawing
src/script.c    event-injection harness used by tests/
```

## Tests

- `tests/grid_check.sh`: headless model checks (sorting, filters, search, selection, TSV/CSV/
  JSON/Markdown/SQL serialisation with NULL, quotes, newlines, big integers, formula guard,
  column layout round trip, display formats, JSON parser, streaming export through a fake
  cursor).
- `tests/smoke.sh`: UI smoke run through the event harness with screenshots.
- `tests/grid_ui.sh`: grid UI suite (server-side sort/filter, frozen + wrapped columns, JSON
  tree, formats, export dialog, history, memory budget, editing with review and apply,
  foreign keys, chart, transpose, compare, binary and geometry viewers) with screenshots in
  `build/ui/`.
- `tests/pg_check.sh`: PostgreSQL driver against `dbv-postgres` in Docker: streaming export
  of 300k rows through a live cursor with flat memory, OID kinds, server-side select.
- `tests/oracle_check.sh`: Oracle driver against Oracle Free in Docker (needs `ORA_PASSWORD`).
- `tests/perf_check.c`: timings of the grid model on 100k rows.

## Testing the UI without a human

`LUME_SCRIPT=tests/smoke.txt ./build/lume` replays clicks, keys and typing through the SDL
event queue and saves screenshots (`shot file.bmp`). `LUME_SCREENSHOT=out.bmp` captures the
first frame. See `tests/smoke.sh`. Quote a `type` argument to keep leading spaces
(`type " c WHERE c."`); `key ctrl+Space` opens the completion popup.

App data (connections, secrets, completion usage) lives under the app-data folder
(`~/Library/Application Support/Lume/Lume/` on macOS). `LUME_PREF_DIR=<dir>` points Lume at
another folder; `tests/smoke.sh` uses that to stay away from your real connections.

## Roadmap

- Find/replace in the editor, multiple query tabs
- Grid: cancel (needs async execution), PNG/JPEG decoding in the viewer, FK listing on
  Oracle, basemaps for geometries
- Autocomplete: a trained ranker over popup choices (today the usage weights are fixed),
  snippets for `JOIN … ON` from foreign keys
- Connect in a background job (today it is synchronous); MySQL/SQL Server behind the same
  `DbDriver` interface
- Passwords in the OS keychain instead of `secrets.txt`
- Light theme

## Fonts

Inter and JetBrains Mono are bundled under the SIL Open Font License (see `assets/fonts`).

## License

Copyright (C) 2026 Isabella Tessarotto. Lume Data is free software under the GNU Affero General
Public License, version 3 only (`AGPL-3.0-only`); see [`LICENSE`](LICENSE). [`LICENSING.md`](LICENSING.md)
adds a section 7 permission for database client libraries loaded from the user's installation
(such as the Oracle Instant Client) and a note on the Lume name and logo.
