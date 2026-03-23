/*
 * servo.c
 *
 *  Created on: Mar 11, 2026
 *      Author: soswi
 */

#include "serwo.h"

#define SERVO_MIN_US 500
#define SERVO_MAX_US 2500
#define SERVO_DEGREES_RANGE 180
#define SERVO_DEBUG_STEP 45

uint32_t degrees_to_us(uint8_t degrees)
	{
		if (degrees > SERVO_DEGREES_RANGE) degrees = SERVO_DEGREES_RANGE;
		return SERVO_MIN_US + ((uint32_t)degrees * (SERVO_MAX_US - SERVO_MIN_US) / SERVO_DEGREES_RANGE);
	}
