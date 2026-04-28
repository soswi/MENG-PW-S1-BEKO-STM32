/**
 * ============================================================================
 * crypto_layer.c
 * ============================================================================
 * AES-128-CTR (single-call) + HMAC-SHA256 (single-call)
 * Używa wyłącznie cmox_cipher_encrypt/decrypt i cmox_mac_compute.
 * Brak multi-call handle'ów — brak zależności od cmox_cipher_retvals wewnętrznych.
 * ============================================================================
 */

#include "crypto_layer.h"
#include "cmox_crypto.h"
#include <string.h>
#include <stdio.h>

/* ========== GLOBAL STATE ========== */

static struct {
    uint8_t  key[CRYPTO_KEY_SIZE];
    uint32_t tx_counter;
    uint32_t rx_counter_last;
    uint8_t  initialized;
} g_ctx = {0};

/* ========== HELPERS ========== */

static void generate_iv(uint32_t counter, uint8_t *iv)
{
    memset(iv, 0, 16);
    iv[0] = (counter >> 24) & 0xFF;
    iv[1] = (counter >> 16) & 0xFF;
    iv[2] = (counter >>  8) & 0xFF;
    iv[3] = (counter >>  0) & 0xFF;
}

static uint32_t extract_counter(const uint8_t *iv)
{
    return ((uint32_t)iv[0] << 24) | ((uint32_t)iv[1] << 16)
         | ((uint32_t)iv[2] <<  8) | ((uint32_t)iv[3]);
}

/**
 * HMAC-SHA256(key, IV || ciphertext) → pierwsze 4 bajty jako MIC
 * Używa single-call cmox_mac_compute — identycznie jak calculate_hmac_sha256 w main.c
 */
static int compute_hmac(const uint8_t *iv, const uint8_t *ciphertext,
                        uint16_t ct_len, uint8_t *out_mic)
{
    /* Zbuduj bufor: IV(16B) || ciphertext */
    uint8_t buf[16 + CRYPTO_MAX_DATA];
    memcpy(buf,      iv,         16);
    memcpy(buf + 16, ciphertext, ct_len);

    uint8_t digest[32];
    size_t  computed_size;

    cmox_mac_retval_t ret = cmox_mac_compute(
        CMOX_HMAC_SHA256_ALGO,
        buf,   16u + ct_len,          /* dane */
        g_ctx.key, CRYPTO_KEY_SIZE,   /* klucz */
        NULL,  0,                     /* dodatkowe dane — brak */
        digest, 32,
        &computed_size
    );

    if (ret != CMOX_MAC_SUCCESS) return -1;

    memcpy(out_mic, digest, CRYPTO_MIC_SIZE);
    return 0;
}

/* ========== PUBLIC API ========== */

int crypto_init(const uint8_t *key)
{
    if (!key) return -1;
    memcpy(g_ctx.key, key, CRYPTO_KEY_SIZE);
    g_ctx.tx_counter      = 0;
    g_ctx.rx_counter_last = 0xFFFFFFFFu;
    g_ctx.initialized     = 1;
    printf("INFO: Crypto initialized\r\n");
    return 0;
}

int crypto_encrypt(const uint8_t *plaintext, size_t len, encrypted_data_t *out)
{
    if (!g_ctx.initialized || !plaintext || !out ||
        len == 0 || len > CRYPTO_MAX_DATA)
        return -1;

    /* 1. Wygeneruj IV z licznika TX */
    generate_iv(g_ctx.tx_counter, out->iv);

    /* 2. AES-128-CTR encrypt — single call */
    size_t olen = 0;
    cmox_cipher_retval_t ret = cmox_cipher_encrypt(
        CMOX_AESFAST_CTR_ENC_ALGO,
        plaintext, len,
        g_ctx.key, CMOX_CIPHER_128_BIT_KEY,
        out->iv,   16u,
        out->ciphertext, &olen
    );
    if (ret != CMOX_CIPHER_SUCCESS) return -1;
    out->ciphertext_len = (uint16_t)olen;

    /* 3. HMAC-SHA256(IV || CT)[:4] */
    if (compute_hmac(out->iv, out->ciphertext, out->ciphertext_len, out->mic) != 0)
        return -1;

    g_ctx.tx_counter++;

    printf("ENCRYPT: %u bytes, ctr=%lu, MIC=%02X%02X%02X%02X\r\n",
           (unsigned)len, (unsigned long)(g_ctx.tx_counter - 1),
           out->mic[0], out->mic[1], out->mic[2], out->mic[3]);
    return 0;
}

int crypto_decrypt(const encrypted_data_t *enc, uint16_t ct_len,
                   uint8_t *out_plaintext, size_t *out_len)
{
    if (!g_ctx.initialized || !enc || !out_plaintext || !out_len ||
        ct_len == 0 || ct_len > CRYPTO_MAX_DATA)
        return -3;

    /* 1. Weryfikacja HMAC */
    uint8_t computed_mic[CRYPTO_MIC_SIZE];
    if (compute_hmac(enc->iv, enc->ciphertext, ct_len, computed_mic) != 0)
        return -3;

    if (memcmp(computed_mic, enc->mic, CRYPTO_MIC_SIZE) != 0) {
        printf("ERROR: HMAC verification failed!\r\n");
        return -1;
    }

    /* 2. Ochrona przed replay */
    uint32_t counter = extract_counter(enc->iv);
    if (g_ctx.rx_counter_last != 0xFFFFFFFFu && counter <= g_ctx.rx_counter_last) {
        printf("ERROR: Replay detected! (ctr=%lu, last=%lu)\r\n",
               (unsigned long)counter, (unsigned long)g_ctx.rx_counter_last);
        return -2;
    }

    /* 3. AES-128-CTR decrypt — single call */
    size_t olen = 0;
    cmox_cipher_retval_t ret = cmox_cipher_decrypt(
        CMOX_AESFAST_CTR_DEC_ALGO,
        enc->ciphertext, ct_len,
        g_ctx.key, CMOX_CIPHER_128_BIT_KEY,
        enc->iv,   16u,
        out_plaintext, &olen
    );
    if (ret != CMOX_CIPHER_SUCCESS) return -3;

    *out_len = olen;
    g_ctx.rx_counter_last = counter;

    printf("DECRYPT: %u bytes, ctr=%lu, OK\r\n",
           (unsigned)olen, (unsigned long)counter);
    return 0;
}

void crypto_self_test(void)
{
    printf("\n=== CRYPTO SELF-TEST ===\r\n");

    static const uint8_t key[16] = {
        0xAE,0x68,0x52,0xF8,0x12,0x10,0x67,0xCC,
        0x4B,0xF7,0xA5,0x76,0x55,0x77,0xF3,0x9E
    };
    uint8_t plaintext[8] = {0x5E,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    encrypted_data_t enc;
    uint8_t dec[8];
    size_t dec_len;

    printf("[1] Init...\r\n");
    if (crypto_init(key) != 0) { printf("FAILED\r\n"); return; }

    printf("[2] Encrypt...\r\n");
    if (crypto_encrypt(plaintext, 8, &enc) != 0) { printf("FAILED\r\n"); return; }
    printf("    CT: ");
    for (int i = 0; i < 8; i++) printf("%02X ", enc.ciphertext[i]);
    printf("\r\n");

    printf("[3] Decrypt...\r\n");
    if (crypto_decrypt(&enc, enc.ciphertext_len, dec, &dec_len) != 0) {
        printf("FAILED\r\n"); return;
    }

    printf("[4] Compare...\r\n");
    if (memcmp(plaintext, dec, 8) == 0)
        printf("SUCCESS\r\n");
    else
        printf("FAILED - dane nie zgadzaja sie\r\n");

    printf("[5] Test tampering...\r\n");
    enc.mic[0] ^= 0xFF;
    if (crypto_decrypt(&enc, enc.ciphertext_len, dec, &dec_len) == -1)
        printf("Tampering detected OK\r\n");
    else
        printf("FAILED - powinno wykryc tamper\r\n");

    printf("=== TEST DONE ===\r\n\n");
}
