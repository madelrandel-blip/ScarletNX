#ifndef HACTOOL_SHA_H
#define HACTOOL_SHA_H

/* ScarletNX: OpenSSL instead of the bundled mbedtls (cmake/hactool-openssl.patch) */
#include <stddef.h>
#include <openssl/evp.h>

/* Enumerations. */
typedef enum {
    HASH_TYPE_SHA1,
    HASH_TYPE_SHA256,
} hash_type_t;

/* Define structs. */
typedef struct {
    EVP_MD_CTX *digest;
} sha_ctx_t;

/* Function prototypes. */
sha_ctx_t *new_sha_ctx(hash_type_t type, int hmac);
void sha_update(sha_ctx_t *ctx, const void *data, size_t l);
void sha_get_hash(sha_ctx_t *ctx, unsigned char *hash);
void free_sha_ctx(sha_ctx_t *ctx);

void sha256_hash_buffer(unsigned char *digest, const void *data, size_t l);

void sha256_get_buffer_hmac(void *digest, const void *secret, size_t s_l, const void *data, size_t d_l);

#endif
