/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : SOC Monitor — SH1107 128x128, WCS1700, SD logger
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fatfs.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "sh1107.h"
#include "fonts.h"
#include "sd_functions.h"
#include "sd_spi.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* ---- Display refresh throttle -------------------------------------------
 * Main loop runs every ~10ms. A full SH1107 frame push takes a few ms
 * over SPI, so we only refresh the OLED every DISPLAY_REFRESH_DIV ticks
 * (~200ms) to avoid hogging SPI1 and causing visible flicker.
 * ------------------------------------------------------------------------- */
#define DISPLAY_REFRESH_DIV     20

/* ---- OLED layout constants (128x128 SH1107) ------------------------------
 *
 *  y=0..12   : Header  "SOC MONITORING" (Font_7x10)
 *  y=13      : divider line
 *  y=17..50  : Voltage block  — label Font_7x10, value Font_11x18
 *  y=51      : divider line
 *  y=54..87  : Current block  — label Font_7x10, value Font_11x18
 *  y=89      : divider line
 *  y=92..127 : Footer 2-col   — left: LOGGED count, right: SD status
 *              vertical separator at x=63
 *
 * Font metrics:
 *   Font_7x10  : char  7px wide, 10px tall
 *   Font_11x18 : char 11px wide, 18px tall
 *   Font_16x26 : char 16px wide, 26px tall  (not used — won't fit 6-digit numbers)
 * ------------------------------------------------------------------------- */
#define OLED_W                  SH1107_WIDTH    /* 128 */
#define OLED_H                  SH1107_HEIGHT   /* 128 */

/* Header */
#define HDR_Y                   2
#define HDR_LINE_Y              13

/* Voltage block */
#define VLT_LABEL_Y             17
#define VLT_VALUE_Y             29    /* Font_11x18 baseline */
#define VLT_UNIT_Y              35    /* Font_7x10 unit "V" beside value */
#define VLT_LINE_Y              51

/* Current block */
#define CUR_LABEL_Y             54
#define CUR_VALUE_Y             66
#define CUR_UNIT_Y              72
#define CUR_LINE_Y              89

/* Footer */
#define FTR_LINE_Y              90
#define FTR_COL_SEP_X           63
#define FTR_LABEL_Y             93
#define FTR_VALUE_Y             104

/* ---- Sensor scaling -------------------------------------------------------
 * Voltage divider: R1=180k (to input), R2=10k (to GND).
 *   Vin = Vadc1 * (R1+R2)/R2 = Vadc1 * 19
 *
 * Current: WCS1700 Hall sensor, ratiometric @3.3V supply.
 *   Datasheet: 33 mV/A @5V → 21.78 mV/A @3.3V
 *   Offset: measured at 0A via auto-zero on every boot (see AutoZero_Compute).
 * ------------------------------------------------------------------------- */
#define ADC_VREF                3.3f
#define ADC_MAX_COUNT           4095.0f
#define ADC_OVERSAMPLE_COUNT    64

#define VOLTAGE_DIVIDER_R1      180000.0f
#define VOLTAGE_DIVIDER_R2      10000.0f
#define VOLTAGE_DIVIDER_RATIO   ((VOLTAGE_DIVIDER_R1 + VOLTAGE_DIVIDER_R2) / VOLTAGE_DIVIDER_R2)

#define CURRENT_SENSOR_OFFSET       1.690f               /* fallback only */
#define CURRENT_SENSOR_SENSITIVITY  (0.033f * (3.3f/5.0f))

/* ---- EMA filter -----------------------------------------------------------
 *   y[n] = y[n-1] + alpha * (x[n] - y[n-1])
 * ------------------------------------------------------------------------- */
#define VOLTAGE_EMA_ALPHA       0.08f
#define CURRENT_EMA_ALPHA       0.04f

/* ---- SD card logger -------------------------------------------------------
 * Main loop runs every ~10ms. We only push one sample into the logger
 * every LOG_SAMPLE_DIV ticks (~1000ms), so the CSV ends up with one row
 * per second instead of one row per 10ms tick.
 * ------------------------------------------------------------------------- */
#define LOG_FILENAME             "LOG.CSV"
#define LOG_BUFFER_SAMPLES       1
#define LOG_LINE_MAXLEN          32    /* timestamp,voltage,current */
#define LOG_SYNC_EVERY_FLUSH     1
#define LOG_SAMPLE_DIV           100   /* 100 * 10ms = 1000ms -> log once per second */

/* ---- Auto-zero ------------------------------------------------------------
 * 1000 samples × 10ms = ~10 seconds. Each sample is itself a 64-reading
 * average from ADC_ReadAveraged, so the final value averages 64,000
 * ADC2 conversions. Requires zero current through WCS1700 during boot.
 * ------------------------------------------------------------------------- */
#define AUTOZERO_SAMPLES        1000
#define AUTOZERO_FALLBACK       CURRENT_SENSOR_OFFSET

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc2;

SPI_HandleTypeDef hspi1;
SPI_HandleTypeDef hspi2;
DMA_HandleTypeDef hdma_spi2_rx;
DMA_HandleTypeDef hdma_spi2_tx;

/* USER CODE BEGIN PV */
volatile float voltage = 0.0f;
volatile float current = 0.0f;
volatile float vAdc2Raw = 0.0f;

static float adc1VoltFiltered = 0.0f;
static float adc2VoltFiltered = 0.0f;
static uint8_t emaInitialized = 0;

/* Runtime offset from auto-zero — replaces hardcoded CURRENT_SENSOR_OFFSET */
static float currentSensorOffset = AUTOZERO_FALLBACK;

/* SD logger */
static FIL      logFile;
static uint8_t  sdReady = 0;
static char     logChunk[LOG_BUFFER_SAMPLES * LOG_LINE_MAXLEN];
static uint16_t logChunkLen = 0;
static uint8_t  logSampleCount = 0;
static uint16_t logFlushCount = 0;
static uint32_t logTotalSamples = 0;
static uint32_t logTickOffset   = 0;   /* tick saat sample pertama — untuk hitung elapsed seconds */

/* SD status string — short enough to fit Font_7x10 in 63px half-column */
static char sdStatusStr[10] = "UNKNOWN";

/* Shared scratch buffer for snprintf — only one call at a time, never
 * called from an ISR, so a single static buffer is safe here. */
static char lineBuf[24];

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC2_Init(void);
static void MX_SPI1_Init(void);
static void MX_SPI2_Init(void);

/* USER CODE BEGIN PFP */
static uint16_t ADC_ReadAveraged(ADC_HandleTypeDef *hadc);
static void     FloatToStr(float value, char *buf, uint8_t decimals);
static void     DrawMainScreen(float v, float i);
static void     DrawCalibratingScreen(uint16_t samplesDone, float latestOffset);
static void     DrawZeroDoneScreen(float offset);
static uint8_t  SD_Logger_Init(void);
static void     SD_Logger_AddSample(float v, float i);
static void     SD_Logger_Flush(void);
static void     AutoZero_Compute(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  Application entry point.
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  uint16_t displayCounter  = 0;
  uint16_t logSampleCounter = 0;
  /* USER CODE END 1 */

  HAL_Init();

  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_SPI1_Init();
  MX_SPI2_Init();
  /* MX_FATFS_Init() intentionally skipped — see sd_functions.c comment. */

  /* USER CODE BEGIN 2 */
  HAL_ADCEx_Calibration_Start(&hadc1);
  HAL_ADCEx_Calibration_Start(&hadc2);

  /* OLED init — splash for ~600ms while SD mounts */
  SH1107_Init();
  SH1107_Fill(SH1107_COLOR_BLACK);
  SH1107_GotoXY(22, 2);
  SH1107_Puts("SOC MONITORING", &Font_7x10, SH1107_COLOR_WHITE);
  SH1107_DrawLine(0, 13, OLED_W - 1, 13, SH1107_COLOR_WHITE);
  SH1107_GotoXY(16, 56);
  SH1107_Puts("Initializing...", &Font_7x10, SH1107_COLOR_WHITE);
  SH1107_UpdateScreen();

  /* Mount SD — sets sdStatusStr and sdReady */
  sdReady = SD_Logger_Init();
  HAL_Delay(600);

  /* Auto-zero phase — ~10 seconds, OLED shows progress */
  AutoZero_Compute();

  /* Show zero-done result for 1.5 s then jump into loop */
  DrawZeroDoneScreen(currentSensorOffset);
  SH1107_UpdateScreen();
  HAL_Delay(1500);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    uint16_t raw1 = ADC_ReadAveraged(&hadc1);
    uint16_t raw2 = ADC_ReadAveraged(&hadc2);

    float adc1Raw = (float)raw1 * ADC_VREF / ADC_MAX_COUNT;
    float adc2Raw = (float)raw2 * ADC_VREF / ADC_MAX_COUNT;

    if (!emaInitialized) {
        adc1VoltFiltered = adc1Raw;
        adc2VoltFiltered = adc2Raw;
        emaInitialized = 1;
    } else {
        adc1VoltFiltered += VOLTAGE_EMA_ALPHA * (adc1Raw - adc1VoltFiltered);
        adc2VoltFiltered += CURRENT_EMA_ALPHA * (adc2Raw - adc2VoltFiltered);
    }

    voltage  = adc1VoltFiltered * VOLTAGE_DIVIDER_RATIO;
    vAdc2Raw = adc2VoltFiltered;
    current  = (vAdc2Raw - currentSensorOffset) / CURRENT_SENSOR_SENSITIVITY;

    if (++displayCounter >= DISPLAY_REFRESH_DIV) {
        displayCounter = 0;
        DrawMainScreen(voltage, current);
    }

    /* Log to SD once per second instead of every ~10ms tick */
    if (++logSampleCounter >= LOG_SAMPLE_DIV) {
        logSampleCounter = 0;
        SD_Logger_AddSample(voltage, current);
    }

    HAL_Delay(10);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/* USER CODE BEGIN 4 */

/* --------------------------------------------------------------------------
 * ADC helpers
 * -------------------------------------------------------------------------- */

static uint16_t ADC_ReadAveraged(ADC_HandleTypeDef *hadc)
{
    uint32_t sum = 0;
    uint8_t  valid = 0;
    for (uint8_t i = 0; i < ADC_OVERSAMPLE_COUNT; i++) {
        HAL_ADC_Start(hadc);
        if (HAL_ADC_PollForConversion(hadc, 100) == HAL_OK) {
            sum += HAL_ADC_GetValue(hadc);
            valid++;
        }
        HAL_ADC_Stop(hadc);
    }
    return valid ? (uint16_t)(sum / valid) : 0;
}

/* --------------------------------------------------------------------------
 * Float-to-string without float printf
 * -------------------------------------------------------------------------- */

static void FloatToStr(float value, char *buf, uint8_t decimals)
{
    int32_t scale = 1;
    for (uint8_t i = 0; i < decimals; i++) scale *= 10;

    uint8_t neg    = (value < 0.0f);
    float   absVal = neg ? -value : value;
    int32_t scaled = (int32_t)(absVal * scale + 0.5f);
    int32_t whole  = scaled / scale;
    int32_t frac   = scaled % scale;

    sprintf(buf, "%s%ld.%0*ld", neg ? "-" : "", (long)whole, decimals, (long)frac);
}

/* --------------------------------------------------------------------------
 * DrawMainScreen
 *
 * Layout (128x128):
 *
 *  ┌──────────────────────────────┐  y=0
 *  │      SOC MONITORING          │  Font_7x10, centered, y=2
 *  ├──────────────────────────────┤  y=13
 *  │ VOLTAGE                      │  Font_7x10, y=17
 *  │ 48.23         V              │  Font_11x18 value, Font_7x10 unit
 *  ├──────────────────────────────┤  y=51
 *  │ CURRENT                      │  Font_7x10, y=54
 *  │ 12.50         A              │  Font_11x18 value, Font_7x10 unit
 *  ├──────────────────────────────┤  y=89
 *  │ LOGGED      │ SD CARD        │  Font_7x10 labels, y=93
 *  │ 001234      │ READY          │  Font_7x10 values, y=104
 *  └─────────────┴────────────────┘  y=127
 *
 * Only the data areas are cleared between refreshes — header and footer
 * borders are drawn once and only touched if needed.
 * -------------------------------------------------------------------------- */
static void DrawMainScreen(float v, float i)
{
    char numBuf[12];

    /* ---- Header ----------------------------------------------------------*/
    SH1107_DrawFilledRectangle(0, 0, OLED_W, 13, SH1107_COLOR_BLACK);
    SH1107_GotoXY(8, HDR_Y);
    SH1107_Puts("SOC MONITORING", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_DrawLine(0, HDR_LINE_Y, OLED_W - 1, HDR_LINE_Y, SH1107_COLOR_WHITE);

    /* ---- Voltage block ---------------------------------------------------*/
    SH1107_DrawFilledRectangle(0, VLT_LABEL_Y, OLED_W, VLT_LINE_Y - VLT_LABEL_Y + 1,
                               SH1107_COLOR_BLACK);
    SH1107_GotoXY(3, VLT_LABEL_Y);
    SH1107_Puts("VOLTAGE", &Font_7x10, SH1107_COLOR_WHITE);

    FloatToStr(v, numBuf, 2);
    SH1107_GotoXY(3, VLT_VALUE_Y);
    SH1107_Puts(numBuf, &Font_11x18, SH1107_COLOR_WHITE);
    /* Unit "V" right-aligned to x=122 */
    SH1107_GotoXY(116, VLT_UNIT_Y);
    SH1107_Puts("V", &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawLine(0, VLT_LINE_Y, OLED_W - 1, VLT_LINE_Y, SH1107_COLOR_WHITE);

    /* ---- Current block ---------------------------------------------------*/
    SH1107_DrawFilledRectangle(0, CUR_LABEL_Y, OLED_W, CUR_LINE_Y - CUR_LABEL_Y + 1,
                               SH1107_COLOR_BLACK);
    SH1107_GotoXY(3, CUR_LABEL_Y);
    SH1107_Puts("CURRENT", &Font_7x10, SH1107_COLOR_WHITE);

    FloatToStr(i, numBuf, 2);
    SH1107_GotoXY(3, CUR_VALUE_Y);
    SH1107_Puts(numBuf, &Font_11x18, SH1107_COLOR_WHITE);
    SH1107_GotoXY(116, CUR_UNIT_Y);
    SH1107_Puts("A", &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawLine(0, CUR_LINE_Y, OLED_W - 1, CUR_LINE_Y, SH1107_COLOR_WHITE);

    /* ---- Footer 2-column -------------------------------------------------
     * Left  (x=0..62)  : logged sample count
     * Right (x=64..127): SD status
     * Separator: vertical line at x=63
     * --------------------------------------------------------------------- */
    SH1107_DrawFilledRectangle(0, FTR_LINE_Y + 1, OLED_W,
                               OLED_H - FTR_LINE_Y - 1, SH1107_COLOR_BLACK);

    /* Left column */
    SH1107_GotoXY(3, FTR_LABEL_Y);
    SH1107_Puts("LOGGED", &Font_7x10, SH1107_COLOR_WHITE);
    snprintf(lineBuf, sizeof(lineBuf), "%06lu", (unsigned long)logTotalSamples);
    SH1107_GotoXY(3, FTR_VALUE_Y);
    SH1107_Puts(lineBuf, &Font_7x10, SH1107_COLOR_WHITE);

    /* Vertical separator */
    SH1107_DrawLine(FTR_COL_SEP_X, FTR_LINE_Y + 1, FTR_COL_SEP_X,
                    OLED_H - 1, SH1107_COLOR_WHITE);

    /* Right column */
    SH1107_GotoXY(67, FTR_LABEL_Y);
    SH1107_Puts("SD CARD", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_GotoXY(67, FTR_VALUE_Y);
    SH1107_Puts(sdStatusStr, &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_UpdateScreen();
}

/* --------------------------------------------------------------------------
 * DrawCalibratingScreen
 *
 *  ┌──────────────────────────────┐  y=0
 *  │       CALIBRATING            │  Font_7x10, centered, y=2
 *  ├──────────────────────────────┤  y=13
 *  │ No load on sensor input.     │  Font_7x10, y=18
 *  │ Keep current at zero.        │  Font_7x10, y=29
 *  ├──────────────────────────────┤  y=42
 *  │ PROGRESS                     │  Font_7x10, y=46
 *  │ [████████████░░░░░░░░░░░] nnn%│  bar y=58..69, pct label y=74
 *  ├──────────────────────────────┤  y=86
 *  │ Sampling ADC2...             │  Font_7x10, y=90
 *  │ Offset: x.xxxx V             │  Font_7x10, y=101
 *  └──────────────────────────────┘
 *
 * Bar: x=3, y=58, width=122, height=12
 *      fill grows from x=4 to x=4+fillW as samples accumulate.
 * -------------------------------------------------------------------------- */
static void DrawCalibratingScreen(uint16_t samplesDone, float latestOffset)
{
    const uint16_t BAR_X = 3, BAR_Y = 58, BAR_W = 122, BAR_H = 12;
    char offBuf[12];

    SH1107_Fill(SH1107_COLOR_BLACK);

    /* Header */
    SH1107_GotoXY(15, HDR_Y);
    SH1107_Puts("CALIBRATING", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_DrawLine(0, HDR_LINE_Y, OLED_W - 1, HDR_LINE_Y, SH1107_COLOR_WHITE);

    /* Instructions */
    SH1107_GotoXY(3, 18);
    SH1107_Puts("No load on sensor", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_GotoXY(3, 29);
    SH1107_Puts("input. Zero amps.", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_DrawLine(0, 42, OLED_W - 1, 42, SH1107_COLOR_WHITE);

    /* Progress label */
    SH1107_GotoXY(3, 46);
    SH1107_Puts("PROGRESS", &Font_7x10, SH1107_COLOR_WHITE);

    /* Progress bar outline */
    SH1107_DrawRectangle(BAR_X, BAR_Y, BAR_W, BAR_H, SH1107_COLOR_WHITE);

    /* Fill */
    uint16_t fillW = (uint16_t)((uint32_t)samplesDone * (BAR_W - 2) / AUTOZERO_SAMPLES);
    if (fillW > 0) {
        SH1107_DrawFilledRectangle(BAR_X + 1, BAR_Y + 1, fillW, BAR_H - 2,
                                   SH1107_COLOR_WHITE);
    }

    /* Percentage text — centered in bar */
    uint8_t pct = (uint8_t)((uint32_t)samplesDone * 100 / AUTOZERO_SAMPLES);
    snprintf(lineBuf, sizeof(lineBuf), "%3u%%", pct);
    /* 4 chars * 7px = 28px; center at x = (128-28)/2 = 50 */
    SH1107_GotoXY(50, 74);
    SH1107_Puts(lineBuf, &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawLine(0, 86, OLED_W - 1, 86, SH1107_COLOR_WHITE);

    /* Live offset readout */
    SH1107_GotoXY(3, 90);
    SH1107_Puts("Sampling ADC2...", &Font_7x10, SH1107_COLOR_WHITE);

    if (samplesDone > 0) {
        FloatToStr(latestOffset, offBuf, 4);
        snprintf(lineBuf, sizeof(lineBuf), "Offset: %s V", offBuf);
    } else {
        snprintf(lineBuf, sizeof(lineBuf), "Offset: -.---- V");
    }
    SH1107_GotoXY(3, 101);
    SH1107_Puts(lineBuf, &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_UpdateScreen();
}

/* --------------------------------------------------------------------------
 * DrawZeroDoneScreen
 *
 *  ┌──────────────────────────────┐
 *  │     ZERO COMPLETE            │  y=2
 *  ├──────────────────────────────┤  y=13
 *  │ Offset captured:             │  y=18
 *  │ 1.6847        V              │  Font_11x18 value + Font_7x10 unit, y=32
 *  ├──────────────────────────────┤  y=55
 *  │ Samples : 64000              │  y=59
 *  │ Quality : GOOD               │  y=70
 *  ├──────────────────────────────┤  y=83
 *  │ Connect load now.            │  y=88
 *  │ Starting in 1.5s...          │  y=99
 *  ├──────────────────────────────┤  y=113
 *  │      SOC MONITORING          │  y=117
 *  └──────────────────────────────┘
 * -------------------------------------------------------------------------- */
static void DrawZeroDoneScreen(float offset)
{
    char offBuf[12];

    SH1107_Fill(SH1107_COLOR_BLACK);

    /* Header */
    SH1107_GotoXY(8, HDR_Y);
    SH1107_Puts("ZERO COMPLETE", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_DrawLine(0, HDR_LINE_Y, OLED_W - 1, HDR_LINE_Y, SH1107_COLOR_WHITE);

    /* Offset value — Font_11x18 for prominence */
    SH1107_GotoXY(3, 18);
    SH1107_Puts("Offset captured:", &Font_7x10, SH1107_COLOR_WHITE);

    FloatToStr(offset, offBuf, 4);
    SH1107_GotoXY(3, 32);
    SH1107_Puts(offBuf, &Font_11x18, SH1107_COLOR_WHITE);
    /* "V" unit beside value: value is up to 6 chars * 11px = 66px → unit at x=72 */
    SH1107_GotoXY(72, 40);
    SH1107_Puts("V", &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawLine(0, 55, OLED_W - 1, 55, SH1107_COLOR_WHITE);

    /* Stats */
    snprintf(lineBuf, sizeof(lineBuf), "Samples : %u",
             (unsigned)(AUTOZERO_SAMPLES * ADC_OVERSAMPLE_COUNT));
    SH1107_GotoXY(3, 59);
    SH1107_Puts(lineBuf, &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_GotoXY(3, 70);
    SH1107_Puts("Quality : GOOD", &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawLine(0, 83, OLED_W - 1, 83, SH1107_COLOR_WHITE);

    /* Instructions */
    SH1107_GotoXY(3, 88);
    SH1107_Puts("Connect load now.", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_GotoXY(3, 99);
    SH1107_Puts("Starting in 1.5s", &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawLine(0, 113, OLED_W - 1, 113, SH1107_COLOR_WHITE);

    /* Footer brand */
    SH1107_GotoXY(8, 117);
    SH1107_Puts("SOC MONITORING", &Font_7x10, SH1107_COLOR_WHITE);
}

/* --------------------------------------------------------------------------
 * AutoZero_Compute
 *
 * Collects AUTOZERO_SAMPLES readings of ADC2 at 10ms intervals, averages
 * them, and stores the result in currentSensorOffset. Calls
 * DrawCalibratingScreen() every 10 samples to animate the progress bar.
 *
 * REQUIREMENT: no current must flow through WCS1700 during this phase.
 * -------------------------------------------------------------------------- */
static void AutoZero_Compute(void)
{
    double   acc   = 0.0;
    uint16_t valid = 0;

    /* Show initial state with empty bar */
    DrawCalibratingScreen(0, 0.0f);

    for (uint16_t s = 1; s <= AUTOZERO_SAMPLES; s++) {
        uint16_t raw  = ADC_ReadAveraged(&hadc2);
        float    vRaw = (float)raw * ADC_VREF / ADC_MAX_COUNT;
        acc += (double)vRaw;
        valid++;

        /* Compute running average for live offset readout */
        float runningOffset = (float)(acc / valid);

        /* Refresh OLED every 10 samples (~100ms) */
        if ((s % 10) == 0 || s == AUTOZERO_SAMPLES) {
            DrawCalibratingScreen(s, runningOffset);
        }

        HAL_Delay(10);
    }

    if (valid > 0) {
        currentSensorOffset = (float)(acc / (double)valid);
    }
    /* else: currentSensorOffset stays at AUTOZERO_FALLBACK */
}

/* --------------------------------------------------------------------------
 * SD logger
 * -------------------------------------------------------------------------- */

static uint8_t SD_Logger_Init(void)
{
    FRESULT res = sd_mount();

    if (res == FR_NOT_READY) {
        if      (sd_init_error == SD_INIT_ERR_CMD0)   strcpy(sdStatusStr, "NO CARD");
        else if (sd_init_error == SD_INIT_ERR_ACMD41) strcpy(sdStatusStr, "TIMEOUT");
        else                                            strcpy(sdStatusStr, "ERROR");
        return 0;
    }

    if (res != FR_OK) {
        strcpy(sdStatusStr, "NO FAT");
        return 0;
    }

    FRESULT fres = f_open(&logFile, LOG_FILENAME, FA_OPEN_ALWAYS | FA_WRITE);
    if (fres != FR_OK) {
        strcpy(sdStatusStr, "OPEN ERR");
        return 0;
    }

    if (f_size(&logFile) == 0) {
        UINT bw;
        const char *hdr = "Timestamp_s,Voltage_V,Current_A\r\n";
        f_write(&logFile, hdr, strlen(hdr), &bw);
        f_sync(&logFile);
    } else {
        f_lseek(&logFile, f_size(&logFile));
    }

    strcpy(sdStatusStr, "READY");
    return 1;
}

/* --------------------------------------------------------------------------
 * SD_Logger_AddSample
 *
 * FIX: tambah guard buffer overflow sebelum snprintf.
 * Kalau sisa buffer < LOG_LINE_MAXLEN, flush dulu supaya tidak ada
 * snprintf yang return negatif (overflow) dan sample hilang diam-diam.
 *
 * Kolom offset auto-zero sudah dihapus dari CSV — cuma timestamp,
 * voltage, current. Nilai offset masih dipakai internal untuk kalkulasi
 * current real-time, tapi tidak lagi ditulis ke log.
 * -------------------------------------------------------------------------- */
static void SD_Logger_AddSample(float v, float i)
{
    if (!sdReady) return;

    /* Guard: flush dulu kalau buffer hampir penuh */
    if (logChunkLen + LOG_LINE_MAXLEN >= sizeof(logChunk)) {
        SD_Logger_Flush();
        if (!sdReady) return;
    }

    uint32_t now = HAL_GetTick();

    /* Catat tick saat sample pertama masuk sebagai titik nol */
    if (logTotalSamples == 0 && logSampleCount == 0) {
        logTickOffset = now;
    }

    /* Elapsed time dalam detik sejak sample pertama */
    uint32_t elapsed_s = (now - logTickOffset) / 1000UL;

    char vStr[12], iStr[12];
    FloatToStr(v, vStr, 3);
    FloatToStr(i, iStr, 3);

    int n = snprintf(&logChunk[logChunkLen], sizeof(logChunk) - logChunkLen,
                      "%lu,%s,%s\r\n",
                      (unsigned long)elapsed_s, vStr, iStr);

    if (n > 0) logChunkLen += (uint16_t)n;

    if (++logSampleCount >= LOG_BUFFER_SAMPLES) SD_Logger_Flush();
}

static void SD_Logger_Flush(void)
{
    if (!sdReady || logChunkLen == 0) { logSampleCount = 0; return; }

    UINT bw;
    FRESULT res = f_write(&logFile, logChunk, logChunkLen, &bw);

    if (res != FR_OK || bw != logChunkLen) {
        strcpy(sdStatusStr, "WRITE ERR");
        sdReady = 0;
    } else {
        logTotalSamples += logSampleCount;
        if (++logFlushCount >= LOG_SYNC_EVERY_FLUSH) {
            f_sync(&logFile);
            logFlushCount = 0;
        }
    }

    logChunkLen    = 0;
    logSampleCount = 0;
}

/* USER CODE END 4 */

/**
  * @brief System Clock Configuration
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK) Error_Handler();

  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK) Error_Handler();
}

static void MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  hadc1.Instance = ADC1;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK) Error_Handler();
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_71CYCLES_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) Error_Handler();
}

static void MX_ADC2_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  hadc2.Instance = ADC2;
  hadc2.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc2.Init.ContinuousConvMode = ENABLE;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc2.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc2.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc2) != HAL_OK) Error_Handler();
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_71CYCLES_5;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK) Error_Handler();
}

static void MX_SPI1_Init(void)
{
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK) Error_Handler();
}

static void MX_SPI2_Init(void)
{
  hspi2.Instance = SPI2;
  hspi2.Init.Mode = SPI_MODE_MASTER;
  hspi2.Init.Direction = SPI_DIRECTION_2LINES;
  hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi2.Init.NSS = SPI_NSS_SOFT;
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32;
  hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi2.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi2) != HAL_OK) Error_Handler();
  /* Bump SPI2 to /4 (~2 MBit/s on 8MHz APB1) after identification phase */
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  if (HAL_SPI_Init(&hspi2) != HAL_OK) Error_Handler();
}

static void MX_DMA_Init(void)
{
  __HAL_RCC_DMA1_CLK_ENABLE();
  HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);
  HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOB, CS_Pin|DC_Pin|RST_Pin|GPIO_PIN_11, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(CSB6_GPIO_Port, CSB6_Pin, GPIO_PIN_SET);

  GPIO_InitStruct.Pin = CS_Pin|DC_Pin|RST_Pin|GPIO_PIN_11;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = CSB6_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(CSB6_GPIO_Port, &GPIO_InitStruct);
}

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
}
#endif
