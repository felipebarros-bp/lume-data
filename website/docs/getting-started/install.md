---
title: Install
sidebar_position: 1
---

# Install

Download the installer for your computer from the [latest release](https://github.com/tessarotto21/lume-data/releases/latest).

## macOS

`Lume-Data-<version>-macos-universal.dmg` runs natively on Apple silicon and Intel Macs (macOS 11 Big Sur or later). It is signed and notarized by Apple.

1. Open the `.dmg`.
2. Drag **Lume Data** to **Applications**.
3. Open it from Applications.

## Linux

`lume-data-<version>-linux-x86_64.tar.gz` and `lume-data-<version>-linux-aarch64.tar.gz` are portable folders: no installation needed.

```bash
tar xzf lume-data-<version>-linux-x86_64.tar.gz
cd lume-data-<version>-linux-x86_64
./lume
```

SDL3 is bundled.

## Windows (preview)

`lume-data-<version>-windows-x64.zip` is a portable folder. Unzip it and run `lume.exe`. The Windows build is new: please [report what does not work](https://github.com/tessarotto21/lume-data/issues/new/choose).

## Database client libraries

SQLite is built in and needs nothing. For the other databases Lume uses the client library already installed on your computer, loaded when you connect. Install the ones you need:

| Database | Client library | If Lume does not find it |
|---|---|---|
| PostgreSQL | `libpq` (macOS: `brew install libpq`; Linux: `libpq5`) | set `LUME_LIBPQ` to the library file |
| MySQL / MariaDB | the MySQL client library (macOS: `brew install mysql-client`) | set `LUME_LIBMYSQL` to the library file |
| SQL Server | FreeTDS (macOS: `brew install freetds`) | set `LUME_LIBSYBDB` to the library file |
| Oracle | Oracle Instant Client | set `LUME_ORACLE_LIB` (or `ORACLE_HOME`) to it |

When a library is missing, connecting fails with a message that names the library and the variable to set. **Help > About Lume Data** lists the drivers whose client library loads on this computer.

## Updates

Lume checks for a new release when it starts and then once a day (**Settings > Privacy > Updates**, on by default; **Help > Check for Updates...** works either way). A newer version is offered with what changed since yours: **Download** opens the installer for your computer in the browser, **Later** asks again next time, **Skip This Version** stays quiet until the next one. Install it over the current one: your connections and settings stay.
