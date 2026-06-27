/**
 * SH1107 128x128 SPI OLED driver for STM32F1 (HAL)
 *
 * Adapted from the SSD1306 I2C library structure
 *   original author:  Tilen Majerle <tilen@majerle.eu>
 *   STM32F10x port:   Alexander Lutsai <s.lyra@ya.ru>
 *
 * This version targets a GME128128-01 (or similar) 128x128 OLED module
 * driven by the SH1107 controller over 4-wire SPI (CS, DC, RST + SPI bus).
 *
 * Pinout used (matches SOC_PROTO.ioc):
 *   SCK  -> PA5 (SPI1_SCK)
 *   MOSI -> PA7 (SPI1_MOSI)
 *   CS   -> PB0
 *   DC   -> PB1
 *   RST  -> PB10
 *
 *   ----------------------------------------------------------------------
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   any later version.
 *   ----------------------------------------------------------------------
 */
#ifndef SH1107_H
#define SH1107_H 100

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f1xx_hal.h"
#include "fonts.h"
#include "stdlib.h"
#include "string.h"

/* ------------------------------------------------------------------ */
/* Panel geometry                                                      */
/* ------------------------------------------------------------------ */
#ifndef SH1107_WIDTH
#define SH1107_WIDTH             128
#endif

#ifndef SH1107_HEIGHT
#define SH1107_HEIGHT            128
#endif

/* ------------------------------------------------------------------ */
/* Pin mapping - override before including this header if your wiring */
/* differs from SOC_PROTO.ioc                                          */
/* ------------------------------------------------------------------ */
#ifndef SH1107_CS_PORT
#define SH1107_CS_PORT           GPIOB
#endif
#ifndef SH1107_CS_PIN
#define SH1107_CS_PIN            GPIO_PIN_0
#endif

#ifndef SH1107_DC_PORT
#define SH1107_DC_PORT           GPIOB
#endif
#ifndef SH1107_DC_PIN
#define SH1107_DC_PIN            GPIO_PIN_1
#endif

#ifndef SH1107_RST_PORT
#define SH1107_RST_PORT          GPIOB
#endif
#ifndef SH1107_RST_PIN
#define SH1107_RST_PIN           GPIO_PIN_10
#endif

#ifndef SH1107_SPI_TIMEOUT
#define SH1107_SPI_TIMEOUT       100
#endif

/**
 * @brief  SH1107 color enumeration
 */
typedef enum {
	SH1107_COLOR_BLACK = 0x00, /*!< Black color, no pixel */
	SH1107_COLOR_WHITE = 0x01  /*!< Pixel is set */
} SH1107_COLOR_t;

/* ------------------------------------------------------------------ */
/* High level API (mirrors the old SSD1306_* API)                      */
/* ------------------------------------------------------------------ */

/**
 * @brief  Initializes the SH1107 LCD (hardware reset + init sequence)
 * @retval Always returns 1. SPI displays have no read-back/ACK like I2C,
 *         so there is no reliable way to detect a missing panel here.
 */
uint8_t SH1107_Init(void);

/** @brief  Pushes the RAM buffer to the physical display over SPI */
void SH1107_UpdateScreen(void);

/** @brief  Inverts the buffer content (software invert) */
void SH1107_ToggleInvert(void);

/** @brief  Fills the whole buffer with one color */
void SH1107_Fill(SH1107_COLOR_t Color);

/** @brief  Sets one pixel in the buffer */
void SH1107_DrawPixel(uint16_t x, uint16_t y, SH1107_COLOR_t color);

/** @brief  Sets the text cursor position */
void SH1107_GotoXY(uint16_t x, uint16_t y);

/** @brief  Draws one character at the cursor, using Font */
char SH1107_Putc(char ch, FontDef_t* Font, SH1107_COLOR_t color);

/** @brief  Draws a null-terminated string at the cursor, using Font */
char SH1107_Puts(char* str, FontDef_t* Font, SH1107_COLOR_t color);

void SH1107_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, SH1107_COLOR_t c);
void SH1107_DrawRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h, SH1107_COLOR_t c);
void SH1107_DrawFilledRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h, SH1107_COLOR_t c);
void SH1107_DrawTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3, SH1107_COLOR_t color);
void SH1107_DrawFilledTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3, SH1107_COLOR_t color);
void SH1107_DrawCircle(int16_t x0, int16_t y0, int16_t r, SH1107_COLOR_t c);
void SH1107_DrawFilledCircle(int16_t x0, int16_t y0, int16_t r, SH1107_COLOR_t c);

/**
 * @brief  Draws a 1bpp bitmap (MSB first per row, byte-padded rows)
 * @param  color: 0 -> black, nonzero -> white
 */
void SH1107_DrawBitmap(int16_t x, int16_t y, const unsigned char* bitmap, int16_t w, int16_t h, uint16_t color);

/** @brief  Hardware invert (0xA6 normal / 0xA7 inverted), no buffer cost */
void SH1107_InvertDisplay(int i);

/** @brief  Sets display contrast, 0x00 - 0xFF */
void SH1107_SetContrast(uint8_t value);

/** @brief  Clears buffer and pushes blank screen */
void SH1107_Clear(void);

/** @brief  Turns the panel on (0xAF) */
void SH1107_ON(void);

/** @brief  Turns the panel off (0xAE) - low power */
void SH1107_OFF(void);

/* ------------------------------------------------------------------ */
/* Low level SPI helpers - exposed in case you need raw command access */
/* ------------------------------------------------------------------ */

/** @brief  Sends a single command byte (DC low) */
void SH1107_WriteCommand(uint8_t command);

/** @brief  Sends a buffer of data bytes (DC high) */
void SH1107_WriteData(uint8_t* data, uint16_t count);

#ifdef __cplusplus
}
#endif

#endif /* SH1107_H */
