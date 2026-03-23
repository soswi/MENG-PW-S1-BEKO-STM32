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

#define RF_FREQUENCY                                870000000 // Hz
#define TX_OUTPUT_POWER                             0         // dBm

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

#define FSK_FDEV            						5e3       // 5 kHz  (h = 2*FDEV/BR = 1.0)
#define FSK_DATARATE        						10e3      // 10 kbps
#define FSK_BANDWIDTH       						50e3      // 50 kHz
#define FSK_AFC_BANDWIDTH   						83.333e3  // OK
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
#define BUFFER_SIZE                                 13

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
						false,							/* Enables disables the CRC [0: OFF, 1: ON] */
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
						false,							/* Enables/Disables the CRC [0: OFF, 1: ON] */
                        0,								/* Enables disables the intra-packet frequency hopping. FSK: N/A ( set to 0 ) */
						0,								/* Number of symbols bewteen each hop. FSK: N/A ( set to 0 ) */
						false,							/* Inverts IQ signals (LoRa only). FSK: N/A ( set to 0 ) */
						true							/* Sets the reception in continuous mode. [false: single mode, true: continuous mode] */
	);

#else
    #error "Please define a frequency band in the compiler options."
#endif
    

//    tx_loop();
    rx_loop();

    while(1)
    {
    	printf("Infinite loop. This should never happen!\r\n");
    }
}

void tx_loop(void)
{
	uint8_t buf[50];
	uint8_t txt[] = "NICK-xxxxxxxx-PozdrowieniaNICK";
	memcpy(buf, txt, sizeof(txt));
	int cnt = 0;
    while( 1 )
    {
        RtcGetTimeStr(buf+5);

        Radio.Send( buf, 13 );

        DelayMs( 250 );
        cnt++;
        HAL_Delay(3000);
    }

}

void rx_loop(void)
{
	char buf[50];
	int loop_cnt = 0;

	printf("\r\n\r\nRX loop start\r\n");
	int time_on_air;
	int payload_size = BUFFER_SIZE;
	time_on_air = Radio.TimeOnAir(MODEM_FSK, payload_size);
	printf("Time on air: %d us for payload_size: %d bytes\r\n", time_on_air, payload_size);

	lcd_init();
	DelayMs(100);
	lcd_clear();

	Radio.Rx(0);

	while(1)
	{
	    DelayMs(25);
//   		lcd_clear();

		snprintf(buf, sizeof(buf), "%d %d %d %d %d ", RssiValue, trx_events_cnt.rxdone, trx_events_cnt.rxerror, trx_events_cnt.rxtimeout, loop_cnt);

		if (State == RX_TIMEOUT)
		{
			Radio.Rx(0);
			State = RX;
		}

		if (State == RX_DONE)
		{

			lcd_clear();
			lcd_set_cursor(0, 0);
			lcd_write_string((uint8_t*)buf);
			lcd_set_cursor(1, 0);
			lcd_write_string(Buffer);

			printf("%s  \t", buf);
			RtcGetTimeStr((uint8_t*)buf);
			printf("Local time: %s, received: %s\r\n", buf, Buffer);
			State = RX;

//			if(Buffer[0]) {
//					__HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, degrees_to_us(15));
//				} else {
//					__HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, degrees_to_us(160));
//			}
		}

		loop_cnt++;
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
    State = RX_ERROR;
    trx_events_cnt.rxerror++;
    Radio.Rx(0);
}
