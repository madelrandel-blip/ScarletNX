#include <stdlib.h>
#include <stdio.h>
#include <openssl/hmac.h>
#include "sha.h"
#include "types.h"
#include "utils.h"

/* ScarletNX: OpenSSL instead of the bundled mbedtls (cmake/hactool-openssl.patch) */

/* Allocate new context. hmac only reserved HMAC state in mbedtls, which the plain hash never used. */
sha_ctx_t *new_sha_ctx(hash_type_t type, int hmac) {
    sha_ctx_t *ctx;
    (void)hmac;

    if ((ctx = malloc(sizeof(*ctx))) == NULL) {
        FATAL_ERROR("Failed to allocate sha_ctx_t!");
    }

    if ((ctx->digest = EVP_MD_CTX_new()) == NULL) {
        FATAL_ERROR("Failed to set up hash context!");
    }

    if (!EVP_DigestInit_ex(ctx->digest, type == HASH_TYPE_SHA1 ? EVP_sha1() : EVP_sha256(), NULL)) {
        FATAL_ERROR("Failed to start hash context!");
    }

    return ctx;
}

/* Free an allocated context. */
void free_sha_ctx(sha_ctx_t *ctx) {
    /* Explicitly allow NULL. */
    if (ctx == NULL) {
        return;
    }

    EVP_MD_CTX_free(ctx->digest);
    free(ctx);
}

/* Update digest with new data. */
void sha_update(sha_ctx_t *ctx, const void *data, size_t l) {
    if (!EVP_DigestUpdate(ctx->digest, data, l)) {
        FATAL_ERROR("Failed to update hash!");
    }
}

/* Read hash from context. */
void sha_get_hash(sha_ctx_t *ctx, unsigned char *hash) {
    if (!EVP_DigestFinal_ex(ctx->digest, hash, NULL)) {
        FATAL_ERROR("Failed to finish hash!");
    }
}

/* SHA256 digest. */
void sha256_hash_buffer(unsigned char *digest, const void *data, size_t l) {
    if (!EVP_Digest(data, l, digest, NULL, EVP_sha256(), NULL)) {
        FATAL_ERROR("Failed to hash buffer!");
    }
}

/* SHA256-HMAC digest. */
void sha256_get_buffer_hmac(void *digest, const void *secret, size_t s_l, const void *data, size_t d_l) {
    if (s_l > 0x7FFFFFFF || HMAC(EVP_sha256(), secret, (int)s_l, data, d_l, digest, NULL) == NULL) {
        FATAL_ERROR("Failed getting HMAC output!");
    }
}
