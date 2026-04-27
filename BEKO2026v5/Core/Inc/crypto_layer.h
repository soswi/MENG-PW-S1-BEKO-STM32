/**
 * ============================================================================
 * crypto_layer.h
 * ============================================================================
 * Moduł szyfrowania dla systemu BEKO
 * 
 * AES-128-CTR (CMOX) + HMAC-SHA256
 * 
 * API:
 *   crypto_init(key) - inicjalizacja (raz)
 *   crypto_encrypt(plaintext, len) - szyfruje dane
 *   crypto_decrypt(ciphertext, len) - odszyfruje dane + weryfikuje HMAC
 * 
 * ============================================================================
 */

#ifndef CRYPTO_LAYER_H
#define CRYPTO_LAYER_H

#include <stdint.h>
#include <stddef.h>

/* ========== KONFIGURACJA ========== */

#define CRYPTO_KEY_SIZE         16      // AES-128 (128 bits)
#define CRYPTO_MIC_SIZE         4       // HMAC-SHA256[:4]
#define CRYPTO_MAX_DATA         256     // Max data size to encrypt

/* ========== STRUKTURY ========== */

/**
 * Wyjście szyfrowania
 */
typedef struct {
    uint8_t iv[16];                 // IV (16 bajtów)
    uint8_t ciphertext[CRYPTO_MAX_DATA];  // Szyfrowane dane
    uint16_t ciphertext_len;        // Długość ciphertext
    uint8_t mic[CRYPTO_MIC_SIZE];   // HMAC-SHA256[:4]
} encrypted_data_t;

/* ========== API ========== */

/**
 * Inicjalizacja warstwy kryptografii
 * Wywoła raz na starcie
 * 
 * Args:
 *   key: wskaźnik na 16-bajtowy klucz AES-128
 * 
 * Return:
 *   0 = OK, -1 = error
 */
int crypto_init(const uint8_t *key);

/**
 * Szyfruj dane (AES-128-CTR + HMAC-SHA256)
 * 
 * Args:
 *   plaintext: dane do szyfrowania
 *   len: długość danych
 *   out: wyjście (IV + ciphertext + MIC)
 * 
 * Return:
 *   0 = OK, -1 = error
 */
int crypto_encrypt(const uint8_t *plaintext, size_t len, encrypted_data_t *out);

/**
 * Odszyfruj dane (AES-128-CTR) + sprawdź HMAC
 * 
 * Args:
 *   encrypted: dane zaszyfrowane (IV + ciphertext + MIC)
 *   ciphertext_len: długość ciphertext
 *   out_plaintext: bufor dla odszyfrowanych danych
 *   out_len: długość odszyfrowanych danych
 * 
 * Return:
 *   0 = OK (HMAC verified)
 *   -1 = HMAC verification failed (tampering!)
 *   -2 = replay attack detected
 *   -3 = other error
 */
int crypto_decrypt(const encrypted_data_t *encrypted, uint16_t ciphertext_len,
                   uint8_t *out_plaintext, size_t *out_len);

/**
 * Self-test (optional)
 */
void crypto_self_test(void);

#endif // CRYPTO_LAYER_H
