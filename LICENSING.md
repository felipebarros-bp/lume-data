# Licensing

Lume Data is free software.

    Copyright (C) 2026 Isabella Tessarotto

    This program is free software: you can redistribute it and/or modify it under the terms of
    the GNU Affero General Public License as published by the Free Software Foundation, version 3
    of the License only (AGPL-3.0-only).

    This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
    without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU Affero General Public License for more details.

The full license text is in [`LICENSE`](LICENSE). Source files carry the line
`SPDX-License-Identifier: AGPL-3.0-only`.

## Additional permission (AGPL-3.0 section 7): database client libraries

Lume connects to databases through client libraries that it does not ship. It loads them at run
time from the user's own installation: libpq (PostgreSQL), the Oracle Instant Client (OCI),
libmysqlclient or MariaDB Connector/C, and FreeTDS. Some of these libraries are not free software,
or their licenses are not compatible with the GNU AGPL.

> As a special exception, the copyright holders of Lume Data give you permission to link or
> combine Lume Data with a database client library that is not part of Lume Data, and that is
> loaded at run time from an installation made separately by the user. The library may be
> distributed under any license. You may convey the resulting combination, provided that you
> follow the terms of the GNU AGPL version 3 in every respect for all of the code other than that
> library. If you modify Lume Data, you may extend this exception to your version, but you are
> not obliged to do so. If you do not wish to do so, delete this exception statement from your
> version.

## Trademark (AGPL-3.0 section 7(e))

The license covers the code, not the names "Lume" and "Lume Data" or their logos. A fork or a
modified version may be distributed under the license, but it must use a different name and logo.
It may say, truthfully, that it is based on Lume Data.

## Third-party components

Bundled components keep their own licenses. They are listed in
[`assets/licenses/THIRD_PARTY.txt`](assets/licenses/THIRD_PARTY.txt), and the About window shows
them as well. The bundled components are SDL3 (zlib), SQLite (public domain), stb_truetype
(MIT / public domain), and Inter and JetBrains Mono (SIL Open Font License).

## Contributions

Contributions are accepted under the same license (AGPL-3.0-only, with the exception above).
Should the project ever change its license, every contributor whose code remains in it would
have to agree; a contributor agreement (CLA) is not in place yet.
