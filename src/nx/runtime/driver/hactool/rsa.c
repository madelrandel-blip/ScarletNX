#include <string.h>
#include <stdlib.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include "rsa.h"
#include "sha.h"
#include "utils.h"
#include "types.h"

/* ScarletNX: OpenSSL instead of the bundled mbedtls (cmake/hactool-openssl.patch). mbedtls only did the modular
   exponentiation and hactool checked the PSS and PKCS#1 v1.5 encodings itself: OpenSSL verifies them here. */

#define RSA_2048_BYTES 0x100

/* The RSA-2048 public key with big-endian modulus n and e = 65537. */
static EVP_PKEY *rsa_public_key(const unsigned char *modulus) {
    static const unsigned char E[3] = {1, 0, 1};
    BIGNUM *n_bn = BN_bin2bn(modulus, RSA_2048_BYTES, NULL);
    BIGNUM *e_bn = BN_bin2bn(E, sizeof(E), NULL);
    OSSL_PARAM_BLD *build = OSSL_PARAM_BLD_new();
    OSSL_PARAM *params = NULL;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    EVP_PKEY *key = NULL;
    if (n_bn && e_bn && build && ctx
        && OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_N, n_bn)
        && OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_E, e_bn)
        && (params = OSSL_PARAM_BLD_to_param(build)) != NULL
        && EVP_PKEY_fromdata_init(ctx) > 0
        && EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
        key = NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(build);
    BN_free(n_bn);
    BN_free(e_bn);
    if (key == NULL) {
        FATAL_ERROR("Failed to import RSA key!");
    }
    return key;
}

/* Verify an RSA-2048 signature over SHA-256(data), with e = 65537. */
static int rsa2048_verify(const void *data, size_t len, const unsigned char *signature, const unsigned char *modulus, int pss) {
    unsigned char hash[0x20];
    EVP_PKEY *key = rsa_public_key(modulus);
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_pkey(NULL, key, NULL);
    int valid = 0;
    sha256_hash_buffer(hash, data, len);
    if (ctx && EVP_PKEY_verify_init(ctx) > 0
        && EVP_PKEY_CTX_set_signature_md(ctx, EVP_sha256()) > 0
        && EVP_PKEY_CTX_set_rsa_padding(ctx, pss ? RSA_PKCS1_PSS_PADDING : RSA_PKCS1_PADDING) > 0
        /* the console's PSS: MGF1 with SHA-256 and a 0x20-byte salt */
        && (!pss || (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) > 0
                     && EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, 0x20) > 0))) {
        valid = EVP_PKEY_verify(ctx, signature, RSA_2048_BYTES, hash, sizeof(hash)) == 1;
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(key);
    return valid;
}

/* Perform an RSA-PSS verify operation on data, with signature and N. */
int rsa2048_pss_verify(const void *data, size_t len, const unsigned char *signature, const unsigned char *modulus) {
    return rsa2048_verify(data, len, signature, modulus, 1);
}

/* Perform an RSA-PKCS1 verify operation on data, with signature and N. */
int rsa2048_pkcs1_verify(const void *data, size_t len, const unsigned char *signature, const unsigned char *modulus) {
    return rsa2048_verify(data, len, signature, modulus, 0);
}

static void calculate_mgf1_and_xor(unsigned char *data, size_t data_size, const void *h_src, size_t h_src_size) {
    unsigned char h_buf[RSA_2048_BYTES] = {0};
    memcpy(h_buf, h_src, h_src_size);

    unsigned char mgf1_buf[0x20];
    size_t ofs = 0;
    unsigned int seed = 0;
    while (ofs < data_size) {
        for (unsigned int i = 0; i < sizeof(seed); i++) {
            h_buf[h_src_size + 3 - i] = (seed >> (8 * i)) & 0xFF;
        }
        sha256_hash_buffer(mgf1_buf, h_buf, h_src_size + 4);
        for (unsigned int i = ofs; i < data_size && i < ofs + 0x20; i++) {
            data[i] ^= mgf1_buf[i - ofs];
        }
        seed++;
        ofs += 0x20;
    }
}

/* Perform an RSA-OAEP decryption (and verification) on data, with Signature and N. Only the private exponent is known,
   and OpenSSL's RSA decryption needs the public one too (for blinding), so the exponentiation is BN_mod_exp's,
   constant-time, and the OAEP decoding stays hactool's. */
int rsa2048_oaep_decrypt_verify(void *out, size_t max_out_len, const unsigned char *signature, const unsigned char *modulus, const unsigned char *exponent, size_t exponent_len, const unsigned char *label_hash, size_t *out_len) {
    unsigned char m_buf[RSA_2048_BYTES];
    BIGNUM *n_bn = BN_bin2bn(modulus, RSA_2048_BYTES, NULL);
    BIGNUM *d_bn = BN_bin2bn(exponent, (int)exponent_len, NULL);
    BIGNUM *c_bn = BN_bin2bn(signature, RSA_2048_BYTES, NULL);
    BIGNUM *m_bn = BN_new();
    BN_CTX *bn_ctx = BN_CTX_new();
    int ok = n_bn && d_bn && c_bn && m_bn && bn_ctx;
    if (ok) {
        BN_set_flags(d_bn, BN_FLG_CONSTTIME);
        ok = BN_mod_exp(m_bn, c_bn, d_bn, n_bn, bn_ctx) && BN_bn2binpad(m_bn, m_buf, RSA_2048_BYTES) == RSA_2048_BYTES;
    }
    BN_CTX_free(bn_ctx);
    BN_free(n_bn);
    BN_clear_free(d_bn);
    BN_free(c_bn);
    BN_clear_free(m_bn);
    if (!ok) {
        FATAL_ERROR("Failed to export exponentiated RSA message!");
    }

    /* There's no automated PSS verification as far as I can tell. */
    if (m_buf[0] != 0x00) {
        return false;
    }

    /* Unmask salt. */
    calculate_mgf1_and_xor(m_buf + 1, 0x20, m_buf + 0x21, RSA_2048_BYTES - 0x20 - 1);
    /* Unmask DB. */
    calculate_mgf1_and_xor(m_buf + 0x21, RSA_2048_BYTES - 0x20 - 1, m_buf + 1, 0x20);

    /* Validate label hash */
    const unsigned char *db = m_buf + 0x21;
    if (memcmp(db, label_hash, 0x20) != 0) {
        return false;
    }

    /* Validate message prefix. */
    const unsigned char *data = db + 0x20;
    size_t remaining = RSA_2048_BYTES - 0x20 - 1 - 0x20;
    while (*data == 0 && remaining) {
        data++;
        remaining--;
    }
    if (remaining == 0 || *data++ != 1) {
        return false;
    }
    remaining--;
    *out_len = remaining;
    if (remaining > max_out_len) {
        remaining = max_out_len;
    }
    memcpy(out, data, remaining);
    return true;
}
