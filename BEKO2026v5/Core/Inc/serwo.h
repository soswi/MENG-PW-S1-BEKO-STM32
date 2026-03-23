
#include "main.h"
#include "stm32u5xx_hal.h"
#include "stm32u5xx_nucleo.h"

extern TIM_HandleTypeDef htim2;

uint32_t degrees_to_us(uint8_t degrees);
