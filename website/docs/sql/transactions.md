---
title: Sessions, schema and transactions
---

# Sessions, schema and transactions

Lume opens **one session per connection**. Query tabs on the same connection share it; tabs on different connections never share a transaction. **Reconnect** starts a new session.

## Session details

**Query > Session > Session Details...** shows the tab's datasource, driver, a session id, the transaction state, the isolation level and the current schema. It never shows a connection string or a password.

## Default schema

The toolbar's **Schema** button sets the session's default schema, from the schemas Lume has already read (no query is run to list them):

- **PostgreSQL** and **Oracle**: changed with `SET search_path` / `ALTER SESSION`, logged in the Output, and shown only once the server accepts it. Until then the default is shown as unknown, never assumed.
- **SQLite**: "main · this database file" plus any attached databases.
- Databases without a default schema to set say so.

Completion keeps proposing qualified names; it does not resolve unqualified names against the chosen schema.

## Transaction mode

The toolbar's **Tx** button (or **Query > Transaction**) switches between:

- **Auto-commit** (default): each statement commits on its own.
- **Manual**: Lume starts a transaction (`BEGIN`, or autocommit off on Oracle). Your changes stay pending until **Commit** or **Rollback**.

The button reads "Tx: Manual · pending" only when the driver reports an open transaction. **Commit** and **Rollback** are enabled only then; their confirmation shows the datasource, schema and session, and the Output records what the server answered.

Isolation levels are offered only where the driver supports them.

## Never losing a transaction by accident

With a transaction open, Lume asks before anything that would end it:

- switching the tab to another connection: **Commit and switch**, **Rollback and switch** or cancel;
- disconnecting: **Commit and disconnect**, **Rollback and disconnect** or cancel;
- leaving Manual mode, or closing the last tab on that connection;
- reconnecting: "Reconnect and lose the transaction?".

A session that was lost reports its transaction as lost, never as committed. A new query tab never joins another tab's open transaction implicitly.

Edits made in the [results grid](editing-data.md) and the [Table Designer](../explore/table-designer.md) run in their own transactions, separate from Manual mode.
