/* hash.c -- SHA-256 through Windows' own CNG provider.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 */
#include "hash.h"

#include <windows.h>
#include <bcrypt.h>
#include <limits.h>

int sha256_begin(sha256_t *s)
{
    s->alg = s->hash = NULL;
    if (BCryptOpenAlgorithmProvider((BCRYPT_ALG_HANDLE *)&s->alg,
                                    BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        return 0;
    if (BCryptCreateHash(s->alg, (BCRYPT_HASH_HANDLE *)&s->hash, NULL, 0,
                         NULL, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(s->alg, 0);
        s->alg = NULL;
        return 0;
    }
    return 1;
}

int sha256_update(sha256_t *s, const void *data, size_t len)
{
    const unsigned char *p = data;
    while (len) {
        ULONG n = len > ULONG_MAX ? ULONG_MAX : (ULONG)len;
        if (!s->hash || BCryptHashData(s->hash, (PUCHAR)p, n, 0) < 0)
            return 0;
        p += n;
        len -= n;
    }
    return 1;
}

int sha256_end(sha256_t *s, char out[65])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[32];
    int ok = s->hash && BCryptFinishHash(s->hash, digest, sizeof digest, 0) >= 0;
    unsigned i;

    if (s->hash)
        BCryptDestroyHash(s->hash);
    if (s->alg)
        BCryptCloseAlgorithmProvider(s->alg, 0);
    s->alg = s->hash = NULL;
    out[0] = 0;
    if (!ok)
        return 0;
    for (i = 0; i < sizeof digest; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15u];
    }
    out[64] = 0;
    return 1;
}

int sha256_hex(const void *data, size_t len, char out[65])
{
    sha256_t s;
    if (!sha256_begin(&s))
        return 0;
    if (!sha256_update(&s, data, len)) {
        sha256_end(&s, out);
        out[0] = 0;
        return 0;
    }
    return sha256_end(&s, out);
}
