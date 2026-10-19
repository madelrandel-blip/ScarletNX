#ifndef HACTOOL_AES_H
#define HACTOOL_AES_H

/* ScarletNX: OpenSSL instead of the bundled mbedtls (cmake/hactool-openssl.patch) */
#include <stddef.h>
#include <openssl/evp.h>

/* Enumerations. */
typedef enum {
    AES_MODE_ECB,
    AES_MODE_CTR,
    AES_MODE_XTS,
    AES_MODE_CBC
} aes_mode_t;

typedef enum {
    AES_DECRYPT = 0,
    AES_ENCRYPT = 1,
} aes_operation_t;

/* Define structs. */
typedef struct {
    EVP_CIPHER_CTX *cipher_enc;
    EVP_CIPHER_CTX *cipher_dec;
} aes_ctx_t;

/* Function prototypes. */
aes_ctx_t *new_aes_ctx(const void *key, unsigned int key_size, aes_mode_t mode);
void free_aes_ctx(aes_ctx_t *ctx);

void aes_setiv(aes_ctx_t *ctx, const void *iv, size_t l);

void aes_encrypt(aes_ctx_t *ctx, void *dst, const void *src, size_t l);
void aes_decrypt(aes_ctx_t *ctx, void *dst, const void *src, size_t l);

void aes_calculate_cmac(void *dst, void *src, size_t size, const void *key);

void aes_xts_encrypt(aes_ctx_t *ctx, void *dst, const void *src, size_t l, size_t sector, size_t sector_size);
void aes_xts_decrypt(aes_ctx_t *ctx, void *dst, const void *src, size_t l, size_t sector, size_t sector_size);

#endif
