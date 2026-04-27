/**
 * ============================================================================
 * BEKO PROTOCOL FRAME FORMAT (Updated)
 * ============================================================================
 * 
 * Established parameters from Projekt_BEKO.pdf, section 4.3
 * 
 * Frame Structure:
 * Lp.  Pole        Rozmiar    Opis
 * ──────────────────────────────────────────────────────────
 * 1    Preambuła   10 bitów   Synchronizacja FSK (RFM95W hardware)
 * 2    Typ         4 bity     Typ ramki (0x1=CMD, 0x2=TELEM, 0x3=ALARM)
 * 3    Licznik     16 bitów   Frame counter (replay attack protection)
 * 4    Flagi       4 bity     Status flags + control bits
 * 5    Dane        16 bitów   Payload (2 bytes: azymut + flagi)
 * 6    CRC         16 bitów   CRC-16 (data integrity)
 * 7    Adresat     4 bity     Destination node ID (0-15)
 * 8    Nadawca     4 bity     Source node ID (0-15)
 * ──────────────────────────────────────────────────────────
 * SUMA: 74 bity (bez 10-bitowej preambuły) = ~10 bajtów danych
 * 
 * ============================================================================
 * ENCRYPTION INTEGRATION
 * ============================================================================
 * 
 * Plaintext Frame (Header):
 *   [Type(4b) + Flags(4b) + Counter(16b) + Payload(16b) + 
 *    CRC(16b) + Dest(4b) + Source(4b)]
 *   = 10 bytes (HEADER - NOT ENCRYPTED)
 * 
 * Ciphertext Payload:
 *   [Encrypted Payload (2 bytes)] = AES-CTR(plaintext.payload, key, IV)
 *   = 2 bytes (ENCRYPTED)
 * 
 * Authentication:
 *   [MIC (4 bytes)] = HMAC-SHA256(header + ciphertext)[:4]
 *   = 4 bytes (HMAC tag)
 * 
 * Total Radio Frame:
 *   [Header (10B)] + [Ciphertext (2B)] + [MIC (4B)]
 *   = 16 bytes per radio transmission
 * 
 * ============================================================================
 * USAGE EXAMPLE
 * ============================================================================
 * 
 * Sending CMD_AZIMUTH from RPi to STM32:
 * 
 * 1. Create frame header (plaintext):
 *    frame_header_t hdr = {
 *        .type = FRAME_TYPE_CMD,      // 0x1
 *        .flags = 0x0,
 *        .counter = 1,                // First frame
 *        .payload = {0x5E, 0x00},     // 90° + flags
 *        .crc = calculate_crc16(...),
 *        .destination = 1,            // Node 1
 *        .source = 0                  // RPi (node 0)
 *    };
 * 
 * 2. Extract payload and encrypt:
 *    uint8_t plaintext[2] = {hdr.payload[0], hdr.payload[1]};
 *    encrypted_frame_t enc_frame;
 *    crypto_encrypt(plaintext, 2, &enc_frame);
 * 
 * 3. Prepare encrypted_frame_t for transmission:
 *    enc_frame.header = hdr;  // Copy plaintext header
 *    // enc_frame.ciphertext already set by crypto_encrypt()
 *    // enc_frame.mic already set by crypto_encrypt()
 * 
 * 4. Send over LoRa:
 *    Radio.Send((uint8_t*)&enc_frame, sizeof(encrypted_frame_t));
 * 
 * ============================================================================
 * RECEIVING
 * ============================================================================
 * 
 * On STM32 RX side:
 * 
 * 1. Receive frame:
 *    encrypted_frame_t rx_frame;
 *    // RxDone callback: memcpy(&rx_frame, payload, 16);
 * 
 * 2. Verify and decrypt:
 *    uint8_t plaintext[2];
 *    uint16_t plain_len;
 *    int result = crypto_decrypt(&rx_frame, plaintext, &plain_len);
 *    
 *    if (result == -1) ALARM("MIC failed - attack!");
 *    if (result == -2) ALARM("Replay attack!");
 *    if (result == 0) {
 *        uint8_t azimuth = plaintext[0];
 *        uint8_t flags = plaintext[1];
 *        // Process command
 *    }
 * 
 * 3. Verify frame header:
 *    frame_header_t hdr = rx_frame.header;
 *    if (hdr.type != FRAME_TYPE_CMD) ERROR("Wrong frame type");
 *    if (hdr.destination != MY_NODE_ID) ERROR("Not for me");
 *    if (!verify_crc16(hdr)) ERROR("CRC failed");
 * 
 * ============================================================================
 */

#ifndef BEKO_PROTOCOL_H
#define BEKO_PROTOCOL_H

#include <stdint.h>

/* Frame type identifiers */
#define FRAME_TYPE_CMD          0x1     // Sterująca
#define FRAME_TYPE_TELEMETRY    0x2     // Telemetria
#define FRAME_TYPE_ALARM        0x3     // Alarm/Error
#define FRAME_TYPE_ACK          0x4     // Potwierdzenie

/* Node IDs (4 bits: 0-15) */
#define NODE_ID_RPi_CENTRAL     0       // Raspberry Pi centrala
#define NODE_ID_STM32_1         1       // STM32 węzeł 1
#define NODE_ID_STM32_2         2       // STM32 węzeł 2 (future)
#define NODE_ID_STM32_N         15      // STM32 węzeł N (future)

/* Flag bits (within 4-bit flag field) */
#define FLAG_ACK_REQUEST        0x1     // Żądaj potwierdzenia
#define FLAG_ACK_RESPONSE       0x2     // To jest potwierdzenie
#define FLAG_ALARM              0x4     // Ramka alarmowa
#define FLAG_FAIL_SAFE          0x8     // Fail-safe mode

/* CRC-16 calculation (for header verification) */
uint16_t crc16_calculate(const uint8_t *data, size_t len);
int crc16_verify(const uint8_t *data, size_t len, uint16_t expected_crc);

#endif // BEKO_PROTOCOL_H
