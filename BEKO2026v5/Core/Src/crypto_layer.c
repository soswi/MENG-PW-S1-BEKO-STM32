/**
 * =============================================================================
 * crypto_layer.c
 * =============================================================================
 * AES-128-CTR encryption/decryption + HMAC-SHA256 message integrity check (MIC).
 *
 * Uses single-call cmox_cipher_encrypt/decrypt and cmox_mac_compute exclusively.
 * No multi-call handles — avoids internal cmox_cipher_retval dependencies.
 *
 * Security mechanisms:
 *   - AES-128-CTR: confidentiality of the payload
 *   - HMAC-SHA256 (first 4 bytes as MIC): authenticity and integrity
 *   - Monotonic TX counter embedded in IV: replay attack prevention on RX side
 * =============================================================================
 */

#include "crypto_layer.h"
#include "cmox_crypto.h"
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * MODULE-INTERNAL STATE
 * ============================================================================ */

/** Internal crypto context — holds key material and sequence counters. */
static struct {
    uint8_t  key[CRYPTO_KEY_SIZE];   /**< Symmetric AES/HMAC key (16 bytes)       */
    uint32_t tx_counter;             /**< Monotonically increasing TX frame counter */
    uint32_t rx_counter_last;        /**< Last accepted RX counter (replay guard)  */
    uint8_t  initialized;            /**< Set to 1 after crypto_init() succeeds    */
} g_ctx = {0};

/* ============================================================================
 * PRIVATE HELPERS
 * ============================================================================ */

/**
 * @brief Encodes a 32-bit counter into the first 4 bytes of a 16-byte IV.
 *        Remaining 12 bytes are zero-padded (big-endian counter in bytes 0–3).
 *
 * @param counter  TX frame counter value to encode.
 * @param iv       Output buffer — must be at least 16 bytes.
 */
static void encode_counter_into_iv(uint32_t counter, uint8_t *iv)
{
    memset(iv, 0, 16);
    iv[0] = (counter >> 24) & 0xFF;
    iv[1] = (counter >> 16) & 0xFF;
    iv[2] = (counter >>  8) & 0xFF;
    iv[3] = (counter >>  0) & 0xFF;
}

/**
 * @brief Decodes the 32-bit counter from the first 4 bytes of an IV.
 *
 * @param iv  16-byte IV buffer (produced by encode_counter_into_iv).
 * @return    Recovered frame counter value.
 */
static uint32_t decode_counter_from_iv(const uint8_t *iv)
{
    return ((uint32_t)iv[0] << 24)
         | ((uint32_t)iv[1] << 16)
         | ((uint32_t)iv[2] <<  8)
         | ((uint32_t)iv[3]);
}

/**
 * @brief Computes HMAC-SHA256 over (IV || ciphertext) and writes the first
 *        CRYPTO_MIC_SIZE bytes to out_mic. Uses the global key from g_ctx.
 *
 * @param iv         16-byte IV.
 * @param ciphertext Encrypted payload buffer.
 * @param ct_len     Length of ciphertext in bytes.
 * @param out_mic    Output buffer for the truncated MIC (CRYPTO_MIC_SIZE bytes).
 * @return  0 on success, -1 if the CMOX MAC operation fails.
 */
static int compute_mic(const uint8_t *iv, const uint8_t *ciphertext,
                       uint16_t ct_len, uint8_t *out_mic)
{
    /* Concatenate IV and ciphertext into a single input buffer for HMAC. */
    uint8_t hmac_input[16 + CRYPTO_MAX_DATA];
    memcpy(hmac_input,      iv,         16);
    memcpy(hmac_input + 16, ciphertext, ct_len);

    uint8_t digest[32];
    size_t  computed_size = 0;

    cmox_mac_retval_t ret = cmox_mac_compute(
        CMOX_HMAC_SHA256_ALGO,
        hmac_input,   16u + (size_t)ct_len,  /* input:  IV || ciphertext  */
        g_ctx.key,    CRYPTO_KEY_SIZE,        /* key                       */
        NULL,         0,                      /* additional data — none    */
        digest,       32,
        &computed_size
    );

    if (ret != CMOX_MAC_SUCCESS) {
        return -1;
    }

    /* Truncate the 32-byte digest to the first CRYPTO_MIC_SIZE bytes. */
    memcpy(out_mic, digest, CRYPTO_MIC_SIZE);
    return 0;
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

/**
 * @brief Initialises the crypto context with the provided symmetric key.
 *        Resets both TX and RX counters.
 *
 * @param key  Pointer to a CRYPTO_KEY_SIZE-byte key buffer.
 * @return  0 on success, -1 if key pointer is NULL.
 */
int crypto_init(const uint8_t *key)
{
    if (!key) {
        return -1;
    }

    memcpy(g_ctx.key, key, CRYPTO_KEY_SIZE);
    g_ctx.tx_counter      = 0;
    g_ctx.rx_counter_last = 0xFFFFFFFFu; /* Sentinel: accept any first counter */
    g_ctx.initialized     = 1;

    printf("INFO: Crypto initialized\r\n");
    return 0;
}

/**
 * @brief Encrypts plaintext using AES-128-CTR and appends a 4-byte MIC.
 *        The TX counter is used to derive the IV, then incremented atomically.
 *
 * @param plaintext  Input data buffer.
 * @param len        Number of bytes to encrypt (1 – CRYPTO_MAX_DATA).
 * @param out        Output structure populated with IV, ciphertext, and MIC.
 * @return  0 on success, -1 on any failure (not initialised, bad length, CMOX error).
 */
int crypto_encrypt(const uint8_t *plaintext, size_t len, encrypted_data_t *out)
{
    if (!g_ctx.initialized || !plaintext || !out ||
        len == 0 || len > CRYPTO_MAX_DATA) {
        return -1;
    }

    /* Step 1: Derive IV from the current TX counter. */
    encode_counter_into_iv(g_ctx.tx_counter, out->iv);

    /* Step 2: AES-128-CTR single-call encrypt. */
    size_t olen = 0;
    cmox_cipher_retval_t ret = cmox_cipher_encrypt(
        CMOX_AESFAST_CTR_ENC_ALGO,
        plaintext,  len,
        g_ctx.key,  CMOX_CIPHER_128_BIT_KEY,
        out->iv,    16u,
        out->ciphertext, &olen
    );

    if (ret != CMOX_CIPHER_SUCCESS) {
        return -1;
    }
    out->ciphertext_len = (uint16_t)olen;

    /* Step 3: Compute MIC over (IV || ciphertext). */
    if (compute_mic(out->iv, out->ciphertext, out->ciphertext_len, out->mic) != 0) {
        return -1;
    }

    /* Step 4: Advance TX counter only after all operations succeed. */
    g_ctx.tx_counter++;

    printf("ENCRYPT: %u bytes, ctr=%lu, MIC=%02X%02X%02X%02X\r\n",
           (unsigned)len,
           (unsigned long)(g_ctx.tx_counter - 1),
           out->mic[0], out->mic[1], out->mic[2], out->mic[3]);

    return 0;
}

/**
 * @brief Decrypts an encrypted_data_t packet after verifying MIC and replay guard.
 *
 * Verification order:
 *   1. MIC check (authenticity / integrity)
 *   2. Replay check (counter must be strictly greater than last accepted)
 *   3. AES-128-CTR decrypt
 *
 * @param enc            Pointer to the received encrypted packet.
 * @param ct_len         Length of the ciphertext portion in bytes.
 * @param out_plaintext  Output buffer for decrypted data (must be ≥ ct_len bytes).
 * @param out_len        Set to the number of decrypted bytes on success.
 * @return   0  — success
 *          -1  — MIC verification failed (tampered or wrong key)
 *          -2  — Replay attack detected (stale counter)
 *          -3  — Parameter error or CMOX decryption failure
 */
int crypto_decrypt(const encrypted_data_t *enc, uint16_t ct_len,
                   uint8_t *out_plaintext, size_t *out_len)
{
    if (!g_ctx.initialized || !enc || !out_plaintext || !out_len ||
        ct_len == 0 || ct_len > CRYPTO_MAX_DATA) {
        return -3;
    }

    /* Step 1: Verify MIC before touching the ciphertext. */
    uint8_t expected_mic[CRYPTO_MIC_SIZE];
    if (compute_mic(enc->iv, enc->ciphertext, ct_len, expected_mic) != 0) {
        return -3;
    }

    if (memcmp(expected_mic, enc->mic, CRYPTO_MIC_SIZE) != 0) {
        printf("ERROR: MIC verification failed!\r\n");
        return -1;
    }

    /* Step 2: Replay protection — reject counters that are not strictly increasing. */
    uint32_t counter = decode_counter_from_iv(enc->iv);
    if (g_ctx.rx_counter_last != 0xFFFFFFFFu && counter <= g_ctx.rx_counter_last) {
        printf("ERROR: Replay detected! (ctr=%lu, last=%lu)\r\n",
               (unsigned long)counter,
               (unsigned long)g_ctx.rx_counter_last);
        return -2;
    }

    /* Step 3: AES-128-CTR single-call decrypt. */
    size_t olen = 0;
    cmox_cipher_retval_t ret = cmox_cipher_decrypt(
        CMOX_AESFAST_CTR_DEC_ALGO,
        enc->ciphertext, ct_len,
        g_ctx.key,       CMOX_CIPHER_128_BIT_KEY,
        enc->iv,         16u,
        out_plaintext,   &olen
    );

    if (ret != CMOX_CIPHER_SUCCESS) {
        return -3;
    }

    *out_len = olen;
    g_ctx.rx_counter_last = counter; /* Update only after full success. */

    printf("DECRYPT: %u bytes, ctr=%lu, OK\r\n",
           (unsigned)olen, (unsigned long)counter);

    return 0;
}

/**
 * @brief Runs a built-in self-test covering encrypt, decrypt, and tamper detection.
 *        Prints pass/fail status for each step via UART.
 *        Intended for use during system bring-up only.
 */
void crypto_self_test(void)
{
    printf("\n=== CRYPTO SELF-TEST ===\r\n");

    static const uint8_t test_key[CRYPTO_KEY_SIZE] = {
        0xAE, 0x68, 0x52, 0xF8, 0x12, 0x10, 0x67, 0xCC,
        0x4B, 0xF7, 0xA5, 0x76, 0x55, 0x77, 0xF3, 0x9E
    };

    uint8_t plaintext[8]  = { 0x5E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    encrypted_data_t enc  = {0};
    uint8_t dec[8]        = {0};
    size_t  dec_len       = 0;

    /* [1] Initialise. */
    printf("[1] Init...\r\n");
    if (crypto_init(test_key) != 0) {
        printf("FAILED\r\n");
        return;
    }

    /* [2] Encrypt. */
    printf("[2] Encrypt...\r\n");
    if (crypto_encrypt(plaintext, 8, &enc) != 0) {
        printf("FAILED\r\n");
        return;
    }
    printf("    CT: ");
    for (int i = 0; i < 8; i++) {
        printf("%02X ", enc.ciphertext[i]);
    }
    printf("\r\n");

    /* [3] Decrypt. */
    printf("[3] Decrypt...\r\n");
    if (crypto_decrypt(&enc, enc.ciphertext_len, dec, &dec_len) != 0) {
        printf("FAILED\r\n");
        return;
    }

    /* [4] Plaintext comparison. */
    printf("[4] Compare...\r\n");
    if (memcmp(plaintext, dec, 8) == 0) {
        printf("SUCCESS\r\n");
    } else {
        printf("FAILED - plaintext mismatch\r\n");
    }

    /* [5] Tamper detection: flip one MIC byte, decrypt must reject it. */
    printf("[5] Test tampering...\r\n");
    enc.mic[0] ^= 0xFF;
    if (crypto_decrypt(&enc, enc.ciphertext_len, dec, &dec_len) == -1) {
        printf("Tampering detected OK\r\n");
    } else {
        printf("FAILED - tamper should have been detected\r\n");
    }

    printf("=== TEST DONE ===\r\n\n");
}