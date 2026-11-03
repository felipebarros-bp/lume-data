---
title: Your first connection
sidebar_position: 2
---

# Your first connection

Lume opens with a **Demo** database (SQLite, in memory) so you can try everything right away.

## Add a connection

1. In the **Explorer**, click **+** (or right-click and choose **New Connection...**).
2. Pick the database (SQLite, PostgreSQL, MySQL / MariaDB, SQL Server, Oracle).
3. Fill in the fields and press **Test**, then **Save**.

**Passwords are kept in your operating system's secret store** (Keychain on macOS, the Secret Service on Linux, Credential Manager on Windows), never in Lume's files.

## Environment and read-only

- **Environment**: label a connection **DEV**, **STG** (staging) or **PROD**. The label and its colour appear next to the connection and in every write confirmation.
- **Read-only in Lume**: Lume refuses to run writes on this connection, as far as the driver allows.

## Duplicate a connection

Right-click a connection and choose **Duplicate Connection...**: the dialog opens with every field filled in, named "... (copy)", with the **password empty** (it is never copied).
