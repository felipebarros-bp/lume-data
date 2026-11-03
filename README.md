# Lume Data

A native database IDE written in C. One window for writing SQL, reading results, understanding
execution plans, designing tables, building reports, and asking an AI assistant for help, without
Electron and without a runtime.

![Query results](docs/screenshots/query-results.png)

- **Native and light.** About 2.3 MB of binary, drawn on the GPU through SDL3 (Metal, Vulkan,
  Direct3D), HiDPI aware, about 1% CPU when idle (the loop sleeps until something happens).
- **Six databases.** SQLite built in; PostgreSQL, Oracle, MySQL, MariaDB and SQL Server through the
  client libraries already installed on the machine, loaded at run time.
- **AI that asks first.** Lume Intelligence is off until you turn it on. The structure of a database
  is shared only after you allow it, rows only with a separate consent, and nothing it writes runs
  without your approval.
- **Cross-platform.** macOS (universal DMG, signed with a Developer ID), Linux (x86_64, arm64), and
  Windows (in progress).

## Why I created Lume Data

For a long time, one thing has bothered me in my day-to-day work as a developer: **why is it so
difficult to find a SQL IDE that is good, fast, modern, and free?** Some tools I tried were powerful
but consumed a surprising amount of resources for simple tasks; others were free and open source,
but sluggish, prone to freezes, or unfinished. I just wanted to open my tool, connect to a database,
write my queries, and get work done.

The inspiration goes back years, to when I discovered Linux and how it began: someone starting a
project out of curiosity and sharing it so others could use it, study it, change it and make it
better. **"That's amazing! This is what technology should be."** Throughout my career I have relied
on countless open-source projects, built by people I will probably never meet. **I always wanted to
do the same someday.**

Later, working with databases every day, I found a commercial IDE that felt close to what I was
looking for. But one thing kept bothering me: the cost. Not because commercial software isn't worth
it, but because access to great tools shouldn't depend on someone's financial circumstances, least
of all for people just getting started: the student writing their first `SELECT`, the person
changing careers, anyone who can't afford another subscription. **I believe technology should
expand human capabilities, not limit opportunities.**

I tried contributing to an existing open-source project, and learned how hard deep changes are in a
codebase with years of decisions behind it. So I asked myself: **"What if I started from scratch?"**
What if performance, simplicity and user experience were principles from the very first line of
code? That is how Lume Data was born, written in C with SDL, so I could control how it uses memory
and CPU and how it responds. **Lightweight shouldn't mean limited**: an intelligent editor, object
navigation, data editing, execution plans, Git, and AI that helps developers, analysts and people who
have never written a line of SQL. The tool should work for the person, not the other way around.

Lume Data is at the beginning of its journey, and I want it to grow in the open, with ideas,
criticism and contributions. **I want someone starting out today to have access to a tool as good
as the one used by an experienced professional at a large company**, without having to choose
between quality and accessibility. If Lume Data helps someone learn SQL, solve a problem, or simply
work with less frustration, it will already have been worth it. This is my way of giving back to
everyone who shared their knowledge before me: **by creating something and sharing it with the
world.**

*Isabella Tessarotto, creator of Lume Data*

## What it does

### Connections and schema
- A database picker with search (`pg`, `mssql`, `file`...), grouped by kind, then a form with the
  fields of that driver. **Test connection** runs in the background and reports the server's answer.
- Environments (DEV / STG / PROD) and a **Read-only in Lume** switch; choosing PROD suggests it.
- **Folders** in the Explorer: a connection's Folder field (`/` nests, `Clients/Acme`) puts it under
  folder rows, which remember being open; right-click a folder to rename it, remove it (its
  connections move up) or create a connection inside it.
- **SSH tunnel** for PostgreSQL, Oracle, MySQL, MariaDB and SQL Server: Lume starts the system's `ssh`
  (`-N -L` to a free local port), so `~/.ssh/config` (aliases, `ProxyJump`), the ssh-agent and your keys
  just work. Key-based only (no SSH password prompt); the tunnel closes with the connection.
- Passwords, AI keys and the ChatGPT session live in the operating system's secret store (macOS
  Keychain, Windows Credential Manager, Secret Service on Linux), never in the connection file. Where a
  system has no secret store, connection passwords fall back to a file only your user can read.
- Schema explorer built for large catalogs (tested with a server of 170,009 schemas): names
  first, details on demand, server-side search, pinned schemas.

![Schema explorer](docs/screenshots/schema-explorer.png)

### SQL editor and results
- Query tabs bound to a connection, syntax highlighting, autocomplete from the cached schema
  (aliases, `JOIN` targets, ranking by your own usage), run the statement under the cursor
  (`⌘↩` / `Ctrl+Enter`) or the whole script, with a real cancel that reaches the server.
- Results belong to the tab that ran them: run again and they are replaced; switch tabs and you see
  that tab's last run.
- A result grid with real cursors (fetch more / fetch all, never an invented total), virtual rows
  and columns, multi-column sort, filters, search, aggregates of the selection, a JSON tree viewer,
  binary / image / geometry previews, charts, transpose and compare.
- Edit table previews with a primary key: changes stay pending until **Review and apply** shows the
  SQL and runs it in one transaction, with optimistic checks against concurrent changes.
- Export to CSV, TSV, JSON, Markdown, SQL `INSERT` and XLSX, streamed through the open cursor.
- Execution Output: every run as structured events (start, rows, notices, errors with SQLSTATE and
  position, transaction state), with **Fix with AI** and **Go to SQL** next to an error.

### Execution plans
- Estimated and actual plans for SQLite, PostgreSQL, Oracle, MySQL, MariaDB and SQL Server, as a
  graph, an operator table and the raw output.
- Findings with their evidence (full scans, misestimates, spills, missing indexes), plan comparison,
  and hypothetical indexes on PostgreSQL (with `hypopg`, nothing is created).

![Execution plan](docs/screenshots/execution-plan.png)

### Lume Intelligence
- One assistant beside every tab. In a query tab it writes, fixes, explains and rewrites SQL in the
  database's dialect, using only tables and columns that exist.
- It reads the execution plan you have open, proposes a rewrite or an index, and can **check its own
  hypothesis**: compare the estimated plans, test a hypothetical index, or run both queries
  read-only (after your approval) and compare row counts and a checksum. No row is sent to the
  model for that.
- For large databases a local index of the catalog picks the tables that matter for each question;
  the model can ask to search or describe more.
- Every proposal is a preview: **Apply to editor** is one undo step, and statements that change data
  are flagged.
- Providers: Anthropic, OpenAI, a ChatGPT plan, or a local OpenAI-compatible model.

![Assistant fixing a query](docs/screenshots/assistant-fix.png)

### Database objects and table design
- Object editor for views, functions, procedures, triggers and packages, with drafts, a diff
  against the server, a reviewed apply and the status the server reports back (`VALID` /
  `INVALID` with its errors).
- Visual Table Designer for PostgreSQL, MySQL, MariaDB, SQL Server, Oracle and SQLite: columns,
  keys, indexes and checks, the exact DDL before anything runs, a risk level for each change, and
  drift detection when the table changed on the server meanwhile.

![Table designer](docs/screenshots/table-designer.png)

### Report Studio
- Reports made of KPI, line, bar, table and text cards over a semantic catalog or the card's own
  SQL, with report-wide filters and periods.
- Themes (colors, header with logo, card styles), pages, and an AI design pass that proposes a
  layout you approve step by step.
- Export to HTML, PDF, PNG, CSV and XLSX.

![Report studio](docs/screenshots/report-studio.png)

### Around the editor
- Source Control: Git status, staging by hunk, commit, pull strategies, push, with an optional AI
  review of the staged change.
- Command palette (`⇧⌘P`), dockable panels with saved layouts, Settings (`⌘,`), keyboard first.

## Performance

Measured by the test suite (`tests/run_all.sh`) on an Apple M4 with 16 GB, macOS 15.5. Every number
below has a bar the suite enforces, so a regression fails the build.

| What | Result | Bar |
|---|---|---|
| Schema explorer on a server with **170,009 schemas** | opens one page of 100 names; no node per schema until asked | no query per listed schema |
| Idle CPU (toolbar, panels, About open, 170k-schema tree) | **0.7 % to 1.3 %** | 2 % |
| UI frames after toolbar / panel changes | p95 **0.66 to 0.76 ms** | 16 ms |
| Result grid scrolling **800,000 rows** | median frame **0.45 ms** | 16 ms |
| Memory with an 800,000-row result open | **145 MB** RSS | 150 MB |
| Load 100,000 rows x 4 columns into the grid model | **32 ms**, 6 MB | 250 ms |
| Sort 100,000 rows (numeric / text / two keys) | **29 / 28 / 55 ms** | 250 ms |
| Search 400,000 cells / filter 100,000 rows | **7.7 / 16 ms** | 250 ms |
| Export 100,000 rows to TSV / JSON | **5.9 / 20 ms** | 250 ms |
| Assistant: index a **15,008-table** catalog | **48 ms** (reloaded from disk: 34 ms) | |
| Assistant: pick the tables for a question among 15,008 | **55 ms** | 1.5 s |
| Execution Output with a full log, scrolled | frame **4 ms**, 124 MB RSS | 16 ms, 150 MB |

Long work never blocks the window: queries, connects, connection tests, plans, exports and AI
requests run off the UI thread, with a cancel that reaches the server.

## Privacy

Lume Data works fully offline and sends nothing by default.

- **Lume Intelligence** is off until you turn it on; the database structure is shared only after you
  allow it, rows only with a separate consent.
- **Anonymous usage statistics** (Settings > Privacy) are off unless you turn them on. When on, Lume
  sends the version, the operating system and, per session, counts and timings in coarse ranges
  ("10-99 queries", "startup 0.5-1 s"). Never SQL, data, names of connections, tables or files, hosts,
  users, paths, keys or prompts, and no installation id. They go to [Aptabase](https://aptabase.com)
  in the EU, which does not store IP addresses. Settings > Privacy shows exactly what is sent.
- **Crash reports**: after a crash, the next launch asks before sending anything (or always / never,
  as you choose in Settings > Privacy). A report holds the version, the operating system, the signal
  and the function names where Lume stopped; never SQL, data, names, paths or user information.
  "View report" shows the exact report before it is sent. Reports go to GlitchTip.

## Install

Release builds come from the GitHub Actions workflow **Release builds** (run by hand):

| Platform | Package |
|---|---|
| macOS 11+ (Apple Silicon and Intel) | `Lume-Data-<version>-macos-universal.dmg` |
| Linux x86_64 / arm64 | `lume-data-<version>-linux-<arch>.tar.gz` (portable; run `./lume`) |
| Windows x64 | `lume-data-<version>-windows-x64.zip` (experimental) |

The database client libraries are not bundled: install the ones you need (for example `libpq`
for PostgreSQL, Oracle Instant Client for Oracle) and Lume Data finds them at run time.

## Build from source

Requires CMake 3.20+, a C11 compiler, SDL3 and SQLite.

```sh
# macOS
brew install sdl3 cmake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/lume [database.db]
```

On Linux install the SDL3 and SQLite development packages (or build SDL3 from source); on Windows
use vcpkg (`sdl3`, `sqlite3`) with its CMake toolchain file.

## Tests

- `sh tests/run_all.sh` runs the full suite (58 suites: headless checks, sanitizer runs and UI
  scenarios driven through the event harness) and writes `build/test-report.md`. The database
  suites need the PostgreSQL, Oracle, SQL Server and MySQL test containers.
- `sh tests/run_ci.sh` runs the suites that need no database server; this is what CI runs.
- The AI suites only talk to local mock servers; a run under the harness refuses any non-local AI
  endpoint.

`LUME_SCRIPT=<file> ./build/lume` replays clicks, keys and typing and saves screenshots;
`LUME_PREF_DIR=<dir>` keeps a run away from your real connections.

## License

Copyright (C) 2026 Isabella Tessarotto. Lume Data is free software under the GNU Affero General
Public License, version 3 only (`AGPL-3.0-only`); see [`LICENSE`](LICENSE).
[`LICENSING.md`](LICENSING.md) adds a section 7 permission for database client libraries loaded
from the user's installation and a note on the Lume Data name and logo. Third-party components and
their licenses are listed in `assets/licenses/THIRD_PARTY.txt`.
