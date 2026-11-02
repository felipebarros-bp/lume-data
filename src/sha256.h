/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/* SHA-256 (FIPS 180-4), for review artifacts' checksums (OBJ-045). hex: 65 bytes. */
#include <stddef.h>
void sha256_hex(const void *data, size_t len, char hex[65]);
