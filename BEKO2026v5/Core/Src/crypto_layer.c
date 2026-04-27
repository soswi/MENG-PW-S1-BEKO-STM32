/**
 * ============================================================================
 * crypto_layer.c
 * ============================================================================
 * Implementacja: AES-128-CTR (CMOX) + HMAC-SHA256
 * 
 * Procedura szyfrowania:
 * 1. Wygeneruj IV z frame_counter
 * 2. Szyfruj plaintext AES-128-CTR
 * 3. Wylicz HMAC-SHA256(IV || ciphertext)[:4]
 * 
 * Procedura deszyfrowania:
 * 1. Sprawdź HMAC (authentication)
 * 2. Sprawdź replay (frame_counter)
 * 3. Odszyfruj AES-128-CTR
 * 
 * ============================================================================
 */

#include "crypto_layer.h"
#include "cmox_cipher.h"
#include "cmox_hash.h"
#include <string.h>
#include <stdio.h>

/* ========== GLOBAL STATE ========== */

static struct {
    uint8_t key[CRYPTO_KEY_SIZE];
    uint32_t tx_counter;
    uint32_t rx_counter_last;
    uint8_t initialized;
} g_ctx = {0};

/* ========== HELPERS ========== */

/**
 * Generate IV from frame counter
 * IV = [counter (4B big-endian)] + [zeros (12B)]
 */
static void generate_iv(uint32_t counter, uint8_t *iv)
{
    memset(iv, 0, 16);
    iv[0] = (counter >> 24) & 0xFF;
    iv[1] = (counter >> 16) & 0xFF;
    iv[2] = (counter >>  8) & 0xFF;
    iv[3] = (counter >>  0) & 0xFF;
}

/**
 * Extract counter from IV
 */
static uint32_t extract_counter(const uint8_t *iv)
{
    return ((uint32_t)iv[0] << 24) | ((uint32_t)iv[1] << 16) |
           ((uint32_t)iv[2] <<  8) | ((uint32_t)iv[3] <<  0);
}

/**
 * Compute HMAC-SHA256(IV || ciphertext)[:4]
 */
static int compute_hmac(const uint8_t *iv, const uint8_t *ciphertext,
                        uint16_t ct_len, uint8_t *out_mic)
{
    cmox_hmac_sha256_handle_t hmac_ctx;
    uint8_t digest[32];
    size_t digest_len;
    cmox_hash_retval_t ret;
    
    // Construct
    cmox_hmac_sha256_construct(&hmac_ctx);
    
    // Init with key
    ret = cmox_hash_hmacSha256_init(&hmac_ctx, g_ctx.key, CRYPTO_KEY_SIZE);
    if (ret != CMOX_HASH_SUCCESS) return -1;
    
    // Update with IV
    ret = cmox_hash_hmacSha256_append(&hmac_ctx, iv, 16);
    if (ret != CMOX_HASH_SUCCESS) return -1;
    
    // Update with ciphertext
    ret = cmox_hash_hmacSha256_append(&hmac_ctx, ciphertext, ct_len);
    if (ret != CMOX_HASH_SUCCESS) return -1;
    
    // Finish
    ret = cmox_hash_hmacSha256_finish(&hmac_ctx, digest, &digest_len);
    if (ret != CMOX_HASH_SUCCESS) return -1;
    
    // Copy first 4 bytes
    memcpy(out_mic, digest, CRYPTO_MIC_SIZE);
    return 0;
}

/* ========== PUBLIC API ========== */

int crypto_init(const uint8_t *key)
{
    if (!key) return -1;
    
    memcpy(g_ctx.key, key, CRYPTO_KEY_SIZE);
    g_ctx.tx_counter = 0;
    g_ctx.rx_counter_last = 0xFFFFFFFF;
    g_ctx.initialized = 1;
    
    printf("INFO: Crypto initialized\r\n");
    return 0;
}

int crypto_encrypt(const uint8_t *plaintext, size_t len, encrypted_data_t *out)
{
    cmox_cipher_handle_t *cipher_ctx;
    cmox_ctr_handle_t ctr_ctx;
    cmox_cipher_retval_t ret;
    size_t olen = 0;
    
    if (!g_ctx.initialized || !plaintext || !out || len == 0 || len > CRYPTO_MAX_DATA) {
        return -1;
    }
    
    // Generate IV
    generate_iv(g_ctx.tx_counter, out->iv);
    
    // Setup AES-CTR
    cipher_ctx = cmox_ctr_construct(&ctr_ctx, CMOX_AESFAST_CTR_ENC);
    if (!cipher_ctx) return -1;
    
    ret = cmox_cipher_init(cipher_ctx);
    if (ret != CMOX_CIPHER_SUCCESS) return -1;
    
    ret = cmox_cipher_setKey(cipher_ctx, g_ctx.key, CRYPTO_KEY_SIZE);
    if (ret != CMOX_CIPHER_SUCCESS) return -1;
    
    ret = cmox_cipher_setIV(cipher_ctx, out->iv, 16);
    if (ret != CMOX_CIPHER_SUCCESS) return -1;
    
    // Encrypt
    ret = cmox_cipher_append(cipher_ctx, plaintext, len,
                            out->ciphertext, &olen);
    cmox_cipher_cleanup(cipher_ctx);
    
    if (ret != CMOX_CIPHER_SUCCESS) return -1;
    
    out->ciphertext_len = (uint16_t)olen;
    
    // Compute HMAC
    if (compute_hmac(out->iv, out->ciphertext, out->ciphertext_len, out->mic) != 0) {
        return -1;
    }
    
    // Increment counter
    g_ctx.tx_counter++;
    
    printf("ENCRYPT: %zu bytes, ctr=%lu, MIC=%02X%02X%02X%02X\r\n",
           len, g_ctx.tx_counter - 1,
           out->mic[0], out->mic[1], out->mic[2], out->mic[3]);
    
    return 0;
}

int crypto_decrypt(const encrypted_data_t *encrypted, uint16_t ciphertext_len,
                   uint8_t *out_plaintext, size_t *out_len)
{
    cmox_cipher_handle_t *cipher_ctx;
    cmox_ctr_handle_t ctr_ctx;
    cmox_cipher_retval_t ret;
    size_t olen = 0;
    uint8_t computed_mic[CRYPTO_MIC_SIZE];
    uint32_t counter;
    
    if (!g_ctx.initialized || !encrypted || !out_plaintext || !out_len ||
        ciphertext_len == 0 || ciphertext_len > CRYPTO_MAX_DATA) {
        return -3;
    }
    
    // Verify HMAC
    if (compute_hmac(encrypted->iv, encrypted->ciphertext,
                     ciphertext_len, computed_mic) != 0) {
        return -3;
    }
    
    if (memcmp(computed_mic, encrypted->mic, CRYPTO_MIC_SIZE) != 0) {
        printf("ERROR: HMAC verification failed!\r\n");
        return -1;
    }
    
    // Check replay
    counter = extract_counter(encrypted->iv);
    if (counter <= g_ctx.rx_counter_last) {
        printf("ERROR: Replay attack detected! (ctr=%lu, last=%lu)\r\n",
               counter, g_ctx.rx_counter_last);
        return -2;
    }
    
    // Setup AES-CTR for decryption
    cipher_ctx = cmox_ctr_construct(&ctr_ctx, CMOX_AESFAST_CTR_DEC);
    if (!cipher_ctx) return -3;
    
    ret = cmox_cipher_init(cipher_ctx);
    if (ret != CMOX_CIPHER_SUCCESS) return -3;
    
    ret = cmox_cipher_setKey(cipher_ctx, g_ctx.key, CRYPTO_KEY_SIZE);
    if (ret != CMOX_CIPHER_SUCCESS) return -3;
    
    ret = cmox_cipher_setIV(cipher_ctx, encrypted->iv, 16);
    if (ret != CMOX_CIPHER_SUCCESS) return -3;
    
    // Decrypt
    ret = cmox_cipher_append(cipher_ctx, encrypted->ciphertext, ciphertext_len,
                            out_plaintext, &olen);
    cmox_cipher_cleanup(cipher_ctx);
    
    if (ret != CMOX_CIPHER_SUCCESS) return -3;
    
    *out_len = olen;
    
    // Update last counter
    g_ctx.rx_counter_last = counter;
    
    printf("DECRYPT: %zu bytes, ctr=%lu, OK\r\n", olen, counter);
    
    return 0;
}

void crypto_self_test(void)
{
    printf("\n=== CRYPTO SELF-TEST ===\r\n");
    
    uint8_t key[16] = {
        0xAE, 0x68, 0x52, 0xF8, 0x12, 0x10, 0x67, 0xCC,
        0x4B, 0xF7, 0xA5, 0x76, 0x55, 0x77, 0xF3, 0x9E
    };
    
    uint8_t plaintext[8] = {0x5E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    encrypted_data_t enc;
    uint8_t dec[8];
    size_t dec_len;
    
    printf("[1] Init...\r\n");
    if (crypto_init(key) != 0) {
        printf("FAILED\r\n");
        return;
    }
    
    printf("[2] Encrypt...\r\n");
    if (crypto_encrypt(plaintext, 8, &enc) != 0) {
        printf("FAILED\r\n");
        return;
    }
    printf("    CT: ");
    for (int i = 0; i < 8; i++) printf("%02X ", enc.ciphertext[i]);
    printf("\r\n");
    
    printf("[3] Decrypt...\r\n");
    if (crypto_decrypt(&enc, 8, dec, &dec_len) != 0) {
        printf("FAILED\r\n");
        return;
    }
    
    printf("[4] Compare...\r\n");
    if (memcmp(plaintext, dec, 8) == 0) {
        printf("✓ SUCCESS\r\n");
    } else {
        printf("✗ FAILED\r\n");
    }
    
    printf("[5] Test tampering...\r\n");
    enc.mic[0] ^= 0xFF;
    if (crypto_decrypt(&enc, 8, dec, &dec_len) == -1) {
        printf("✓ Tampering detected\r\n");
    } else {
        printf("✗ Should detect tampering\r\n");
    }
    
    printf("\n=== TEST DONE ===\r\n\n");
}
