/*********************************************************************
* Filename:   sha256.h
* Author:     Brad Conte (brad AT bradconte.com)
* Copyright:
* Disclaimer: This code is presented "as is" without any guarantees.
* Details:    Defines the API for the corresponding SHA-256 implementation.
*********************************************************************/

#ifndef LIQUID_SHA256_H
#define LIQUID_SHA256_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_BLOCK_SIZE 32

typedef unsigned char BYTE;
typedef unsigned int WORD;

typedef struct {
    BYTE data[64];
    WORD datalen;
    unsigned long long bitlen;
    WORD state[8];
} SHA256_CTX;

void sha256_init(SHA256_CTX* ctx);
void sha256_update(SHA256_CTX* ctx, const BYTE data[], size_t len);
void sha256_final(SHA256_CTX* ctx, BYTE hash[]);

#ifdef __cplusplus
}
#endif

#endif
