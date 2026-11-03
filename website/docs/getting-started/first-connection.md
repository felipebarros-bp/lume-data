---
title: Your first connection
sidebar_position: 2
---

# Your first connection

Lume opens with a **Demo** database (SQLite, in memory) so you can try everything right away. For something larger, **File > Connection > Open the E-commerce Demo Database** opens a small online store (categories, products, customers, orders, order items, payments, shipments and reviews) with two years of fictitious data, a good playground for reports and ER diagrams.

## Add a connection

1. In the **Explorer**, click **+** (or right-click and choose **New Connection...**, or press **Shift+Cmd+N**).
2. Pick the database: type to filter (`pg`, `mssql`, `file`...), arrows to move, **Enter** to continue.
3. Fill in the fields. **Enter** moves to the next empty field.
4. Press **Test connection**: it runs in the background and shows the server's answer beside the button.
5. Press **Save & Connect** (or **Save** to keep it without connecting).

**Passwords are kept in your operating system's secret store** (Keychain on macOS, the Secret Service on Linux, Credential Manager on Windows), never in Lume's connection file.

## Environment and read-only

- **Environment**: label a connection **DEV**, **STG** (staging) or **PROD**. The label appears on the toolbar's datasource button and in every write confirmation. Choosing PROD also turns on read-only, which you can turn off.
- **Read-only in Lume**: Lume refuses writes on this connection. The database login itself is unchanged.

See [Environments and read-only](../connections/environments.md) for what each one does.

## Next

- [A five-minute tour](tour.md)
- [Managing connections](../connections/managing.md): edit, duplicate, connect, delete.
