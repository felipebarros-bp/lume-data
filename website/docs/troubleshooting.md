---
title: Troubleshooting and FAQ
---

# Troubleshooting and FAQ

## Connecting

**"libpq not found" / "MySQL client library not found" / "FreeTDS (libsybdb) not found" / "Oracle client not found".**
Lume uses the database client library installed on your computer. Install it, or point the variable named in the message at it (`LUME_LIBPQ`, `LUME_LIBMYSQL`, `LUME_LIBSYBDB`, `LUME_ORACLE_LIB`). See [Install](getting-started/install.md#database-client-libraries). **Help > About Lume Data** lists the drivers that load on this computer.

**The first Oracle connection takes several seconds.**
The first connection after starting Lume can be slow while the Oracle client initialises; later ones are fast. The window keeps working meanwhile, and **Cancel Connect** stops it.

**The network dropped (a VPN, Wi-Fi) while I was connected.**
Closing the connection, reconnecting or quitting gives up on the dead session after a few seconds instead of waiting for the network to time out, on every database. A statement that was running is left behind with its session and reported as not finished. Reconnect once the network is back.

**Run is disabled.**
Hover it: the tooltip says why. Usually the tab has no connection (**Select connection** on the toolbar), the connection is not open, or a statement is still running on it (**Stop** it first).

**My query ran on the wrong database.**
It cannot: each query tab runs on its own connection, shown on the toolbar. Clicking a connection in the Explorer only selects it. To change a tab's connection, use the toolbar's datasource button.

## Writes refused

**"Refused by the database: this login lacks the privilege".**
The server refused it; it is not a Lume setting. **"Read-only in Lume"** in the Output means the connection has Read-only in Lume checked ([Environments and read-only](connections/environments.md)).

**I cannot edit the rows of my result.**
Only table previews of tables with a primary key are editable, and only when every key column is in the result. The cell menu says why a result is read-only. See [Editing data](sql/editing-data.md).

## Results

**The row count says "total unknown".**
Lume loads results in batches and never guesses a total. **Fetch More Rows** or **Fetch All Rows** read the rest; the total appears when the result is complete.

**A result tab says it was discarded.**
Result tabs share a memory limit (**Query > Results > Memory Limit**). Over it, the oldest unpinned tab gives its rows back and keeps a **Re-run query** button. Pin tabs you want to keep, or raise the limit.

**My sort or filter does not match the database.**
Sorting, filtering and search work on the loaded rows and say so ("sorted locally"). For table previews, use **Sort on Server** / **Filter on Server...** in the header menu.

## The window

**A panel disappeared.**
**Window > Show Panel**, or **Window > Panels...** (**Option+Cmd+P**), lists every panel and its state. **Window > Layout > Default (Reset)** restores the default arrangement.

**The toolbar disappeared.**
**View > Toolbar > Main Toolbar** brings it back. Every command still works from the menus, the palette and its shortcut.

**Screen readers do not read Lume.**
Not yet: the interface is drawn by Lume itself and does not expose an accessibility tree. Everything can be used from the keyboard and the menus, and states are written in words, not only colours.

## Lume Intelligence

**The assistant says it is off.**
Set up a provider first: [Set up a provider](intelligence/setup.md). Everything else works without it.

**It says it needs the database structure.**
Allow **Share the structure of** the connection in the consent card, or ask about the SQL in front of you only.

**ChatGPT sign-in fails because a port is busy.**
The sign-in returns on port 1455. Close the other program using it and try again.

## Reporting a problem

1. Note what you did and what you saw.
2. **Help > About Lume Data > Copy Info** copies the version, build and system (never connection data or secrets).
3. Lume's log, `lume.log`, is in the `logs` folder inside the preferences folder (**Settings > General** shows where). It never holds passwords or SQL values, but read it before sharing.
4. [Open an issue](https://github.com/tessarotto21/lume-data/issues/new/choose).

## FAQ

**Is Lume free?** Yes. Lume Data is free software under the GNU AGPL-3.0.

**Does Lume send my data anywhere?** No, unless you turn something on. See [Privacy](privacy.md).

**Where are my connections and settings?** In Lume's preferences folder (**Settings > General**). Passwords are in your operating system's secret store, not in that folder.

**Can I use Lume offline?** Yes. Only the update check, optional statistics, crash reports and Lume Intelligence use the network.

**Which databases are supported?** SQLite, PostgreSQL, MySQL, MariaDB, SQL Server and Oracle.
