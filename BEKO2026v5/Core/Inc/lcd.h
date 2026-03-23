/*
 * lcd.h
 *
 *  Created on: Feb 19, 2024
 *      Author: Przemysław Korpas
 */

#ifndef INC_LCD_H_
#define INC_LCD_H_


/* Public interface */

void lcd_demo(void);
void lcd_write_string(uint8_t *str);
void lcd_set_cursor(uint8_t row, uint8_t column);
void lcd_clear(void);
void lcd_backlight(uint8_t state);



#endif /* INC_LCD_H_ */
