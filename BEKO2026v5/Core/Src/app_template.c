/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
    (C)2013 Semtech

Description: BEKO antenna rotor control — radio application layer
             (refactored from Ping-Pong template)

License: Revised BSD License, see LICENSE.TXT file include in the project
*/

#include <string.h>
#include "board.h"
#include "radio.h"
#include "app_template.h"
#include "lcd.h"
#include "stm32u5xx_hal.h"
#include "serwo.h"
#include "crypto_layer.h"

/* ============================================================================
 * RADIO PHYSICAL LAYER CONFIGURATION
 * ============================================================================ */

#define RF_FREQUENCY        868000000   /**< Carrier frequency in Hz (868 MHz ISM) */
#define TX_OUTPUT_POWER     17          /**< Transmit power in dBm                 */

#if defined(USE_MODEM_LORA)

#define LORA_BANDWIDTH              0   /**< 0: 125 kHz, 1: 250 kHz, 2: 500 kHz   */
#define LORA_SPREADING_FACTOR       7   /**< SF7 – SF12                            */
#define LORA_CODINGRATE             1   /**< 1: 4/5, 2: 4/6, 3: 4/7, 4: 4/8      */
#define LORA_PREAMBLE_LENGTH        8   /**< Preamble length in symbols            */
#define LORA_SYMBOL_TIMEOUT         5   /**< RX timeout in symbols                 */
#define LORA_FIX_LENGTH_PAYLOAD_ON  false
#define LORA_IQ_INVERSION_ON        false

#elif defined(USE_MODEM_FSK)

/** FSK deviation: h = 2*FDEV/BR = 1.0 → FDEV = BR/2 */
#define FSK_FDEV                4800   /**< Frequency deviation in Hz (4.8 kHz)   */
#define FSK_DATARATE            4800   /**< Bit rate in bps (4.8 kbps)            */
#define FSK_BANDWIDTH           9600   /**< RX filter bandwidth in Hz (9.6 kHz)   */
#define FSK_AFC_BANDWIDTH       9600   /**< AFC bandwidth in Hz                   */
#define FSK_PREAMBLE_LENGTH     10     /**< Preamble length in bytes              */
#define FSK_FIX_LENGTH_PAYLOAD_ON false

#else
    #error "Please define a modem (USE_MODEM_LORA or USE_MODEM_FSK) in compiler options."
#endif

/* ============================================================================
 * PROTOCOL CONSTANTS
 * ============================================================================ */

#define RX_TIMEOUT_VALUE    1000        /**< RX timeout in milliseconds           */
#define BUFFER_SIZE         64          /**< Radio RX/TX buffer size in bytes     */

/** BEKO frame type identifiers. */
#define FRAME_TYPE_CMD      0x01        /**< Command: azimuth set-point           */
#define FRAME_TYPE_TELEM    0x02        /**< Telemetry: actual ADC/angle reading  */
#define FRAME_TYPE_ALARM    0x03        /**< Alarm: anomaly report from STM32     */
#define FRAME_TYPE_ACK      0x04        /**< Acknowledgement                      */

/** Node address identifiers. */
#define ADDR_CENTRAL        0x01        /**< Central station (Raspberry Pi)       */
#define ADDR_NODE1          0x02        /**< Execution node 1 (STM32)            */

/**
 * TX/RX role selector — compile-time switch.
 *   1 → STM32 transmits (used for TX integration tests)
 *   0 → STM32 receives  (normal operational mode)
 */
#define TX_RX_TOGGLE        0

/* ============================================================================
 * BEKO FRAME DEFINITION
 * ============================================================================ */

/**
 * @brief Wire format of a BEKO protocol frame.
 *        __attribute__((packed)) ensures no compiler-inserted padding.
 */
typedef struct __attribute__((packed)) {
    uint8_t  type;          /**< Frame type (FRAME_TYPE_*)                       */
    uint16_t counter;       /**< Sequence counter — replay protection (big-endian)*/
    uint8_t  flags;         /**< Status / control flags                          */
    uint8_t  data[32];      /**< Encrypted payload (IV + ciphertext + MIC)       */
    uint8_t  data_len;      /**< Number of valid bytes used in data[]            */
    uint16_t crc;           /**< CRC-16/CCITT over fields before this one        */
    uint8_t  dst;           /**< Destination node address                        */
    uint8_t  src;           /**< Source node address                             */
} beko_frame_t;

/* ============================================================================
 * RADIO STATE MACHINE
 * ============================================================================ */

/** Application-level radio FSM states. */
typedef enum {
    LOWPOWER,
    RX,
    RX_DONE,
    RX_TIMEOUT,
    RX_ERROR,
    TX,
    TX_TIMEOUT,
} radio_state_t;

static radio_state_t State = LOWPOWER;

/* ============================================================================
 * MODULE-LEVEL VARIABLES
 * ============================================================================ */

static volatile int8_t  RssiValue  = 0;
static volatile int8_t  SnrValue   = 0;

static uint16_t BufferSize = BUFFER_SIZE;
static uint8_t  Buffer[BUFFER_SIZE];

/** Diagnostic counters for radio event monitoring. */
typedef struct {
    int rx_done;
    int rx_timeout;
    int rx_error;
    int tx_done;
    int tx_timeout;
} radio_event_counters_t;

static radio_event_counters_t radio_events = {0};

/* ============================================================================
 * RADIO EVENT CALLBACKS — forward declarations
 * ============================================================================ */

static void OnTxDone(void);
static void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr);
static void OnTxTimeout(void);
static void OnRxTimeout(void);
static void OnRxError(void);

/* ============================================================================
 * PRIVATE HELPERS
 * ============================================================================ */

/**
 * @brief Computes CRC-16/CCITT-FALSE over a byte buffer.
 *        Polynomial: 0x1021, initial value: 0xFFFF.
 *
 * @param buf  Input buffer.
 * @param len  Number of bytes to process.
 * @return     16-bit CRC value.
 */
static uint16_t calc_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

/**
 * @brief Serialises a BEKO frame into a flat byte buffer ready for Radio.Send().
 *        CRC is computed over all fields preceding the crc field in the struct.
 *
 * @param out       Destination buffer — must be at least sizeof(beko_frame_t) bytes.
 * @param type      Frame type (FRAME_TYPE_*).
 * @param counter   Sequence counter value.
 * @param flags     Frame flags byte.
 * @param data      Pointer to payload bytes (may be NULL if data_len == 0).
 * @param data_len  Number of bytes to copy from data[] (capped at 32).
 * @param dst       Destination address.
 * @param src       Source address.
 * @return          Number of bytes written to out (always sizeof(beko_frame_t)).
 */
static uint8_t build_frame(uint8_t *out,
                            uint8_t type, uint16_t counter, uint8_t flags,
                            const uint8_t *data, uint8_t data_len,
                            uint8_t dst, uint8_t src)
{
    beko_frame_t f;
    memset(&f, 0, sizeof(f));

    f.type     = type;
    f.counter  = counter;
    f.flags    = flags;
    f.data_len = data_len;
    f.dst      = dst;
    f.src      = src;

    if (data && data_len) {
        uint8_t copy_len = (data_len < 32) ? data_len : 32;
        memcpy(f.data, data, copy_len);
    }

    /* CRC covers every field that precedes crc in the packed struct. */
    f.crc = calc_crc16((uint8_t *)&f, (uint16_t)offsetof(beko_frame_t, crc));

    memcpy(out, &f, sizeof(f));
    return (uint8_t)sizeof(beko_frame_t);
}

/**
 * @brief Parses and validates an inbound raw frame buffer.
 *        On success, decrypts the payload using the crypto layer and prints the result.
 *        Frames with invalid length, failed CRC, wrong destination, or insufficient
 *        encrypted payload are silently discarded with a warning log.
 *
 * @param buf      Received byte buffer.
 * @param buf_len  Number of bytes received.
 */
static void parse_frame(uint8_t *buf, uint16_t buf_len)
{
    printf("\r\n--- Frame received (%d B) ---\r\n", buf_len);

    /* Sanity check: buffer must be at least one complete frame. */
    if (buf_len < sizeof(beko_frame_t)) {
        printf("WARN: Frame too short, discarding.\r\n");
        return;
    }

    beko_frame_t *f = (beko_frame_t *)buf;

    /* CRC integrity check. */
    uint16_t crc_calc = calc_crc16(buf, (uint16_t)offsetof(beko_frame_t, crc));
    if (crc_calc != f->crc) {
        printf("WARN: CRC mismatch (received=0x%04X, computed=0x%04X) — discarding.\r\n",
               f->crc, crc_calc);
        return;
    }

    printf("type=0x%02X  seq=%d  flags=0x%02X  CRC=OK\r\n",
           f->type, f->counter, f->flags);
    printf("dst=0x%02X  src=0x%02X  enc_len=%d\r\n",
           f->dst, f->src, f->data_len);

    /* Address filter: accept only frames addressed to this node. */
    if (f->dst != ADDR_NODE1) {
        printf("WARN: Frame not addressed to this node — discarding.\r\n");
        return;
    }

    /*
     * Minimum encrypted payload: 16 B (IV) + ≥1 B (ciphertext) + CRYPTO_MIC_SIZE.
     * Guard against underflow before computing enc.ciphertext_len.
     */
    if (f->data_len < (16 + 1 + CRYPTO_MIC_SIZE)) {
        printf("WARN: Encrypted payload too short (%d B) — discarding.\r\n", f->data_len);
        return;
    }

    /* Unpack the encrypted_data_t from the frame's data field. */
    encrypted_data_t enc;
    memcpy(enc.iv,         f->data,                                   16);
    enc.ciphertext_len   = f->data_len - 16 - CRYPTO_MIC_SIZE;
    memcpy(enc.ciphertext, f->data + 16,                              enc.ciphertext_len);
    memcpy(enc.mic,        f->data + 16 + enc.ciphertext_len,         CRYPTO_MIC_SIZE);

    /* Decrypt and verify. */
    uint8_t plaintext[CRYPTO_MAX_DATA];
    size_t  plaintext_len = 0;

    int ret = crypto_decrypt(&enc, enc.ciphertext_len, plaintext, &plaintext_len);
    switch (ret) {
        case  0:
            plaintext[plaintext_len] = '\0';
            printf("DECRYPT OK: \"%s\"\r\n", (char *)plaintext);
            break;
        case -1: printf("DECRYPT FAIL: MIC mismatch\r\n");    break;
        case -2: printf("DECRYPT FAIL: Replay detected\r\n"); break;
        default: printf("DECRYPT FAIL: err=%d\r\n", ret);     break;
    }
}

/* ============================================================================
 * PUBLIC APPLICATION LOOPS
 * ============================================================================ */

/**
 * @brief TX integration test loop — continuously encrypts and transmits a test
 *        command frame to the central station. Runs indefinitely.
 *
 * Packet content: "AZI=090" encrypted with AES-128-CTR + MIC.
 * Frame addressed from ADDR_NODE1 → ADDR_CENTRAL.
 */
void tx_loop(void)
{
    static const uint8_t aes_key[CRYPTO_KEY_SIZE] = {
        0xAE, 0x68, 0x52, 0xF8, 0x12, 0x10, 0x67, 0xCC,
        0x4B, 0xF7, 0xA5, 0x76, 0x55, 0x77, 0xF3, 0x9E
    };

    crypto_init(aes_key);

    uint8_t  tx_buf[128];
    uint16_t seq = 0;

    printf("TX loop started (with encryption)\r\n");

    while (1) {
        /* Payload: azimuth command string. */
        const uint8_t plaintext[]   = "AZI=090";
        const uint8_t plaintext_len = (uint8_t)(sizeof(plaintext) - 1); /* exclude '\0' */

        encrypted_data_t enc;
        if (crypto_encrypt(plaintext, plaintext_len, &enc) != 0) {
            printf("Encryption error — retrying in 3 s\r\n");
            HAL_Delay(3000);
            continue;
        }

        /*
         * Pack the encrypted packet: IV (16 B) || ciphertext || MIC (4 B).
         * Total size is always 16 + ciphertext_len + CRYPTO_MIC_SIZE.
         */
        uint8_t enc_buf[16 + CRYPTO_MAX_DATA + CRYPTO_MIC_SIZE];
        uint8_t enc_buf_len = 0;

        memcpy(enc_buf + enc_buf_len, enc.iv,         16);
        enc_buf_len += 16;
        memcpy(enc_buf + enc_buf_len, enc.ciphertext, enc.ciphertext_len);
        enc_buf_len += enc.ciphertext_len;
        memcpy(enc_buf + enc_buf_len, enc.mic,        CRYPTO_MIC_SIZE);
        enc_buf_len += CRYPTO_MIC_SIZE;

        printf("TX: seq=%d  enc_len=%d (IV + CT + MIC)\r\n", seq, enc_buf_len);

        uint8_t frame_len = build_frame(tx_buf,
                                        FRAME_TYPE_CMD,
                                        seq++,
                                        0x00,
                                        enc_buf, enc_buf_len,
                                        ADDR_CENTRAL,
                                        ADDR_NODE1);

        Radio.Send(tx_buf, frame_len);
        HAL_Delay(3000);
    }
}

/**
 * @brief RX operational loop — continuously listens for inbound frames and
 *        dispatches them to parse_frame(). Restarts RX on timeout or error.
 *        Runs indefinitely.
 */
void rx_loop(void)
{
    static const uint8_t aes_key[CRYPTO_KEY_SIZE] = {
        0xAE, 0x68, 0x52, 0xF8, 0x12, 0x10, 0x67, 0xCC,
        0x4B, 0xF7, 0xA5, 0x76, 0x55, 0x77, 0xF3, 0x9E
    };

    crypto_init(aes_key);

    printf("RX loop started\r\n");
    Radio.Rx(0); /* Start continuous reception. */

    while (1) {
        DelayMs(25);

        if (State == RX_TIMEOUT) {
            printf("RX timeout — restarting receiver...\r\n");
            Radio.Rx(0);
            State = RX;
        }

        if (State == RX_DONE) {
            State = RX;
            parse_frame(Buffer, BufferSize);
            Radio.Rx(0);
        }
    }
}

/* ============================================================================
 * APPLICATION ENTRY POINT
 * ============================================================================ */

/**
 * @brief Radio application main entry point.
 *        Initialises the board, configures the radio modem, then enters
 *        either tx_loop() or rx_loop() depending on TX_RX_TOGGLE.
 *        Never returns under normal operation.
 */
void app_main(void)
{
    /* Board and peripheral bring-up. */
    BoardInitMcu();
    BoardInitPeriph();

    /* Wire up radio event callbacks. */
    RadioEvents_t RadioEvents;
    RadioEvents.TxDone    = OnTxDone;
    RadioEvents.RxDone    = OnRxDone;
    RadioEvents.TxTimeout = OnTxTimeout;
    RadioEvents.RxTimeout = OnRxTimeout;
    RadioEvents.RxError   = OnRxError;

    Radio.Init(&RadioEvents);
    Radio.SetChannel(RF_FREQUENCY);

#if defined(USE_MODEM_LORA)

    Radio.SetTxConfig(MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH,
                      LORA_SPREADING_FACTOR, LORA_CODINGRATE,
                      LORA_PREAMBLE_LENGTH, LORA_FIX_LENGTH_PAYLOAD_ON,
                      true, 0, 0, LORA_IQ_INVERSION_ON, 3000);

    Radio.SetRxConfig(MODEM_LORA, LORA_BANDWIDTH, LORA_SPREADING_FACTOR,
                      LORA_CODINGRATE, 0, LORA_PREAMBLE_LENGTH,
                      LORA_SYMBOL_TIMEOUT, LORA_FIX_LENGTH_PAYLOAD_ON,
                      0, true, 0, 0, LORA_IQ_INVERSION_ON, true);

#elif defined(USE_MODEM_FSK)

    Radio.SetTxConfig(
        MODEM_FSK,                   /* Modem: FSK                               */
        TX_OUTPUT_POWER,             /* Output power [dBm]                       */
        FSK_FDEV,                    /* Frequency deviation [Hz]                 */
        0,                           /* Bandwidth — N/A for FSK                  */
        FSK_DATARATE,                /* Bit rate [bps]                           */
        0,                           /* Coding rate — N/A for FSK               */
        FSK_PREAMBLE_LENGTH,         /* Preamble length [bytes]                  */
        FSK_FIX_LENGTH_PAYLOAD_ON,   /* Variable-length packets                  */
        true,                        /* CRC enabled                              */
        0,                           /* Freq. hopping — N/A for FSK             */
        0,                           /* Hop period — N/A for FSK               */
        0,                           /* IQ inversion — N/A for FSK             */
        3000                         /* TX timeout [ms]                          */
    );

    Radio.SetRxConfig(
        MODEM_FSK,                   /* Modem: FSK                               */
        FSK_BANDWIDTH,               /* Single-side bandwidth [Hz]               */
        FSK_DATARATE,                /* Bit rate [bps]                           */
        0,                           /* Coding rate — N/A for FSK               */
        FSK_AFC_BANDWIDTH,           /* AFC bandwidth [Hz]                       */
        FSK_PREAMBLE_LENGTH,         /* Preamble length [bytes]                  */
        0,                           /* Symbol timeout — N/A for FSK           */
        FSK_FIX_LENGTH_PAYLOAD_ON,   /* Variable-length packets                  */
        0,                           /* Payload length (variable mode → 0)       */
        true,                        /* CRC enabled                              */
        0,                           /* Freq. hopping — N/A for FSK             */
        0,                           /* Hop period — N/A for FSK               */
        false,                       /* IQ inversion — N/A for FSK             */
        true                         /* Continuous reception mode                */
    );

#else
    #error "Please define a frequency band in the compiler options."
#endif

    /* Select role: transmitter (test) or receiver (operational). */
#if TX_RX_TOGGLE
    tx_loop();
#else
    rx_loop();
#endif

    /* Execution should never reach this point. */
    while (1) {
        printf("FATAL: app_main() returned unexpectedly.\r\n");
    }
}

/* ============================================================================
 * RADIO EVENT CALLBACKS
 * ============================================================================ */

/**
 * @brief Called by the radio driver when a transmission completes successfully.
 */
static void OnTxDone(void)
{
    Radio.Sleep();
    State = TX;
    radio_events.tx_done++;
}

/**
 * @brief Called by the radio driver when a frame has been received.
 *
 * @param payload  Pointer to received bytes (driver-managed buffer).
 * @param size     Number of bytes received.
 * @param rssi     Received signal strength indicator in dBm.
 * @param snr      Signal-to-noise ratio (LoRa only; meaningless for FSK).
 */
static void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr)
{
    printf("OnRxDone: %d B, RSSI=%d\r\n", size, rssi);

    BufferSize = size;
    memcpy(Buffer, payload, BufferSize);
    RssiValue = rssi;
    SnrValue  = snr;
    State     = RX_DONE;

    radio_events.rx_done++;
}

/**
 * @brief Called by the radio driver when a TX operation times out.
 */
static void OnTxTimeout(void)
{
    Radio.Sleep();
    State = TX_TIMEOUT;
    radio_events.tx_timeout++;
}

/**
 * @brief Called by the radio driver when an RX window expires without a frame.
 */
static void OnRxTimeout(void)
{
    State = RX_TIMEOUT;
    radio_events.rx_timeout++;
}

/**
 * @brief Called by the radio driver when a reception error is detected
 *        (e.g. CRC error at the physical layer).
 *        Immediately restarts the receiver.
 */
static void OnRxError(void)
{
    printf("OnRxError — restarting receiver.\r\n");
    State = RX_ERROR;
    radio_events.rx_error++;
    Radio.Rx(0);
}