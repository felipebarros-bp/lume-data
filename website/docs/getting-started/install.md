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

SDL3 is bundled. Database client libraries are loaded when present: install `libpq5` for PostgreSQL, the MySQL / MariaDB client for MySQL, Oracle Instant Client for Oracle. SQLite needs nothing.

## Windows (preview)

`lume-data-<version>-windows-x64.zip` is a portable folder. Unzip it and run `lume.exe`. The Windows build is new: please [report what does not work](https://github.com/tessarotto21/lume-data/issues/new/choose).

## Updates

Lume checks for a new release when it starts and then once a day (**Settings > Privacy > Updates**, on by default; **Help > Check for Updates** works either way). A newer version is offered with what changed since yours; **Download** opens the installer for your computer in the browser. Install it over the current one: your connections and settings stay.
