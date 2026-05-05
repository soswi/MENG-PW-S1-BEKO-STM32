/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
    (C)2013 Semtech

Description: Ping-Pong implementation

License: Revised BSD License, see LICENSE.TXT file include in the project

Maintainer: Miguel Luis and Gregory Cristian
*/
#include <string.h>
#include "board.h"
#include "radio.h"
#include "app_template.h"
#include "lcd.h"
#include "stm32u5xx_hal.h"
#include "serwo.h"
#include "crypto_layer.h"

#define RF_FREQUENCY                                868000000 // Hz
#define TX_OUTPUT_POWER                             17         // dBm

#if defined( USE_MODEM_LORA )

#define LORA_BANDWIDTH                              0         // [0: 125 kHz,
                                                              //  1: 250 kHz,
                                                              //  2: 500 kHz,
                                                              //  3: Reserved]
#define LORA_SPREADING_FACTOR                       7         // [SF7..SF12]
#define LORA_CODINGRATE                             1         // [1: 4/5,
                                                              //  2: 4/6,
                                                              //  3: 4/7,
                                                              //  4: 4/8]
#define LORA_PREAMBLE_LENGTH                        8         // Same for Tx and Rx
#define LORA_SYMBOL_TIMEOUT                         5         // Symbols
#define LORA_FIX_LENGTH_PAYLOAD_ON                  false
#define LORA_IQ_INVERSION_ON                        false

#elif defined( USE_MODEM_FSK )

#define FSK_FDEV            						4800      // 4.8 kHz  (h = 2*FDEV/BR = 1.0)
#define FSK_DATARATE        						4800      // 4.8 kbps
#define FSK_BANDWIDTH       						9600      // 9.6 kHz
#define FSK_AFC_BANDWIDTH   						9600      // OK
#define FSK_PREAMBLE_LENGTH 						10        //
#define FSK_FIX_LENGTH_PAYLOAD_ON 					false

#else
    #error "Please define a modem in the compiler options."
#endif

typedef enum
{
    LOWPOWER,
    RX,
	RX_DONE,
    RX_TIMEOUT,
    RX_ERROR,
    TX,
    TX_TIMEOUT,
}States_t;

#define RX_TIMEOUT_VALUE                            1000
#define BUFFER_SIZE                                 64

// Frame types
#define FRAME_TYPE_CMD      0x01
#define FRAME_TYPE_TELEM    0x02
#define FRAME_TYPE_ALARM    0x03
#define FRAME_TYPE_ACK      0x04

// Nodes addresses
#define ADDR_CENTRAL        0x01       // Raspberry Pi
#define ADDR_NODE1          0x02       // STM32 node 1

// Flags

// ============================================================
#define TX_RX_TOGGLE    1   // 1 = STM32 nadaje, 0 = STM32 odbiera
// ============================================================

// Frame structure (packed — no padding)
typedef struct __attribute__((packed)) {
    uint8_t  type;          // Frame type (1B)
    uint16_t counter;       // Sequence counter (2B, big-endian)
    uint8_t  flags;         // Flags (1B)
    uint8_t  data[32];       // Payload (32B)
    uint8_t  data_len;      // Length of actual data in data[]
    uint16_t crc;           // CRC-16 (2B)
    uint8_t  dst;           // Destination address (1B)
    uint8_t  src;           // Source address (1B)
} beko_frame_t;

// Prosty CRC-16 (XOR kolejnych bajtów << 8, wystarczy na start)
static uint16_t calc_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

// Buduje ramkę do bufora tx, zwraca długość
static uint8_t build_frame(uint8_t *out, uint8_t type, uint16_t counter,
                            uint8_t flags, const uint8_t *data, uint8_t data_len,
                            uint8_t dst, uint8_t src)
{
    beko_frame_t f;
    memset(&f, 0, sizeof(f));
    f.type     = type;
    f.counter  = counter;
    f.flags    = flags;
    f.data_len = data_len;
    if (data && data_len)
        memcpy(f.data, data, data_len < 32 ? data_len : 32);
    f.dst = dst;
    f.src = src;
    f.crc = calc_crc16((uint8_t*)&f, offsetof(beko_frame_t, crc));
    memcpy(out, &f, sizeof(f));
    return (uint8_t)sizeof(beko_frame_t);
}

States_t State = LOWPOWER;

volatile int8_t RssiValue = 0;
volatile int8_t SnrValue = 0;


uint16_t BufferSize = BUFFER_SIZE;
uint8_t Buffer[BUFFER_SIZE];


typedef struct {
	int rxdone;
	int rxtimeout;
	int rxerror;
	int txdone;
	int txtimeout;
} trx_events_cnt_t;

trx_events_cnt_t trx_events_cnt;

int rx_cnt = 0;
int txdone_cnt = 0;


/*!
 * Radio events function pointer
 */
static RadioEvents_t RadioEvents;

/*!
 * \brief Function to be executed on Radio Tx Done event
 */
void OnTxDone( void );

/*!
 * \brief Function to be executed on Radio Rx Done event
 */
void OnRxDone( uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr );

/*!
 * \brief Function executed on Radio Tx Timeout event
 */
void OnTxTimeout( void );

/*!
 * \brief Function executed on Radio Rx Timeout event
 */
void OnRxTimeout( void );

/*!
 * \brief Function executed on Radio Rx Error event
 */
void OnRxError( void );




/**
 * Main application entry point.
 */
void app_main( void )
{
    // Target board initialisation
    BoardInitMcu( );
    BoardInitPeriph( );

    // Radio initialization
    RadioEvents.TxDone = OnTxDone;
    RadioEvents.RxDone = OnRxDone;
    RadioEvents.TxTimeout = OnTxTimeout;
    RadioEvents.RxTimeout = OnRxTimeout;
    RadioEvents.RxError = OnRxError;

    Radio.Init( &RadioEvents );

    Radio.SetChannel( RF_FREQUENCY );

#if defined( USE_MODEM_LORA )

    Radio.SetTxConfig( MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH,
                                   LORA_SPREADING_FACTOR, LORA_CODINGRATE,
                                   LORA_PREAMBLE_LENGTH, LORA_FIX_LENGTH_PAYLOAD_ON,
                                   true, 0, 0, LORA_IQ_INVERSION_ON, 3000 );
    
    Radio.SetRxConfig( MODEM_LORA, LORA_BANDWIDTH, LORA_SPREADING_FACTOR,
                                   LORA_CODINGRATE, 0, LORA_PREAMBLE_LENGTH,
                                   LORA_SYMBOL_TIMEOUT, LORA_FIX_LENGTH_PAYLOAD_ON,
                                   0, true, 0, 0, LORA_IQ_INVERSION_ON, true );

#elif defined( USE_MODEM_FSK )

    Radio.SetTxConfig(  MODEM_FSK,						/* Radio modem to be used [0: FSK, 1: LoRa] */
    					TX_OUTPUT_POWER,				/* Sets the output power [dBm] */
						FSK_FDEV,						/* Sets the frequency deviation (FSK only) [Hz] */
						0,								/* Sets the bandwidth (LoRa only); 0 for FSK */
                        FSK_DATARATE, 					/* Sets the Datarate. FSK: 600..300000 bits/s */
						0,								/* Sets the coding rate (LoRa only) FSK: N/A ( set to 0 ) */
                        FSK_PREAMBLE_LENGTH,			/* Sets the preamble length. FSK: Number of bytes */
						FSK_FIX_LENGTH_PAYLOAD_ON,		/* Fixed length packets [0: variable, 1: fixed] */
						true,							/* Enables disables the CRC [0: OFF, 1: ON] */
						0,								/* Enables disables the intra-packet frequency hopping. FSK: N/A ( set to 0 ) */
						0,								/* Number of symbols bewteen each hop. FSK: N/A ( set to 0 ) */
						0,								/* Inverts IQ signals (LoRa only). FSK: N/A ( set to 0 ) */
						3000							/* Transmission timeout [ms] */
	);
    
    Radio.SetRxConfig(  MODEM_FSK,						/* Radio modem to be used [0: FSK, 1: LoRa] */
    					FSK_BANDWIDTH,					/* Sets the bandwidth. FSK: >= 2600 and <= 250000 Hz. (CAUTION: This is "single side bandwidth") */
						FSK_DATARATE,					/* Sets the Datarate. FSK: 600..300000 bits/s */
						0,								/* Sets the coding rate (LoRa only) FSK: N/A ( set to 0 ) */
						FSK_AFC_BANDWIDTH,				/* Sets the AFC Bandwidth (FSK only). FSK: >= 2600 and <= 250000 Hz */
						FSK_PREAMBLE_LENGTH,			/* Sets the Preamble length. FSK: Number of bytes */
						0,								/* Sets the RxSingle timeout value (LoRa only). FSK: N/A ( set to 0 ) */
						FSK_FIX_LENGTH_PAYLOAD_ON,		/* Fixed length packets [0: variable, 1: fixed] */
						0,								/* Sets payload length when fixed lenght is used. */
						true,							/* Enables/Disables the CRC [0: OFF, 1: ON] */
                        0,								/* Enables disables the intra-packet frequency hopping. FSK: N/A ( set to 0 ) */
						0,								/* Number of symbols bewteen each hop. FSK: N/A ( set to 0 ) */
						false,							/* Inverts IQ signals (LoRa only). FSK: N/A ( set to 0 ) */
						true							/* Sets the reception in continuous mode. [false: single mode, true: continuous mode] */
	);

#else
    #error "Please define a frequency band in the compiler options."
#endif
    

    #if TX_RX_TOGGLE
        tx_loop();
    #else
        rx_loop();
    #endif

    while(1)
    {
    	printf("Infinite loop. This should never happen!\r\n");
    }
}

static void parse_frame(uint8_t *buf, uint16_t buf_len)
{
    printf("\r\n--- Odebrano ramke (%d B) ---\r\n", buf_len);

    if (buf_len < sizeof(beko_frame_t))
    {
        printf("WARN: za krotka ramka\r\n");
        return;
    }

    beko_frame_t *f = (beko_frame_t *)buf;

    uint16_t crc_calc = calc_crc16(buf, offsetof(beko_frame_t, crc));
    if (crc_calc != f->crc)
    {
        printf("WARN: CRC FAIL (recv=0x%04X calc=0x%04X)\r\n",
               f->crc, crc_calc);
        return;
    }

    printf("type=0x%02X  seq=%d  flags=0x%02X  crc=OK\r\n",
           f->type, f->counter, f->flags);
    printf("dst=0x%02X  src=0x%02X  enc_len=%d\r\n",
           f->dst, f->src, f->data_len);

    if (f->dst != ADDR_NODE1)
    {
        printf("WARN: ramka nie dla nas\r\n");
        return;
    }

    if (f->data_len < 21)
    {
        printf("WARN: enc_len za maly\r\n");
        return;
    }

    encrypted_data_t enc;
    memcpy(enc.iv,         f->data,      16);
    enc.ciphertext_len   = f->data_len - 16 - CRYPTO_MIC_SIZE;
    memcpy(enc.ciphertext, f->data + 16, enc.ciphertext_len);
    memcpy(enc.mic,        f->data + 16 + enc.ciphertext_len, CRYPTO_MIC_SIZE);

    uint8_t plaintext[CRYPTO_MAX_DATA];
    size_t  plaintext_len = 0;

    int ret = crypto_decrypt(&enc, enc.ciphertext_len, plaintext, &plaintext_len);
    if (ret == 0)
    {
        plaintext[plaintext_len] = '\0';
        printf("DECRYPT OK: \"%s\"\r\n", (char *)plaintext);
    }
    else if (ret == -1) printf("DECRYPT FAIL: HMAC\r\n");
    else if (ret == -2) printf("DECRYPT FAIL: Replay\r\n");
    else                printf("DECRYPT FAIL: err=%d\r\n", ret);
}

void tx_loop(void)
{
    static const uint8_t aes_key[16] = {
        0xAE,0x68,0x52,0xF8,0x12,0x10,0x67,0xCC,
        0x4B,0xF7,0xA5,0x76,0x55,0x77,0xF3,0x9E
    };

    crypto_init(aes_key);

    uint8_t tx_buf[128];
    uint16_t seq = 0;

    printf("TX loop start (z szyfrowaniem)\r\n");

    while(1)
    {
        uint8_t plaintext[] = "AZI=090";
        encrypted_data_t enc;

        if (crypto_encrypt(plaintext, 7, &enc) != 0) {
            printf("Blad szyfrowania!\r\n");
            HAL_Delay(3000);
            continue;
        }

        // Spakuj IV + CT + MIC do bufora
        uint8_t enc_buf[16 + 12 + 4];
        uint8_t enc_len = 0;
        memcpy(enc_buf,            enc.iv,         16);
        enc_len += 16;
        memcpy(enc_buf + enc_len,  enc.ciphertext, enc.ciphertext_len);
        enc_len += enc.ciphertext_len;
        memcpy(enc_buf + enc_len,  enc.mic,        4);
        enc_len += 4;

        printf("TX: seq=%d enc_len=%d (IV+CT+MIC)\r\n", seq, enc_len);

        uint8_t frame_len = build_frame(tx_buf,
                                        FRAME_TYPE_CMD,
                                        seq++,
                                        0x00,
                                        enc_buf, enc_len,
                                        ADDR_CENTRAL,
                                        ADDR_NODE1);

        Radio.Send(tx_buf, frame_len);
        HAL_Delay(3000);
    }
}

void rx_loop(void)
{
    static const uint8_t aes_key[16] = {
        0xAE,0x68,0x52,0xF8,0x12,0x10,0x67,0xCC,
        0x4B,0xF7,0xA5,0x76,0x55,0x77,0xF3,0x9E
    };
    crypto_init(aes_key);

    printf("RX loop start\r\n");
    Radio.Rx(0);

    while(1)
    {
        DelayMs(25);

        if (State == RX_TIMEOUT)
        {
            printf("RX timeout, restarting...\r\n"); 
            Radio.Rx(0);
            State = RX;
        }

        if (State == RX_DONE)
        {
            State = RX;
            parse_frame(Buffer, BufferSize);
            Radio.Rx(0);
        }
    }
}

void OnTxDone( void )
{
    Radio.Sleep( );
    State = TX;
    trx_events_cnt.txdone++;
}

void OnRxDone( uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr )
{
    printf("OnRxDone: %d B, RSSI=%d\r\n", size, rssi);  // dodaj tę linię
    BufferSize = size;
    memcpy( Buffer, payload, BufferSize );
    RssiValue = rssi;
    SnrValue = snr;
//    State = RX;
    State = RX_DONE;
    trx_events_cnt.rxdone++;
//    Radio.Rx(0);
}

void OnTxTimeout( void )
{
    Radio.Sleep( );
    State = TX_TIMEOUT;
    trx_events_cnt.txtimeout++;
}

void OnRxTimeout( void )
{
    State = RX_TIMEOUT;
    trx_events_cnt.rxtimeout++;
}

void OnRxError( void )
{
    printf("OnRxError!\r\n");
    State = RX_ERROR;
    trx_events_cnt.rxerror++;
    Radio.Rx(0);
}
