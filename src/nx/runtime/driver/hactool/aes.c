#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <openssl/core_names.h>
#include "aes.h"
#include "types.h"
#include "utils.h"

/* ScarletNX: OpenSSL instead of the bundled mbedtls (cmake/hactool-openssl.patch). Padding stays off, as every caller
   passes whole blocks. A call starts from the IV or counter the last call left, as mbedtls_cipher_reset did, which
   also dropped what was left of a partial CTR block: set_num(0) does that here. */
static const EVP_CIPHER *aes_cipher(unsigned int key_size, aes_mode_t mode) {
    switch (mode) {
        case AES_MODE_ECB: return key_size == 32 ? EVP_aes_256_ecb() : key_size == 24 ? EVP_aes_192_ecb() : EVP_aes_128_ecb();
        case AES_MODE_CTR: return key_size == 32 ? EVP_aes_256_ctr() : key_size == 24 ? EVP_aes_192_ctr() : EVP_aes_128_ctr();
        case AES_MODE_CBC: return key_size == 32 ? EVP_aes_256_cbc() : key_size == 24 ? EVP_aes_192_cbc() : EVP_aes_128_cbc();
        /* XTS takes both keys, the data key and the tweak key */
        case AES_MODE_XTS: return key_size == 64 ? EVP_aes_256_xts() : EVP_aes_128_xts();
    }
    return NULL;
}

/* Allocate a new context. */
aes_ctx_t *new_aes_ctx(const void *key, unsigned int key_size, aes_mode_t mode) {
    aes_ctx_t *ctx;
    const EVP_CIPHER *cipher = aes_cipher(key_size, mode);

    if ((ctx = malloc(sizeof(*ctx))) == NULL) {
        FATAL_ERROR("Failed to allocate aes_ctx_t!");
    }

    ctx->cipher_dec = EVP_CIPHER_CTX_new();
    ctx->cipher_enc = EVP_CIPHER_CTX_new();
    if (cipher == NULL || ctx->cipher_dec == NULL || ctx->cipher_enc == NULL
        || (unsigned int)EVP_CIPHER_get_key_length(cipher) != key_size) {
        FATAL_ERROR("Failed to set up AES context!");
    }

    if (!EVP_CipherInit_ex(ctx->cipher_dec, cipher, NULL, key, NULL, AES_DECRYPT)
        || !EVP_CipherInit_ex(ctx->cipher_enc, cipher, NULL, key, NULL, AES_ENCRYPT)) {
        FATAL_ERROR("Failed to set key for AES context!");
    }
    EVP_CIPHER_CTX_set_padding(ctx->cipher_dec, 0);
    EVP_CIPHER_CTX_set_padding(ctx->cipher_enc, 0);

    return ctx;
}

/* Free an allocated context. */
void free_aes_ctx(aes_ctx_t *ctx) {
    /* Explicitly allow NULL. */
    if (ctx == NULL) {
        return;
    }

    EVP_CIPHER_CTX_free(ctx->cipher_dec);
    EVP_CIPHER_CTX_free(ctx->cipher_enc);
    free(ctx);
}

/* Set AES CTR or IV for a context. As with mbedtls, a longer IV is cut to the mode's length, and ECB, which has none,
   ignores it. */
void aes_setiv(aes_ctx_t *ctx, const void *iv, size_t l) {
    size_t iv_length = (size_t)EVP_CIPHER_CTX_get_iv_length(ctx->cipher_dec);
    if (iv_length == 0) {
        return;
    }
    if (l < iv_length
        || !EVP_CipherInit_ex(ctx->cipher_dec, NULL, NULL, NULL, iv, -1)
        || !EVP_CipherInit_ex(ctx->cipher_enc, NULL, NULL, NULL, iv, -1)) {
        FATAL_ERROR("Failed to set IV for AES context!");
    }
}

/* Calculate CMAC. */
void aes_calculate_cmac(void *dst, void *src, size_t size, const void *key) {
    EVP_MAC *mac = EVP_MAC_fetch(NULL, OSSL_MAC_NAME_CMAC, NULL);
    EVP_MAC_CTX *m_ctx = mac ? EVP_MAC_CTX_new(mac) : NULL;
    OSSL_PARAM params[2];
    size_t out_len = 0;
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER, "AES-128-CBC", 0);
    params[1] = OSSL_PARAM_construct_end();
    if (m_ctx == NULL
        || !EVP_MAC_init(m_ctx, key, 0x10, params)
        || !EVP_MAC_update(m_ctx, src, size)
        || !EVP_MAC_final(m_ctx, dst, &out_len, 0x10)) {
            FATAL_ERROR("Failed to calculate CMAC!");
    }
    EVP_MAC_CTX_free(m_ctx);
    EVP_MAC_free(mac);
}

static void aes_crypt(EVP_CIPHER_CTX *cipher, void *dst, const void *src, size_t l) {
    int out_len;
    EVP_CIPHER_CTX_set_num(cipher, 0);
    while (l) {
        /* EVP lengths are ints */
        int n = l > 0x40000000 ? 0x40000000 : (int)l;
        if (!EVP_CipherUpdate(cipher, dst, &out_len, src, n)) {
            FATAL_ERROR("AES operation failed!");
        }
        dst = (unsigned char *)dst + n;
        src = (const unsigned char *)src + n;
        l -= (size_t)n;
    }
}

/* Encrypt with context. */
void aes_encrypt(aes_ctx_t *ctx, void *dst, const void *src, size_t l) {
    aes_crypt(ctx->cipher_enc, dst, src, l);
}

/* Decrypt with context. EVP decrypts in place. */
void aes_decrypt(aes_ctx_t *ctx, void *dst, const void *src, size_t l) {
    aes_crypt(ctx->cipher_dec, dst, src, l);
}

static void get_tweak(unsigned char *tweak, size_t sector) {
    for (int i = 0xF; i >= 0; i--) { /* the console's LE custom tweak... */
        tweak[i] = (unsigned char)(sector & 0xFF);
        sector >>= 8;
    }
}

/* Encrypt with context for XTS. */
void aes_xts_encrypt(aes_ctx_t *ctx, void *dst, const void *src, size_t l, size_t sector, size_t sector_size) {
    unsigned char tweak[0x10];

    if (l % sector_size != 0) {
        FATAL_ERROR("Length must be multiple of sectors!");
    }

    for (size_t i = 0; i < l; i += sector_size) {
        /* Workaround for the console's custom sector...manually generate the tweak. */
        get_tweak(tweak, sector++);
        aes_setiv(ctx, tweak, 16);
        aes_encrypt(ctx, (char *)dst + i, (const char *)src + i, sector_size);
    }
}

/* Decrypt with context for XTS. */
void aes_xts_decrypt(aes_ctx_t *ctx, void *dst, const void *src, size_t l, size_t sector, size_t sector_size) {
    unsigned char tweak[0x10];

    if (l % sector_size != 0) {
        FATAL_ERROR("Length must be multiple of sectors!");
    }

    for (size_t i = 0; i < l; i += sector_size) {
        /* Workaround for the console's custom sector...manually generate the tweak. */
        get_tweak(tweak, sector++);
        aes_setiv(ctx, tweak, 16);
        aes_decrypt(ctx, (char *)dst + i, (const char *)src + i, sector_size);
    }
}
