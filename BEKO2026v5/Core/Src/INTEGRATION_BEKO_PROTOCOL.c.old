/**
 * ============================================================================
 * INTEGRATION: BEKO Protocol Frame (AES-128-CTR Encryption)
 * ============================================================================
 * 
 * Frame Format (ustalone parametry - 74 bity / 10 bajtów + szyfrowanie):
 * 
 * PLAINTEXT HEADER (10 bajtów - wysyłane w plaintext):
 *   [Typ(4b) + Flagi(4b)] [Licznik(16b)] [Payload(16b)] 
 *   [CRC(16b)] [Adresat(4b) + Nadawca(4b)] [Reserved(16b)]
 * 
 * ENCRYPTED (2 bajty payload + 4 bajty MIC):
 *   [Ciphertext(16b)] [MIC(32b)]
 * 
 * Total: 10 + 2 + 4 = 16 bajów na radio
 * 
 * ============================================================================
 */

/* ========== ZMIANA 1: INCLUDE ========== */

// Na górze app_template.c:
#include "crypto_layer_cmox.h"
#include "BEKO_PROTOCOL_FRAME_FORMAT.h"

/* ========== ZMIANA 2: PAYLOAD STRUCTURES ========== */

// Struktura payloadu (będzie szyfrowana)
typedef struct {
    uint8_t azimuth;        // Położenie docelowe (0-180°)
    uint8_t flags;          // Flagi sterowania / status
} plaintext_payload_t;

// Buffer na odbiór ramki radiowej
uint8_t rx_buffer[16];      // 16 bajtów: header(10) + ciphertext(2) + mic(4)

/* ========== ZMIANA 3: INITIALIZATION ========== */

void app_main(void)
{
    BoardInitMcu();
    BoardInitPeriph();
    
    // ===== INITIALIZE CRYPTO =====
    const uint8_t shared_key[16] = {
        0xAE, 0x68, 0x52, 0xF8, 0x12, 0x10, 0x67, 0xCC,
        0x4B, 0xF7, 0xA5, 0x76, 0x55, 0x77, 0xF3, 0x9E
    };
    
    if (crypto_init(shared_key) != 0) {
        printf("ERROR: Crypto init failed\r\n");
        while(1);
    }
    
    crypto_self_test();
    printf("INFO: BEKO Protocol ready (AES-128-CTR + BEKO Frame)\r\n");
    
    // Radio init...
    RadioEvents.RxDone = OnRxDone;
    Radio.Init(&RadioEvents);
    Radio.SetChannel(RF_FREQUENCY);
    
    // ===== BEKO CONFIG =====
    // My node ID and other params would be configured here
    
    // ... rest of init ...
}

/* ========== ZMIANA 4: OnRxDone() ========== */

void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr)
{
    // ===== VALIDATE FRAME SIZE =====
    if (size != sizeof(encrypted_frame_t)) {
        printf("ERROR: Invalid frame size: %d (expected %d)\r\n",
               size, (int)sizeof(encrypted_frame_t));
        State = RX_ERROR;
        trx_events_cnt.rxerror++;
        return;
    }
    
    // ===== CAST TO ENCRYPTED_FRAME =====
    encrypted_frame_t *rx_frame = (encrypted_frame_t *)payload;
    
    // ===== VERIFY FRAME HEADER (plaintext) =====
    frame_header_t *hdr = &rx_frame->header;
    
    // Check if frame is for us
    if (hdr->destination != NODE_ID_STM32_1) {
        printf("INFO: Frame not for me (dest=%d)\r\n", hdr->destination);
        State = RX;
        return;
    }
    
    // Verify CRC of header
    if (!crc16_verify((uint8_t*)hdr, 8, hdr->crc)) {
        printf("ERROR: Header CRC failed\r\n");
        State = RX_ERROR;
        trx_events_cnt.rxerror++;
        return;
    }
    
    printf("INFO: Received frame - Type=0x%X, Counter=%u, From=%d, RSSI=%d\r\n",
           hdr->type, hdr->counter, hdr->source, rssi);
    
    // ===== DECRYPT AND VERIFY =====
    uint8_t plaintext[2];
    uint16_t plain_len;
    
    int decrypt_result = crypto_decrypt(rx_frame, plaintext, &plain_len);
    
    if (decrypt_result == -1) {
        printf("!!! ALARM: MIC VERIFICATION FAILED !!!\r\n");
        printf("!!! POSSIBLE ATTACK DETECTED !!!\r\n");
        State = RX_ERROR;
        trx_events_cnt.rxerror++;
        return;
    }
    else if (decrypt_result == -2) {
        printf("!!! ALARM: REPLAY ATTACK DETECTED !!!\r\n");
        printf("!!! Attacker trying to reuse frame counter=%u !!!\r\n", hdr->counter);
        State = RX_ERROR;
        trx_events_cnt.rxerror++;
        return;
    }
    else if (decrypt_result < 0) {
        printf("ERROR: Decryption failed: %d\r\n", decrypt_result);
        State = RX_ERROR;
        trx_events_cnt.rxerror++;
        return;
    }
    
    // ===== PROCESS DECRYPTED PAYLOAD =====
    uint8_t azimuth = plaintext[0];
    uint8_t payload_flags = plaintext[1];
    
    printf("DECRYPTED: azimuth=%d°, flags=0x%02X (frame_counter=%u)\r\n",
           azimuth, payload_flags, hdr->counter);
    
    // ===== HANDLE FRAME TYPE =====
    switch (hdr->type) {
        case FRAME_TYPE_CMD:
            printf("COMMAND: Move to azimuth %d°\r\n", azimuth);
            // Send servo to position
            uint32_t pwm = degrees_to_us(azimuth);
            __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, pwm);
            // Wait and verify with potentiometer
            DelayMs(1500);
            uint8_t actual = raw_to_degrees(pot_get_raw());
            printf("FEEDBACK: Actual azimuth = %d°\r\n", actual);
            break;
            
        case FRAME_TYPE_TELEMETRY:
            printf("TELEMETRY REQUEST\r\n");
            // Send back telemetry
            tx_telemetry(hdr->source);
            break;
            
        case FRAME_TYPE_ALARM:
            printf("ALARM from node %d: flags=0x%02X\r\n", hdr->source, payload_flags);
            break;
            
        case FRAME_TYPE_ACK:
            printf("ACK from node %d\r\n", hdr->source);
            break;
            
        default:
            printf("ERROR: Unknown frame type 0x%X\r\n", hdr->type);
    }
    
    RssiValue = rssi;
    SnrValue = snr;
    State = RX_DONE;
    trx_events_cnt.rxdone++;
}

/* ========== ZMIANA 5: TRANSMIT COMMAND ========== */

/**
 * Send encrypted command to target node
 */
void tx_command(uint8_t target_node, uint8_t azimuth, uint8_t flags)
{
    // ===== PREPARE PLAINTEXT PAYLOAD =====
    plaintext_payload_t plaintext = {
        .azimuth = azimuth,
        .flags = flags
    };
    
    // ===== ENCRYPT PAYLOAD =====
    encrypted_frame_t enc_frame;
    if (crypto_encrypt((uint8_t *)&plaintext, 2, &enc_frame) != 0) {
        printf("ERROR: Encryption failed\r\n");
        return;
    }
    
    // ===== PREPARE FRAME HEADER (plaintext) =====
    frame_header_t hdr;
    hdr.type = FRAME_TYPE_CMD;
    hdr.flags = FLAG_ACK_REQUEST;  // Request acknowledgment
    hdr.counter = crypto_get_tx_counter() - 1;  // From crypto layer
    hdr.destination = target_node;
    hdr.source = NODE_ID_RPi_CENTRAL;
    hdr.payload[0] = plaintext.azimuth;
    hdr.payload[1] = plaintext.flags;
    hdr.crc = crc16_calculate((uint8_t*)&hdr, 8);
    memset(hdr.reserved, 0, 2);
    
    // Copy header to encrypted frame
    enc_frame.header = hdr;
    
    // ===== TRANSMIT =====
    printf("TX CMD: node=%d, azimuth=%d°, ctr=%u, size=%d\r\n",
           target_node, azimuth, hdr.counter, (int)sizeof(encrypted_frame_t));
    
    Radio.Send((uint8_t *)&enc_frame, sizeof(encrypted_frame_t));
}

/* ========== ZMIANA 6: TRANSMIT TELEMETRY ========== */

/**
 * Send encrypted telemetry to target node
 */
void tx_telemetry(uint8_t target_node)
{
    uint8_t actual_azimuth = raw_to_degrees(pot_get_raw());
    
    plaintext_payload_t plaintext = {
        .azimuth = actual_azimuth,
        .flags = 0x00
    };
    
    encrypted_frame_t enc_frame;
    if (crypto_encrypt((uint8_t *)&plaintext, 2, &enc_frame) != 0) {
        printf("ERROR: Telemetry encryption failed\r\n");
        return;
    }
    
    frame_header_t hdr;
    hdr.type = FRAME_TYPE_TELEMETRY;
    hdr.flags = 0x00;
    hdr.counter = crypto_get_tx_counter() - 1;
    hdr.destination = target_node;
    hdr.source = NODE_ID_STM32_1;
    hdr.payload[0] = plaintext.azimuth;
    hdr.payload[1] = plaintext.flags;
    hdr.crc = crc16_calculate((uint8_t*)&hdr, 8);
    memset(hdr.reserved, 0, 2);
    
    enc_frame.header = hdr;
    
    printf("TX TELEM: azimuth=%d°, node=%d\r\n", actual_azimuth, target_node);
    Radio.Send((uint8_t *)&enc_frame, sizeof(encrypted_frame_t));
}

/* ========== ZMIANA 7: HELPER FUNCTIONS ========== */

uint8_t raw_to_degrees(uint32_t raw)
{
    if (raw < POT_MIN_RAW) raw = POT_MIN_RAW;
    if (raw > POT_MAX_RAW) raw = POT_MAX_RAW;
    return ((raw - POT_MIN_RAW) * 180) / (POT_MAX_RAW - POT_MIN_RAW);
}

uint32_t degrees_to_us(uint8_t degrees)
{
    if (degrees > 180) degrees = 180;
    return 500 + ((uint32_t)degrees * 2000 / 180);
}

/**
 * CRC-16 implementation (CCITT polynomial 0x1021)
 */
uint16_t crc16_calculate(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i] << 8);
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

int crc16_verify(const uint8_t *data, size_t len, uint16_t expected_crc)
{
    uint16_t calculated = crc16_calculate(data, len);
    return (calculated == expected_crc) ? 1 : 0;
}

/* ========== PODSUMOWANIE ========== */

/*
 * ZMIENIONE STRUKTURY:
 * ✅ frame_header_t – 10 bajtów (typ, licznik, flagi, payload, CRC, adresy)
 * ✅ encrypted_frame_t – 16 bajtów (header + ciphertext + mic)
 * ✅ plaintext_payload_t – 2 bajty (azymut + flags)
 * 
 * NOWE FUNKCJE:
 * ✅ tx_command()      – send encrypted command
 * ✅ tx_telemetry()    – send encrypted telemetry
 * ✅ crc16_calculate() – CRC for frame header
 * ✅ crc16_verify()    – Verify frame integrity
 * ✅ raw_to_degrees()  – Convert ADC to angle
 * ✅ degrees_to_us()   – Convert angle to PWM
 * 
 * ZMIENIONY OnRxDone():
 * ✅ Validate frame header (CRC, destination)
 * ✅ Decrypt payload + verify MIC
 * ✅ Process by frame type (CMD/TELEM/ALARM/ACK)
 * 
 * FRAME PROTOCOL:
 * ✅ FRAME_TYPE_CMD (0x1)       – Command frame
 * ✅ FRAME_TYPE_TELEMETRY (0x2) – Telemetry frame
 * ✅ FRAME_TYPE_ALARM (0x3)     – Alarm frame
 * ✅ FRAME_TYPE_ACK (0x4)       – Acknowledgment
 * 
 * FRAME SIZE:
 * ✅ 16 bytes per radio transmission
 * ✅ 10B header (plaintext) + 2B ciphertext + 4B MIC
 */
