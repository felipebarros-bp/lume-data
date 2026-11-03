# Lume: notes for Claude

## Build and tests

- Build with `cmake --build build`. When a file is added, run `cmake -S . -B build` first.
- The full pipeline is `sh tests/run_all.sh`. It writes `build/test-report.md`.
- **Always run tests in the background.** Isa keeps using the Mac while they run.
  - With `LUME_SCRIPT`, test windows are never shown, never raised and get no Dock icon.
  - `LUME_FOREGROUND=1` shows them, and only for watching (`tests/watch.sh` sets it).
- **Never edit sources while a pipeline is running.** Each suite rebuilds, so a half-done edit fails the suites that come after it.
- **Rebuilding replaces `build/lume`.** If Isa has Lume open, its Keychain reads stop working until she reopens it. Tell her before rebuilding.
- The harness `wait` counts frames, not time. For async state use `rpt await`, `git await`, `awaitconn "<conn>"`, `objawait` or `nsx ... await`.

## AI in tests

- **`LUME_ALLOW_REAL_AI` is OFF by default and stays off.**
  - Without it, a run under `LUME_SCRIPT` refuses every AI endpoint that is not on loopback (`ai.c`: `ai_ready` and `ai_http`). Automated tests only ever talk to the mocks (`tests/tools/ai_mock.py`, `tests/tools/openai_mock.py`).
  - Set it **only** on the command line of a manual validation run against a real provider, while working on the AI features. Never put it in a script, in `run_all.sh` or in a saved file.
  - Real runs use Isa's own plan or key, so they spend her usage. Say so before starting.
- **The OS secret store in tests:** use `LUME_SECSTORE_SERVICE="Lume Data Test"`, so a test never reads or overwrites Isa's real entries (service "Lume Data").

## Security rules

- **Secrets:**
  - Never print or log passwords, keys or tokens.
  - Pasted AI keys and the ChatGPT session live only in the OS secret store (`src/secstore.c`): never in a file, a log, the screen or a command line.
  - Test database passwords go only into the test prefs `secrets.txt`, written with `umask 077`.
- **Logs and history:** never store credentials, connection strings or parameter values there.
- **AI is off by default:**
  - The model gets the database **structure** only after the user consents.
  - **Rows** go only with their own "data values" consent, and that consent shows a "your risk" warning.
  - The model's SQL is checked with `sqlguard`, run locally with `WHERE 1=0`, and executed read-only after approval.
- **Never use Claude subscription tokens (`claude setup-token`).** Anthropic's terms forbid them in third-party apps.
- **Ask first** before destructive or outward actions, and before accepting a license on Isa's behalf.

## User documentation (website/)

The public documentation is the Docusaurus site in `website/`, published to GitHub Pages
(https://tessarotto21.github.io/lume-data) by `.github/workflows/docs.yml` on every push to `main`
that touches `website/`. Each release freezes a version of it (the release workflow).

- **Every user-facing change updates the docs in the same commit**: a new feature gets its page
  or section, a changed behaviour, label, menu, shortcut or setting is corrected where it appears,
  a removed one is removed. Add new pages to `website/sidebars.js`.
- Write for users: task-oriented, English, the exact menu / button / shortcut names the app shows.
  No internal ids (DGM-xxx, BL-xxx), source file names or developer jargon.
- Only document what exists; say plainly when something is partial.
- Never name other products, and never use real company or customer data in examples.
- Check with `cd website && npm run build` (it fails on broken links) before committing.

## Contributing: what a PR needs (learned from reviews)

**Before opening**
- Rebase on the latest `main` and resolve conflicts there (several areas move fast: `explorer.c`,
  `dialog.c`, `conn.c`). Keep current names: environments are **DEV / STG / PROD** (STG replaced HML;
  "HML" is still read from old files).
- Run the full pipeline (`sh tests/run_all.sh`, 0 failed). New suites go in `tests/run_all.sh` and, when
  they need no database server, in the macOS / Linux lists of `tests/run_ci.sh`.
- Update `website/` for anything a user sees (see above). No `Co-Authored-By` lines.
- The description says what, why, design, tests (what ran, what was skipped and why).

**Tests that really test**
- Drive the UI the way a user does: right-click a row (`rbutton "tdrow:..."` / `rnode`), click the menu
  item by label (`menu "Add Foreign Key"`), click buttons by target (`button "..."`). A harness op that
  calls the action directly (`td menu <id>`) skips the popup and hides bugs in it.
- Assert on state dumps (`dgmstate`, `tdstate`, `explorerdump`...), not on timing.
- Cover the failure paths too (refused input, a cancelled operation, an empty catalog).

**Code rules that reviews keep catching**
- **Processes:** `posix_spawn` with an argv (never a shell); option look-alikes refused and `--` before
  operands; the child must not inherit Lume's descriptors (`POSIX_SPAWN_CLOEXEC_DEFAULT` on macOS,
  `O_CLOEXEC` / close-on-exec elsewhere); a child must not outlive Lume (a watch pipe or cleanup).
- **Threads:** code that can run on a worker (connect, catalog reads, AI) uses re-entrant calls only:
  `strtok_r`, `localtime_r`, no static buffers.
- **Network trust:** never accept an unknown SSH host key or certificate silently; show the fingerprint
  and ask.
- **Secrets:** never copied (Duplicate Connection leaves passwords empty), never logged, never on a
  command line; a struct copied from a live connection has its runtime pointers cleared (db, tables,
  attempt, tunnel).
- **Sizes:** no fixed-size stacks / arrays that silently drop items as data grows; allocate from the count.
- **Menus:** popup ids of different menus must not overlap ranges another branch interprets (a type
  picker's 100.. range once swallowed every component menu item).
- **Immediate-mode UI:** `ui_target` sees only widgets recorded so far in *this* frame; a widget drawn
  later on top (a dropdown) must keep its own rect from the previous frame for hit-testing below it.
- **Input:** per-frame fields reset in `input_begin_frame` (main window) **and** `app_input_begin`
  (panel windows).
- Never name other products in code, comments or docs; never use real company or customer data.

## License

GNU AGPL-3.0-only, Copyright (C) 2026 Isabella Tessarotto (a personal project). See `LICENSE` and `LICENSING.md`. New source files start with `SPDX-License-Identifier: AGPL-3.0-only`.
