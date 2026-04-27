/**
 * ============================================================================
 * crypto_layer_cmox.c
 * ============================================================================
 * Implementacja warstwy kryptografii dla systemu BEKO z CMOX
 * 
 * AES-128-CTR (hardware accelerated) + HMAC-SHA256
 * ============================================================================
 */

#include "crypto_layer_cmox.h"
#include "cmox_cipher.h"
#include "cmox_hash.h"
#include <string.h>
#include <stdio.h>

/* ========== ZMIENNE GLOBALNE ========== */

static crypto_context_t g_crypto_ctx = {
    .initialized = 0,
    .tx_frame_counter = CRYPTO_FRAME_COUNTER_INIT,
    .rx_frame_counter_last = 0xFFFFFFFF
};

/* Handles dla CMOX */
static cmox_cipher_handle_t *g_cipher_ctx_enc = NULL;
static cmox_cipher_handle_t *g_cipher_ctx_dec = NULL;
static cmox_ctr_handle_t g_Ctr_Ctx_Enc;
static cmox_ctr_handle_t g_Ctr_Ctx_Dec;

/* ========== WEWNĘTRZNE FUNKCJE POMOCNICZE ========== */

/**
 * Wygeneruj IV (Nonce) na podstawie frame_counter
 * 
 * Dla CTR mode:
 * IV = [0x00000000] [frame_counter] [0x00000000][0x00000001]
 * (12 bytes nonce + 4 bytes counter part)
 */
static void generate_iv(uint32_t frame_counter, uint8_t *out_iv)
{
    memset(out_iv, 0, CRYPTO_IV_SIZE);
    
    // Umieść frame_counter w bajtach 0-3 (big-endian)
    out_iv[0] = (uint8_t)((frame_counter >> 24) & 0xFF);
    out_iv[1] = (uint8_t)((frame_counter >> 16) & 0xFF);
    out_iv[2] = (uint8_t)((frame_counter >>  8) & 0xFF);
    out_iv[3] = (uint8_t)((frame_counter >>  0) & 0xFF);
    
    // Reszta (12 bajtów) pozostaje zerami (lub można napełnić losowo)
}

/**
 * Oblicz HMAC-SHA256 z IV i ciphertext
 * MIC = HMAC-SHA256(key, IV || Ciphertext)[:4]
 * 
 * Używamy CMOX Hash API
 */
static int compute_hmac(const uint8_t *iv, const uint8_t *ciphertext,
                        uint16_t ciphertext_len, uint8_t *out_mic)
{
    cmox_hash_retval_t retval;
    cmox_hmac_sha256_handle_t hmac_ctx;
    uint8_t digest[32];
    size_t digest_len;
    
    // Construct HMAC-SHA256 context
    cmox_hmac_sha256_construct(&hmac_ctx);
    
    // Initialize with key
    retval = cmox_hash_hmacSha256_init(&hmac_ctx, g_crypto_ctx.key, CRYPTO_KEY_SIZE);
    if (retval != CMOX_HASH_SUCCESS) {
        printf("ERROR: cmox_hash_hmacSha256_init failed: %d\r\n", retval);
        return -1;
    }
    
    // Update with IV
    retval = cmox_hash_hmacSha256_append(&hmac_ctx, iv, CRYPTO_IV_SIZE);
    if (retval != CMOX_HASH_SUCCESS) {
        printf("ERROR: cmox_hash_hmacSha256_append (IV) failed: %d\r\n", retval);
        return -1;
    }
    
    // Update with ciphertext
    retval = cmox_hash_hmacSha256_append(&hmac_ctx, ciphertext, ciphertext_len);
    if (retval != CMOX_HASH_SUCCESS) {
        printf("ERROR: cmox_hash_hmacSha256_append (CT) failed: %d\r\n", retval);
        return -1;
    }
    
    // Finalize
    retval = cmox_hash_hmacSha256_finish(&hmac_ctx, digest, &digest_len);
    if (retval != CMOX_HASH_SUCCESS) {
        printf("ERROR: cmox_hash_hmacSha256_finish failed: %d\r\n", retval);
        return -1;
    }
    
    // Copy first 4 bytes as MIC
    memcpy(out_mic, digest, CRYPTO_MIC_SIZE);
    
    return 0;
}

/* ========== PUBLICZNE FUNKCJE API ========== */

int crypto_init(const uint8_t *key)
{
    if (key == NULL) {
        printf("ERROR: crypto_init - NULL key pointer\r\n");
        return -1;
    }
    
    // Skopiuj klucz
    memcpy(g_crypto_ctx.key, key, CRYPTO_KEY_SIZE);
    g_crypto_ctx.initialized = 1;
    g_crypto_ctx.tx_frame_counter = CRYPTO_FRAME_COUNTER_INIT;
    g_crypto_ctx.rx_frame_counter_last = 0xFFFFFFFF;
    
    printf("INFO: Crypto layer (CMOX AES-128-CTR) initialized\r\n");
    return 0;
}

int crypto_encrypt(const uint8_t *plaintext, uint16_t plain_len,
                   encrypted_frame_t *out_frame)
{
    cmox_cipher_retval_t retval;
    size_t computed_size = 0;
    
    // Validacja parametrów
    if (!g_crypto_ctx.initialized) {
        printf("ERROR: Crypto not initialized\r\n");
        return -1;
    }
    
    if (plaintext == NULL || out_frame == NULL) {
        printf("ERROR: crypto_encrypt - NULL pointer\r\n");
        return -1;
    }
    
    if (plain_len == 0 || plain_len > CRYPTO_MAX_PAYLOAD) {
        printf("ERROR: crypto_encrypt - Invalid payload length: %d\r\n", plain_len);
        return -1;
    }
    
    // ===== KROK 1: Wygeneruj IV =====
    generate_iv(g_crypto_ctx.tx_frame_counter, out_frame->iv);
    
    // ===== KROK 2: Skonfiguruj AES-128-CTR dla szyfrowania =====
    g_cipher_ctx_enc = cmox_ctr_construct(&g_Ctr_Ctx_Enc, CMOX_AESFAST_CTR_ENC);
    if (g_cipher_ctx_enc == NULL) {
        printf("ERROR: cmox_ctr_construct failed\r\n");
        return -1;
    }
    
    retval = cmox_cipher_init(g_cipher_ctx_enc);
    if (retval != CMOX_CIPHER_SUCCESS) {
        printf("ERROR: cmox_cipher_init failed: %d\r\n", retval);
        return -1;
    }
    
    retval = cmox_cipher_setKey(g_cipher_ctx_enc, g_crypto_ctx.key, CRYPTO_KEY_SIZE);
    if (retval != CMOX_CIPHER_SUCCESS) {
        printf("ERROR: cmox_cipher_setKey failed: %d\r\n", retval);
        return -1;
    }
    
    retval = cmox_cipher_setIV(g_cipher_ctx_enc, out_frame->iv, CRYPTO_IV_SIZE);
    if (retval != CMOX_CIPHER_SUCCESS) {
        printf("ERROR: cmox_cipher_setIV failed: %d\r\n", retval);
        return -1;
    }
    
    // ===== KROK 3: Szyfruj plaintext =====
    retval = cmox_cipher_append(g_cipher_ctx_enc, plaintext, plain_len,
                                out_frame->ciphertext, &computed_size);
    if (retval != CMOX_CIPHER_SUCCESS) {
        printf("ERROR: cmox_cipher_append failed: %d\r\n", retval);
        return -1;
    }
    
    out_frame->ciphertext_len = (uint16_t)computed_size;
    
    // Cleanup encrypt context
    cmox_cipher_cleanup(g_cipher_ctx_enc);
    
    // ===== KROK 4: Oblicz HMAC-SHA256 =====
    if (compute_hmac(out_frame->iv, out_frame->ciphertext,
                     out_frame->ciphertext_len, out_frame->mic) != 0) {
        printf("ERROR: compute_hmac failed\r\n");
        return -1;
    }
    
    // ===== KROK 5: Inkrementuj licznik =====
    g_crypto_ctx.tx_frame_counter++;
    
    printf("INFO: Encrypted %d bytes (CTR), counter=%lu, MIC=%02X%02X%02X%02X\r\n",
           plain_len, g_crypto_ctx.tx_frame_counter - 1,
           out_frame->mic[0], out_frame->mic[1],
           out_frame->mic[2], out_frame->mic[3]);
    
    return 0;
}

int crypto_decrypt(const encrypted_frame_t *frame,
                   uint8_t *out_plaintext, uint16_t *out_len)
{
    cmox_cipher_retval_t retval;
    uint8_t computed_mic[CRYPTO_MIC_SIZE];
    size_t computed_size = 0;
    uint32_t frame_counter;
    
    // Validacja
    if (!g_crypto_ctx.initialized) {
        printf("ERROR: Crypto not initialized\r\n");
        return -3;
    }
    
    if (frame == NULL || out_plaintext == NULL || out_len == NULL) {
        printf("ERROR: crypto_decrypt - NULL pointer\r\n");
        return -3;
    }
    
    if (frame->ciphertext_len == 0 || frame->ciphertext_len > CRYPTO_MAX_PAYLOAD) {
        printf("ERROR: crypto_decrypt - Invalid ciphertext length: %d\r\n",
               frame->ciphertext_len);
        return -3;
    }
    
    // ===== KROK 1: Weryfikacja MIC (authentication) =====
    if (compute_hmac(frame->iv, frame->ciphertext,
                     frame->ciphertext_len, computed_mic) != 0) {
        printf("ERROR: compute_hmac verification failed\r\n");
        return -3;
    }
    
    // Porównaj MIC
    if (memcmp(computed_mic, frame->mic, CRYPTO_MIC_SIZE) != 0) {
        printf("ERROR: MIC VERIFICATION FAILED! (POSSIBLE ATTACK!)\r\n");
        printf("  Expected: %02X%02X%02X%02X\r\n",
               computed_mic[0], computed_mic[1],
               computed_mic[2], computed_mic[3]);
        printf("  Got:      %02X%02X%02X%02X\r\n",
               frame->mic[0], frame->mic[1],
               frame->mic[2], frame->mic[3]);
        return -1;  // Błąd MIC = możliwy atak
    }
    
    // ===== KROK 2: Ekstrahuj frame_counter z IV =====
    frame_counter = (uint32_t)((frame->iv[0] << 24) |
                               (frame->iv[1] << 16) |
                               (frame->iv[2] <<  8) |
                               (frame->iv[3] <<  0));
    
    // ===== KROK 3: Sprawdzenie replay attack =====
    if (frame_counter <= g_crypto_ctx.rx_frame_counter_last) {
        printf("ERROR: REPLAY ATTACK DETECTED! Frame counter=%lu, last=%lu\r\n",
               frame_counter, g_crypto_ctx.rx_frame_counter_last);
        return -2;
    }
    
    // ===== KROK 4: Skonfiguruj AES-128-CTR do deszyfrowania =====
    g_cipher_ctx_dec = cmox_ctr_construct(&g_Ctr_Ctx_Dec, CMOX_AESFAST_CTR_DEC);
    if (g_cipher_ctx_dec == NULL) {
        printf("ERROR: cmox_ctr_construct (DEC) failed\r\n");
        return -3;
    }
    
    retval = cmox_cipher_init(g_cipher_ctx_dec);
    if (retval != CMOX_CIPHER_SUCCESS) {
        printf("ERROR: cmox_cipher_init (DEC) failed: %d\r\n", retval);
        return -3;
    }
    
    retval = cmox_cipher_setKey(g_cipher_ctx_dec, g_crypto_ctx.key, CRYPTO_KEY_SIZE);
    if (retval != CMOX_CIPHER_SUCCESS) {
        printf("ERROR: cmox_cipher_setKey (DEC) failed: %d\r\n", retval);
        return -3;
    }
    
    retval = cmox_cipher_setIV(g_cipher_ctx_dec, frame->iv, CRYPTO_IV_SIZE);
    if (retval != CMOX_CIPHER_SUCCESS) {
        printf("ERROR: cmox_cipher_setIV (DEC) failed: %d\r\n", retval);
        return -3;
    }
    
    // ===== KROK 5: Odszyfruj =====
    retval = cmox_cipher_append(g_cipher_ctx_dec, frame->ciphertext,
                                frame->ciphertext_len,
                                out_plaintext, &computed_size);
    if (retval != CMOX_CIPHER_SUCCESS) {
        printf("ERROR: cmox_cipher_append (DEC) failed: %d\r\n", retval);
        return -3;
    }
    
    *out_len = (uint16_t)computed_size;
    
    // Cleanup decrypt context
    cmox_cipher_cleanup(g_cipher_ctx_dec);
    
    // ===== KROK 6: Aktualizacja ostatniego zaakceptowanego licznika =====
    g_crypto_ctx.rx_frame_counter_last = frame_counter;
    
    printf("INFO: Decrypted %d bytes (CTR), counter=%lu, MIC verified\r\n",
           *out_len, frame_counter);
    
    return 0;
}

void crypto_reset_counter(void)
{
    g_crypto_ctx.tx_frame_counter = CRYPTO_FRAME_COUNTER_INIT;
    g_crypto_ctx.rx_frame_counter_last = 0xFFFFFFFF;
    printf("INFO: Crypto counters reset\r\n");
}

uint32_t crypto_get_tx_counter(void)
{
    return g_crypto_ctx.tx_frame_counter;
}

/* ========== SELF-TEST ========== */

void crypto_self_test(void)
{
    printf("\n=== CRYPTO LAYER SELF-TEST (CMOX AES-128-CTR) ===\r\n");
    
    // Test vector (znany plaintext)
    uint8_t key[16] = {
        0xAE, 0x68, 0x52, 0xF8, 0x12, 0x10, 0x67, 0xCC,
        0x4B, 0xF7, 0xA5, 0x76, 0x55, 0x77, 0xF3, 0x9E
    };
    
    uint8_t plaintext[8] = {
        0x5E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    encrypted_frame_t encrypted;
    uint8_t decrypted[8];
    uint16_t dec_len;
    
    printf("[1] Initializing crypto with test key...\r\n");
    if (crypto_init(key) != 0) {
        printf("    FAILED to init\r\n");
        return;
    }
    
    printf("[2] Encrypting plaintext (CTR mode)...\r\n");
    printf("    Plaintext: ");
    for (int i = 0; i < 8; i++) printf("%02X ", plaintext[i]);
    printf("\r\n");
    
    if (crypto_encrypt(plaintext, 8, &encrypted) != 0) {
        printf("    FAILED to encrypt\r\n");
        return;
    }
    
    printf("    Ciphertext: ");
    for (int i = 0; i < encrypted.ciphertext_len; i++) 
        printf("%02X ", encrypted.ciphertext[i]);
    printf("\r\n");
    printf("    MIC: %02X%02X%02X%02X\r\n",
           encrypted.mic[0], encrypted.mic[1],
           encrypted.mic[2], encrypted.mic[3]);
    
    printf("[3] Decrypting ciphertext (CTR mode)...\r\n");
    if (crypto_decrypt(&encrypted, decrypted, &dec_len) != 0) {
        printf("    FAILED to decrypt\r\n");
        return;
    }
    
    printf("    Decrypted: ");
    for (int i = 0; i < dec_len; i++) printf("%02X ", decrypted[i]);
    printf("\r\n");
    
    printf("[4] Verifying plaintext equality...\r\n");
    if (memcmp(plaintext, decrypted, 8) == 0) {
        printf("    ✓ SUCCESS: Plaintext matches!\r\n");
    } else {
        printf("    ✗ FAILED: Plaintext mismatch!\r\n");
    }
    
    printf("[5] Testing MIC verification (tampering detection)...\r\n");
    encrypted.mic[0] ^= 0xFF;
    if (crypto_decrypt(&encrypted, decrypted, &dec_len) == -1) {
        printf("    ✓ SUCCESS: Tampering detected!\r\n");
    } else {
        printf("    ✗ FAILED: Should have detected tampering!\r\n");
    }
    
    printf("[6] Testing replay attack detection...\r\n");
    encrypted.mic[0] ^= 0xFF;  // Restore
    crypto_reset_counter();
    
    uint8_t plaintext2[8] = {0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    encrypted_frame_t encrypted2;
    if (crypto_encrypt(plaintext2, 8, &encrypted2) != 0) {
        printf("    FAILED to encrypt second frame\r\n");
        return;
    }
    
    printf("    Attempting to decrypt old frame again...\r\n");
    if (crypto_decrypt(&encrypted, decrypted, &dec_len) == -2) {
        printf("    ✓ SUCCESS: Replay attack detected!\r\n");
    } else {
        printf("    ✗ FAILED: Should have detected replay!\r\n");
    }
    
    printf("\n=== SELF-TEST COMPLETED ===\r\n\n");
}
