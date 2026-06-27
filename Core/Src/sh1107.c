/**
 * SH1107 128x128 SPI OLED driver for STM32F1 (HAL)
 *
 * Adapted from the SSD1306 I2C library structure
 *   original author:  Tilen Majerle <tilen@majerle.eu>
 *   STM32F10x port:   Alexander Lutsai <s.lyra@ya.ru>
 *
 * Init sequence cross-checked against the SH1107 datasheet and the
 * Adafruit_SH110x reference driver (128x128 variant: display offset
 * 0x00, multiplex ratio 0x7F / 127).
 *
 *   ----------------------------------------------------------------------
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   any later version.
 *   ----------------------------------------------------------------------
 */
#include "sh1107.h"

extern SPI_HandleTypeDef hspi1;

/* Absolute value */
#define ABS(x)   ((x) > 0 ? (x) : -(x))

/* SH1107 data buffer: 128 x 128 pixels, 1 bit per pixel = 2048 bytes */
static uint8_t SH1107_Buffer[SH1107_WIDTH * SH1107_HEIGHT / 8];

/* Private SH1107 structure */
typedef struct {
	uint16_t CurrentX;
	uint16_t CurrentY;
	uint8_t Inverted;
	uint8_t Initialized;
} SH1107_t;

static SH1107_t SH1107;

/////////////////////////////////////////////////////////////////////////////////////////////////////////
//  Low level SPI / GPIO
/////////////////////////////////////////////////////////////////////////////////////////////////////////

static void SH1107_CS_LOW(void)  { HAL_GPIO_WritePin(SH1107_CS_PORT, SH1107_CS_PIN, GPIO_PIN_RESET); }
static void SH1107_CS_HIGH(void) { HAL_GPIO_WritePin(SH1107_CS_PORT, SH1107_CS_PIN, GPIO_PIN_SET); }
static void SH1107_DC_CMD(void)  { HAL_GPIO_WritePin(SH1107_DC_PORT, SH1107_DC_PIN, GPIO_PIN_RESET); } /* command */
static void SH1107_DC_DATA(void) { HAL_GPIO_WritePin(SH1107_DC_PORT, SH1107_DC_PIN, GPIO_PIN_SET); }   /* data    */

void SH1107_WriteCommand(uint8_t command) {
	SH1107_DC_CMD();
	SH1107_CS_LOW();
	HAL_SPI_Transmit(&hspi1, &command, 1, SH1107_SPI_TIMEOUT);
	SH1107_CS_HIGH();
}

void SH1107_WriteData(uint8_t* data, uint16_t count) {
	SH1107_DC_DATA();
	SH1107_CS_LOW();
	HAL_SPI_Transmit(&hspi1, data, count, SH1107_SPI_TIMEOUT);
	SH1107_CS_HIGH();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
//  Init / screen update
/////////////////////////////////////////////////////////////////////////////////////////////////////////

uint8_t SH1107_Init(void) {

	/* Idle states */
	SH1107_CS_HIGH();

	/* Hardware reset, active low */
	HAL_GPIO_WritePin(SH1107_RST_PORT, SH1107_RST_PIN, GPIO_PIN_RESET);
	HAL_Delay(10);
	HAL_GPIO_WritePin(SH1107_RST_PORT, SH1107_RST_PIN, GPIO_PIN_SET);
	HAL_Delay(10);

	SH1107_WriteCommand(0xAE);       /* Display off */
	SH1107_WriteCommand(0xD5);       /* Set display clock divide ratio/osc freq */
	SH1107_WriteCommand(0x51);
	SH1107_WriteCommand(0x20);       /* Set memory addressing mode (page mode) */
	SH1107_WriteCommand(0x81);       /* Contrast control */
	SH1107_WriteCommand(0x4F);
	SH1107_WriteCommand(0xAD);       /* DC-DC control mode set */
	SH1107_WriteCommand(0x8A);
	SH1107_WriteCommand(0xA0);       /* Segment re-map: normal (not flipped) */
	SH1107_WriteCommand(0xC0);       /* COM scan direction: increasing */
	SH1107_WriteCommand(0xDC);       /* Set display start line */
	SH1107_WriteCommand(0x00);
	SH1107_WriteCommand(0xD3);       /* Set display offset = 0 (128 row panel) */
	SH1107_WriteCommand(0x00);
	SH1107_WriteCommand(0xD9);       /* Set pre-charge period */
	SH1107_WriteCommand(0x22);
	SH1107_WriteCommand(0xDB);       /* Set VCOM deselect level */
	SH1107_WriteCommand(0x35);
	SH1107_WriteCommand(0xA8);       /* Set multiplex ratio = 127 (128 rows) */
	SH1107_WriteCommand(0x7F);
	SH1107_WriteCommand(0xA4);       /* Display all-on resume (show RAM content) */
	SH1107_WriteCommand(0xA6);       /* Normal display (not inverted) */

	HAL_Delay(100);                  /* Recommended after VPP charge pump setup */

	SH1107_WriteCommand(0xAF);       /* Display ON */

	/* Clear screen */
	SH1107_Fill(SH1107_COLOR_BLACK);

	/* Push to panel */
	SH1107_UpdateScreen();

	/* Set default values */
	SH1107.CurrentX = 0;
	SH1107.CurrentY = 0;
	SH1107.Inverted = 0;

	/* Initialized OK. There is no way to confirm the panel is physically
	 * present over SPI (no ACK like I2C), so this always returns 1. */
	SH1107.Initialized = 1;
	return 1;
}

void SH1107_UpdateScreen(void) {
	uint8_t page;
	uint8_t pages = SH1107_HEIGHT / 8;

	for (page = 0; page < pages; page++) {
		SH1107_WriteCommand(0xB0 + page); /* Set page address (0..15) */
		SH1107_WriteCommand(0x00);        /* Set lower column address = 0 */
		SH1107_WriteCommand(0x10);        /* Set higher column address = 0 */

		SH1107_WriteData(&SH1107_Buffer[SH1107_WIDTH * page], SH1107_WIDTH);
	}
}

void SH1107_ToggleInvert(void) {
	uint16_t i;

	SH1107.Inverted = !SH1107.Inverted;

	for (i = 0; i < sizeof(SH1107_Buffer); i++) {
		SH1107_Buffer[i] = ~SH1107_Buffer[i];
	}
}

void SH1107_InvertDisplay(int i) {
	if (i) {
		SH1107_WriteCommand(0xA7); /* Inverted display */
	} else {
		SH1107_WriteCommand(0xA6); /* Normal display */
	}
}

void SH1107_SetContrast(uint8_t value) {
	SH1107_WriteCommand(0x81);
	SH1107_WriteCommand(value);
}

void SH1107_Fill(SH1107_COLOR_t color) {
	memset(SH1107_Buffer, (color == SH1107_COLOR_BLACK) ? 0x00 : 0xFF, sizeof(SH1107_Buffer));
}

void SH1107_DrawPixel(uint16_t x, uint16_t y, SH1107_COLOR_t color) {
	if (x >= SH1107_WIDTH || y >= SH1107_HEIGHT) {
		return;
	}

	if (SH1107.Inverted) {
		color = (SH1107_COLOR_t)!color;
	}

	if (color == SH1107_COLOR_WHITE) {
		SH1107_Buffer[x + (y / 8) * SH1107_WIDTH] |= 1 << (y % 8);
	} else {
		SH1107_Buffer[x + (y / 8) * SH1107_WIDTH] &= ~(1 << (y % 8));
	}
}

void SH1107_GotoXY(uint16_t x, uint16_t y) {
	SH1107.CurrentX = x;
	SH1107.CurrentY = y;
}

char SH1107_Putc(char ch, FontDef_t* Font, SH1107_COLOR_t color) {
	uint32_t i, b, j;

	if (
		SH1107_WIDTH <= (SH1107.CurrentX + Font->FontWidth) ||
		SH1107_HEIGHT <= (SH1107.CurrentY + Font->FontHeight)
	) {
		return 0;
	}

	for (i = 0; i < Font->FontHeight; i++) {
		b = Font->data[(ch - 32) * Font->FontHeight + i];
		for (j = 0; j < Font->FontWidth; j++) {
			if ((b << j) & 0x8000) {
				SH1107_DrawPixel(SH1107.CurrentX + j, (SH1107.CurrentY + i), (SH1107_COLOR_t) color);
			} else {
				SH1107_DrawPixel(SH1107.CurrentX + j, (SH1107.CurrentY + i), (SH1107_COLOR_t)!color);
			}
		}
	}

	SH1107.CurrentX += Font->FontWidth;

	return ch;
}

char SH1107_Puts(char* str, FontDef_t* Font, SH1107_COLOR_t color) {
	while (*str) {
		if (SH1107_Putc(*str, Font, color) != *str) {
			return *str;
		}
		str++;
	}
	return *str;
}

void SH1107_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, SH1107_COLOR_t c) {
	int16_t dx, dy, sx, sy, err, e2, i, tmp;

	if (x0 >= SH1107_WIDTH)  { x0 = SH1107_WIDTH - 1; }
	if (x1 >= SH1107_WIDTH)  { x1 = SH1107_WIDTH - 1; }
	if (y0 >= SH1107_HEIGHT) { y0 = SH1107_HEIGHT - 1; }
	if (y1 >= SH1107_HEIGHT) { y1 = SH1107_HEIGHT - 1; }

	dx = (x0 < x1) ? (x1 - x0) : (x0 - x1);
	dy = (y0 < y1) ? (y1 - y0) : (y0 - y1);
	sx = (x0 < x1) ? 1 : -1;
	sy = (y0 < y1) ? 1 : -1;
	err = ((dx > dy) ? dx : -dy) / 2;

	if (dx == 0) {
		if (y1 < y0) { tmp = y1; y1 = y0; y0 = tmp; }
		if (x1 < x0) { tmp = x1; x1 = x0; x0 = tmp; }
		for (i = y0; i <= y1; i++) {
			SH1107_DrawPixel(x0, i, c);
		}
		return;
	}

	if (dy == 0) {
		if (y1 < y0) { tmp = y1; y1 = y0; y0 = tmp; }
		if (x1 < x0) { tmp = x1; x1 = x0; x0 = tmp; }
		for (i = x0; i <= x1; i++) {
			SH1107_DrawPixel(i, y0, c);
		}
		return;
	}

	while (1) {
		SH1107_DrawPixel(x0, y0, c);
		if (x0 == x1 && y0 == y1) {
			break;
		}
		e2 = err;
		if (e2 > -dx) {
			err -= dy;
			x0 += sx;
		}
		if (e2 < dy) {
			err += dx;
			y0 += sy;
		}
	}
}

void SH1107_DrawRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h, SH1107_COLOR_t c) {
	if (x >= SH1107_WIDTH || y >= SH1107_HEIGHT) {
		return;
	}
	if ((x + w) >= SH1107_WIDTH)  { w = SH1107_WIDTH - x; }
	if ((y + h) >= SH1107_HEIGHT) { h = SH1107_HEIGHT - y; }

	SH1107_DrawLine(x, y, x + w, y, c);
	SH1107_DrawLine(x, y + h, x + w, y + h, c);
	SH1107_DrawLine(x, y, x, y + h, c);
	SH1107_DrawLine(x + w, y, x + w, y + h, c);
}

void SH1107_DrawFilledRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h, SH1107_COLOR_t c) {
	uint8_t i;

	if (x >= SH1107_WIDTH || y >= SH1107_HEIGHT) {
		return;
	}
	if ((x + w) >= SH1107_WIDTH)  { w = SH1107_WIDTH - x; }
	if ((y + h) >= SH1107_HEIGHT) { h = SH1107_HEIGHT - y; }

	for (i = 0; i <= h; i++) {
		SH1107_DrawLine(x, y + i, x + w, y + i, c);
	}
}

void SH1107_DrawTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3, SH1107_COLOR_t color) {
	SH1107_DrawLine(x1, y1, x2, y2, color);
	SH1107_DrawLine(x2, y2, x3, y3, color);
	SH1107_DrawLine(x3, y3, x1, y1, color);
}

void SH1107_DrawFilledTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3, SH1107_COLOR_t color) {
	int16_t deltax = 0, deltay = 0, x = 0, y = 0, xinc1 = 0, xinc2 = 0,
	yinc1 = 0, yinc2 = 0, den = 0, num = 0, numadd = 0, numpixels = 0,
	curpixel = 0;

	deltax = ABS(x2 - x1);
	deltay = ABS(y2 - y1);
	x = x1;
	y = y1;

	if (x2 >= x1) { xinc1 = 1; xinc2 = 1; } else { xinc1 = -1; xinc2 = -1; }
	if (y2 >= y1) { yinc1 = 1; yinc2 = 1; } else { yinc1 = -1; yinc2 = -1; }

	if (deltax >= deltay) {
		xinc1 = 0; yinc2 = 0;
		den = deltax; num = deltax / 2; numadd = deltay; numpixels = deltax;
	} else {
		xinc2 = 0; yinc1 = 0;
		den = deltay; num = deltay / 2; numadd = deltax; numpixels = deltay;
	}

	for (curpixel = 0; curpixel <= numpixels; curpixel++) {
		SH1107_DrawLine(x, y, x3, y3, color);

		num += numadd;
		if (num >= den) {
			num -= den;
			x += xinc1;
			y += yinc1;
		}
		x += xinc2;
		y += yinc2;
	}
}

void SH1107_DrawCircle(int16_t x0, int16_t y0, int16_t r, SH1107_COLOR_t c) {
	int16_t f = 1 - r;
	int16_t ddF_x = 1;
	int16_t ddF_y = -2 * r;
	int16_t x = 0;
	int16_t y = r;

	SH1107_DrawPixel(x0, y0 + r, c);
	SH1107_DrawPixel(x0, y0 - r, c);
	SH1107_DrawPixel(x0 + r, y0, c);
	SH1107_DrawPixel(x0 - r, y0, c);

	while (x < y) {
		if (f >= 0) {
			y--;
			ddF_y += 2;
			f += ddF_y;
		}
		x++;
		ddF_x += 2;
		f += ddF_x;

		SH1107_DrawPixel(x0 + x, y0 + y, c);
		SH1107_DrawPixel(x0 - x, y0 + y, c);
		SH1107_DrawPixel(x0 + x, y0 - y, c);
		SH1107_DrawPixel(x0 - x, y0 - y, c);

		SH1107_DrawPixel(x0 + y, y0 + x, c);
		SH1107_DrawPixel(x0 - y, y0 + x, c);
		SH1107_DrawPixel(x0 + y, y0 - x, c);
		SH1107_DrawPixel(x0 - y, y0 - x, c);
	}
}

void SH1107_DrawFilledCircle(int16_t x0, int16_t y0, int16_t r, SH1107_COLOR_t c) {
	int16_t f = 1 - r;
	int16_t ddF_x = 1;
	int16_t ddF_y = -2 * r;
	int16_t x = 0;
	int16_t y = r;

	SH1107_DrawPixel(x0, y0 + r, c);
	SH1107_DrawPixel(x0, y0 - r, c);
	SH1107_DrawPixel(x0 + r, y0, c);
	SH1107_DrawPixel(x0 - r, y0, c);
	SH1107_DrawLine(x0 - r, y0, x0 + r, y0, c);

	while (x < y) {
		if (f >= 0) {
			y--;
			ddF_y += 2;
			f += ddF_y;
		}
		x++;
		ddF_x += 2;
		f += ddF_x;

		SH1107_DrawLine(x0 - x, y0 + y, x0 + x, y0 + y, c);
		SH1107_DrawLine(x0 + x, y0 - y, x0 - x, y0 - y, c);

		SH1107_DrawLine(x0 + y, y0 + x, x0 - y, y0 + x, c);
		SH1107_DrawLine(x0 + y, y0 - x, x0 - y, y0 - x, c);
	}
}

void SH1107_DrawBitmap(int16_t x, int16_t y, const unsigned char* bitmap, int16_t w, int16_t h, uint16_t color) {
	int16_t byteWidth = (w + 7) / 8;
	uint8_t byte = 0;

	for (int16_t j = 0; j < h; j++, y++) {
		for (int16_t i = 0; i < w; i++) {
			if (i & 7) {
				byte <<= 1;
			} else {
				byte = (*(const unsigned char *)(&bitmap[j * byteWidth + i / 8]));
			}
			if (byte & 0x80) {
				SH1107_DrawPixel(x + i, y, color);
			}
		}
	}
}

void SH1107_Clear(void) {
	SH1107_Fill(SH1107_COLOR_BLACK);
	SH1107_UpdateScreen();
}

void SH1107_ON(void) {
	SH1107_WriteCommand(0xAF);
}

void SH1107_OFF(void) {
	SH1107_WriteCommand(0xAE);
}
