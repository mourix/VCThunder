/* hash.h -- SHA-256 through Windows' own CNG provider.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * The one implementation: the loader hashes the image it is about to run, the
 * importer hashes the executable it finds inside a CHD and the one it wrote. */
#ifndef VCT_HASH_H
#define VCT_HASH_H

#include <stddef.h>

typedef struct {
    void *alg, *hash;
} sha256_t;

int sha256_begin(sha256_t *s);
int sha256_update(sha256_t *s, const void *data, size_t len);
/* Writes 64 lowercase hex digits and a NUL, and releases s either way. */
int sha256_end(sha256_t *s, char out[65]);
int sha256_hex(const void *data, size_t len, char out[65]);

#endif
