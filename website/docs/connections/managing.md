---
title: Managing connections
---

# Managing connections

A connection is a saved data source: a name, a database type and the fields that type needs. They are listed in the **Explorer**.

## Databases and their fields

| Database | Fields (defaults) |
|---|---|
| SQLite | File (**Browse...** to pick it) |
| PostgreSQL | Host (`localhost`), Port (`5432`), Database (`postgres`), User (`postgres`), Password |
| MySQL / MariaDB | Host (`127.0.0.1`), Port (`3306`), Database, User (`root`), Password |
| SQL Server | Host (`127.0.0.1`), Port (`1433`), Database (`master`), User (`sa`), Password |
| Oracle | Host (`localhost`), Port (`1521`), Service name (`FREEPDB1`), User, Password |

Under **General**, **Name** is what the Explorer shows (left empty, Lume builds one from the fields), plus [Environment and Read-only in Lume](environments.md).

PostgreSQL, MySQL / MariaDB, SQL Server and Oracle need their client library on your computer: see [Install](../getting-started/install.md#database-client-libraries).

## Create, edit, duplicate, delete

| To | Do |
|---|---|
| Create | **+** in the Explorer, **File > Connection > New Connection...** or **Shift+Cmd+N** |
| Edit | right-click > **Edit Connection...**, **File > Connection > Edit Connection...** or **Cmd+E** |
| Change the database type | in the edit dialog, **Change...** next to the type. Fields with the same name (host, port, user...) keep their values |
| Duplicate | right-click > **Duplicate Connection...**: the dialog opens with every field filled in, named "... (copy)", with the **password empty** (it is never copied) |
| Delete | in the edit dialog, **Delete**, then **Confirm delete** |

Saving an edit to a connected connection offers **Save & Reconnect**.

## Connect and disconnect

- **Connect**: double-click the connection, right-click > **Connect**, or **Cmd+D**. Connecting runs in the background: the window keeps working, and the toolbar shows *connecting*. **Cancel Connect** (toolbar datasource menu) stops it.
- **Disconnect**: right-click > **Disconnect**, or **Cmd+D** again. If the tab has an open transaction, Lume asks whether to commit or roll it back first.
- **Reconnect** (toolbar datasource menu or **Query > Session > Reconnect**) opens a fresh session. It is refused while a statement runs; an open transaction is reported as lost, never as committed.

A connection's state is always shown in words and with a mark: disconnected, connecting, connected, failed, reconnecting. After a failure, the datasource menu shows the last error (without the password).

## Which connection a query uses

Each query tab is bound to **its own** connection, shown on the toolbar's datasource button. Clicking a connection in the Explorer only selects it; it never redirects a query. To change the tab's connection, use the toolbar's datasource button: it lists connections with search, recent first, with type, state and environment. A tab with no connection shows **Select connection**, and Run is disabled until you pick one.

## Where passwords live

Passwords are stored in the operating system's secret store under "Lume Data": Keychain on macOS, Credential Manager on Windows, the Secret Service on Linux. Lume's connection file never holds one. Where a system has no secret store, passwords fall back to a file only your user can read. Deleting a connection removes its passwords too.

## When connecting fails

The message names the cause. The most common ones:

- **Client library not found**: install the library for that database or point the variable named in the message at it ([Install](../getting-started/install.md#database-client-libraries)).
- **Authentication failed / host not reachable**: the server's own message is shown. Use **Test connection** in the dialog to try again without saving.

See also [Troubleshooting](../troubleshooting.md).
