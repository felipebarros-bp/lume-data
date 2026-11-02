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

## License

GNU AGPL-3.0-only, Copyright (C) 2026 Isabella Tessarotto (a personal project). See `LICENSE` and `LICENSING.md`. New source files start with `SPDX-License-Identifier: AGPL-3.0-only`.
