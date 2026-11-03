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

## Organise connections in folders

Give a connection a **Folder** in its dialog to group it in the Explorer. A `/` nests folders: `Clients/Acme` puts it in **Acme** inside **Clients**. Spaces and empty levels are tidied (` Clients // Acme ` becomes `Clients/Acme`); leave it empty for the top level.

- Folders come first at each level (by name), then the connections; each folder shows how many connections it holds.
- Folders start closed; Lume remembers which ones you opened.
- Right-click a folder:
  - **New Connection Here...** opens the dialog with that folder filled in.
  - **Rename Folder...** edits the name in place (Enter applies, Esc cancels); its connections follow.
  - **Remove Folder (keeps its connections)** moves its connections up one level; nothing is deleted.
  - **Expand All Folders** / **Collapse All Folders**.
- **Duplicate Connection...** keeps the folder of the original.

## Connect through an SSH tunnel

For PostgreSQL, MySQL, MariaDB, SQL Server and Oracle, tick **Use SSH tunnel** in the connection dialog when the database is only reachable through an SSH server (a bastion or jump host). Fill in:

| Field | What to enter |
|---|---|
| SSH host | the SSH server, or a `Host` name from your `~/.ssh/config` |
| Port | the SSH port; empty means 22 or what your `~/.ssh/config` says |
| SSH user | the SSH login; empty means `~/.ssh/config` or your login name |
| Key file | a private key (`~/` works); empty means your SSH agent or `~/.ssh/config` |

The database **Host** and **Port** stay the ones as seen from the SSH server (often `localhost` or an internal name).

- Lume uses your system's `ssh`, so `~/.ssh/config` (aliases, `ProxyJump`), the SSH agent and your keys work as they do in a terminal.
- Key-based login only: Lume never asks for an SSH password. A key with a passphrase works when the passphrase is in your agent (on macOS, the Keychain).
- The tunnel opens when the connection connects and closes with it. If Lume quits or crashes, its tunnels stop; after a forced quit, the next launch stops the ones left behind.
- Turning **Use SSH tunnel** off keeps the SSH settings for later. **Duplicate Connection...** copies them too.

### Trusting a new SSH server

The first time Lume connects to an SSH server your computer does not know yet, it does not trust it silently. It shows the server's **host key fingerprint** (for example `ssh-ed25519 SHA256:...`):

- after a connect: in a **Trust the SSH host ...?** dialog, with **Trust and Connect**;
- in the connection dialog: **Test connection** shows the fingerprint, with **Trust Host Key**.

Compare it with the fingerprint your server administrator gives you, and trust it only if they match. Lume then adds that exact key to your `~/.ssh/known_hosts`. If the server later shows a different key, the connection is refused with *Host key verification failed*: check with your administrator before changing `known_hosts`.

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
- **SSH tunnel: ...**: the message is `ssh`'s own (host not reachable, key refused, host key changed). Check the SSH fields and try the same login with `ssh` in a terminal.
- **Authentication failed / host not reachable**: the server's own message is shown. Use **Test connection** in the dialog to try again without saving.

See also [Troubleshooting](../troubleshooting.md).
