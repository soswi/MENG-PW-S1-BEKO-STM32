/**
 * ============================================================================
 * crypto_layer_cmox.h
 * ============================================================================
 * Warstwa kryptografii dla systemu BEKO
 * 
 * Implementacja:
 *  - AES-128-CTR (szyfrowanie) – CMOX hardware-accelerated
 *  - HMAC-SHA256 (uwierzytelnienie)
 *  - Liczniki sekwencyjne (replay protection)
 * 
 * Biblioteka: CMOX (STMicroelectronics)
 * Platform: STM32U545RE
 * ============================================================================
 */

#ifndef CRYPTO_LAYER_CMOX_H
#define CRYPTO_LAYER_CMOX_H

#include <stdint.h>
#include <stddef.h>

/* ========== KONFIGURACJA ========== */

#define CRYPTO_KEY_SIZE         16      // AES-128 (128 bits)
#define CRYPTO_MIC_SIZE         4       // First 4 bytes of HMAC-SHA256
#define CRYPTO_PAYLOAD_SIZE     2       // Payload (azymut + flags)
#define CRYPTO_FRAME_COUNTER_INIT 0

/* ========== PARAMETRY RAMKI RADIOWEJ ========== */

// Struktura ramki (ustalone parametry z dokumentacji BEKO)
// Lp.  Pole        Rozmiar    Opis
// ──────────────────────────────────────────────────
// 1    Preambuła   10 bitów   Synchronizacja FSK (RFM95W)
// 2    Typ         4 bity     Typ ramki (CMD=1, TELEM=2, etc)
// 3    Licznik     16 bitów   Frame counter (replay protection)
// 4    Flagi       4 bity     Status flags + control
// 5    Dane        16 bitów   Payload (2 bajty)
// 6    CRC         16 bitów   Suma kontrolna
// 7    Adresat     4 bity     ID węzła docelowego
// 8    Nadawca     4 bity     ID węzła źródłowego
// ──────────────────────────────────────────────────
// SUMA: 74 bity (bez preambuły) = ~10 bajtów

#define FRAME_HEADER_SIZE       10      // Całkowita ramka: 10 bajtów
#define FRAME_TYPE_CMD          0x1     // Typ: komenda sterująca
#define FRAME_TYPE_TELEMETRY    0x2     // Typ: telemetria
#define FRAME_TYPE_ALARM        0x3     // Typ: alarm/błąd

/* ========== STRUKTURY DANYCH ========== */

/**
 * Struktura nagłówka ramki radiowej (plaintext, nieszyfrowana)
 * 
 * Bezpośrednie mapowanie na ustalone parametry:
 * - Preambuła (10b): obsługiwana przez RFM95W
 * - Typ (4b): type field
 * - Licznik (16b): counter field
 * - Flagi (4b): flags field
 * - Dane (16b): payload (2B azymut + flags)
 * - CRC (16b): crc field
 * - Adresat (4b): destination (4 bity)
 * - Nadawca (4b): source (4 bity)
 */
typedef struct {
    // Byte 0: Typ (4b) + Flagi (4b)
    uint8_t type : 4;               // [4b] Typ ramki (FRAME_TYPE_*)
    uint8_t flags : 4;              // [4b] Flagi statusu
    
    // Bytes 1-2: Licznik sekwencyjny (replay protection)
    uint16_t counter;               // [16b] Frame counter
    
    // Bytes 3-4: Payload (dane użyteczne)
    uint8_t payload[2];             // [16b] Dane (azymut + flagi)
    
    // Bytes 5-6: CRC
    uint16_t crc;                   // [16b] CRC-16 weryfikacja integracji
    
    // Byte 7: Adresy (Adresat 4b + Nadawca 4b)
    uint8_t destination : 4;        // [4b] ID węzła docelowego
    uint8_t source : 4;             // [4b] ID węzła źródłowego
    
    // Byte 8-9: Wyrównanie (padding do 10 bajtów)
    uint8_t reserved[2];            // [16b] Zarezerwowane
} frame_header_t;

/**
 * Struktura zaszyfrowanej ramki (AES-128-CTR + HMAC-SHA256)
 * 
 * Workflow szyfrowania:
 * 1. Przygotuj frame_header_t (plaintext) – 10 bajtów
 * 2. Ekstrahuj payload[2] z frame_header.payload
 * 3. Szyfruj payload (2B) → ciphertext (2B, CTR bez padding)
 * 4. Wylicz HMAC-SHA256(frame_header + ciphertext)[:4]
 * 5. Wyślij [frame_header] + [ciphertext] + [mic]
 * 
 * Layout radiowy (razem 16 bajtów):
 *   [Frame Header: 10B] – plaintext (typ, licznik, flagi, CRC, adresy)
 *   [Ciphertext: 2B] – Encrypted payload (CTR mode)
 *   [MIC: 4B] – HMAC-SHA256[:4] Authentication tag
 *   = 10 + 2 + 4 = 16 bajtów
 */
typedef struct {
    frame_header_t header;          // [10B] Nagłówek ramki (plaintext)
    uint8_t ciphertext[2];          // [2B] Szyfrowany payload
    uint8_t mic[CRYPTO_MIC_SIZE];   // [4B] Message Integrity Code
} encrypted_frame_t;

/**
 * Kontekst kryptograficzny – przechowuje klucze i stan licznika
 */
typedef struct {
    uint8_t key[CRYPTO_KEY_SIZE];           // Shared AES-128 key
    uint32_t tx_frame_counter;              // Licznik wychodzący (16-bitowy aktywny)
    uint32_t rx_frame_counter_last;         // Ostatni zaakceptowany licznik
    uint8_t initialized;                    // Flaga inicjalizacji
} crypto_context_t;

/* ========== API FUNKCJI ========== */

/**
 * Inicjalizacja warstwy kryptografii (CMOX)
 * Musi być wywołana raz przy starcie systemu
 * 
 * Args:
 *   key: wskaźnik na 16-bajtowy klucz AES (Shared Secret)
 * 
 * Return:
 *   0 na sukces, -1 na błąd
 */
int crypto_init(const uint8_t *key);

/**
 * Szyfrowanie plaintext (AES-128-CTR) i generowanie MIC
 * 
 * Args:
 *   plaintext: dane do szyfrowania
 *   plain_len: długość plaintext
 *   out_frame: wskaźnik na strukturę do przechowywania wyniku
 * 
 * Return:
 *   0 na sukces, -1 na błąd
 */
int crypto_encrypt(const uint8_t *plaintext, uint16_t plain_len,
                   encrypted_frame_t *out_frame);

/**
 * Odszyfrowanie ramki (AES-128-CTR) i weryfikacja MIC
 * 
 * Args:
 *   frame: struktura zaszyfrowanej ramki
 *   out_plaintext: bufor dla deszyfrowanego tekstu
 *   out_len: wskaźnik dla długości deszyfrowanego tekstu
 * 
 * Return:
 *   0 na sukces
 *   -1 na błąd MIC (MOŻLIWY ATAK!)
 *   -2 na błąd replay attack
 *   -3 na inne błędy
 */
int crypto_decrypt(const encrypted_frame_t *frame,
                   uint8_t *out_plaintext, uint16_t *out_len);

/**
 * Reset licznika sekwencyjnego (debug/test)
 */
void crypto_reset_counter(void);

/**
 * Pobranie aktualnego licznika TX
 */
uint32_t crypto_get_tx_counter(void);

/**
 * Test funkcjonalności
 */
void crypto_self_test(void);

#endif // CRYPTO_LAYER_CMOX_H
