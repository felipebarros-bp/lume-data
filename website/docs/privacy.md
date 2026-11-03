---
title: Privacy
sidebar_position: 10
---

# Privacy: what leaves your computer

Lume works offline. These are the only things that can leave your machine, and each is under your control in **Settings > Privacy**.

| What | When | Default |
|---|---|---|
| **Update check** | at launch and once a day: a request to GitHub's public releases list | on (turn off in Settings) |
| **Anonymous usage statistics** | the version, the operating system and coarse counts per session ("10-99 queries") | **off** |
| **Crash reports** | after a crash, on the next launch: the version, the system and where it stopped | **ask each time** |
| **Lume Intelligence** | only when you ask, after a dialog showing exactly what is sent | not configured |

## Where it goes

- **Usage statistics** go to Aptabase, in the EU. It uses the IP address only to derive the country and does not store it. A session's summary is sent at the next launch, so quitting never waits on the network. **Settings > Privacy** shows exactly what is sent.
- **Crash reports** go to GlitchTip. After a crash, the next launch asks first (**Send report**, or **View report** to read the exact report). Settings > Privacy can make it always or never send.
- **The update check** asks GitHub for the latest Lume Data release. It carries nothing about you or your work (GitHub sees the IP address, as with any download).

## Never sent

SQL text, query results, table or connection names, hosts, users, paths, passwords or keys are never part of statistics or crash reports. There is no installation id: every launch is a new, unrelated session.

## Passwords

Connection passwords and AI keys live in your operating system's secret store (Keychain, Secret Service, Credential Manager), never in Lume's files or logs.

## Lume Intelligence

The assistant is off until you configure a provider. Database **structure** goes to it only after you consent; **rows** need a separate consent that warns you about the risk. SQL it writes is checked, run first with `WHERE 1=0`, and executed read-only after you approve it. See [What is sent](intelligence/what-is-sent.md).
