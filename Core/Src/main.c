/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
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
#include "ekf_soc.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define DISPLAY_REFRESH_DIV     20

#define OLED_W                  SH1107_WIDTH
#define OLED_H                  SH1107_HEIGHT

#define HDR_Y                   2
#define HDR_LINE_Y              13

#define VLT_LABEL_Y             17
#define VLT_VALUE_Y             29
#define VLT_UNIT_Y              35
#define VLT_LINE_Y              51

#define CUR_LABEL_Y             54
#define CUR_VALUE_Y             66
#define CUR_UNIT_Y              72
#define CUR_LINE_Y              89

#define FTR_LINE_Y              90
#define FTR_COL_SEP_X           63
#define FTR_LABEL_Y             93
#define FTR_VALUE_Y             104

/* adjustable: ADC / sensor scaling */
#define ADC_VREF                3.3f
#define ADC_MAX_COUNT           4095.0f
#define ADC_OVERSAMPLE_COUNT    64

#define VOLTAGE_DIVIDER_R1      180000.0f
#define VOLTAGE_DIVIDER_R2      10000.0f
#define VOLTAGE_DIVIDER_RATIO   ((VOLTAGE_DIVIDER_R1 + VOLTAGE_DIVIDER_R2) / VOLTAGE_DIVIDER_R2)

#define CURRENT_SENSOR_OFFSET       1.690f
#define CURRENT_SENSOR_SENSITIVITY  (0.033f * (3.3f/5.0f))

/* adjustable: EMA smoothing */
#define VOLTAGE_EMA_ALPHA       0.08f
#define CURRENT_EMA_ALPHA       0.04f

/* adjustable: SD logger timing */
#define LOG_FILENAME             "LOG.CSV"
#define LOG_BUFFER_SAMPLES       1
#define LOG_LINE_MAXLEN          44
#define LOG_SYNC_EVERY_FLUSH     1
#define LOG_SAMPLE_DIV           100

/* adjustable: auto-zero sample count */
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

static float currentSensorOffset = AUTOZERO_FALLBACK;

static FIL      logFile;
static uint8_t  sdReady = 0;
static char     logChunk[LOG_BUFFER_SAMPLES * LOG_LINE_MAXLEN];
static uint16_t logChunkLen = 0;
static uint8_t  logSampleCount = 0;
static uint16_t logFlushCount = 0;
static uint32_t logTotalSamples = 0;
static uint32_t logTickOffset   = 0;

static char sdStatusStr[10] = "UNKNOWN";
static char lineBuf[24];

static EKF_SOC_HandleTypeDef hekfSoc;
static uint32_t ekfLastTick = 0;

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
static void     SD_Logger_AddSample(float v, float i, float soc_pct);
static void     SD_Logger_Flush(void);
static void     AutoZero_Compute(void);
static void     EKF_SOC_ConfigureModel(EKF_SOC_HandleTypeDef *hekf);
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

  /* USER CODE BEGIN 2 */
  HAL_ADCEx_Calibration_Start(&hadc1);
  HAL_ADCEx_Calibration_Start(&hadc2);

  SH1107_Init();
  SH1107_Fill(SH1107_COLOR_BLACK);
  SH1107_GotoXY(22, 2);
  SH1107_Puts("SOC MONITORING", &Font_7x10, SH1107_COLOR_WHITE);
  SH1107_DrawLine(0, 13, OLED_W - 1, 13, SH1107_COLOR_WHITE);
  SH1107_GotoXY(16, 56);
  SH1107_Puts("Initializing...", &Font_7x10, SH1107_COLOR_WHITE);
  SH1107_UpdateScreen();

  sdReady = SD_Logger_Init();
  HAL_Delay(600);

  AutoZero_Compute();

  DrawZeroDoneScreen(currentSensorOffset);
  SH1107_UpdateScreen();
  HAL_Delay(1500);

  /* EKF: ConfigureModel must run before Init */
  EKF_SOC_ConfigureModel(&hekfSoc);
  {
      float x0[2] = { 0.0f, 1.0f };
      EKF_SOC_Init(&hekfSoc, x0);
  }
  ekfLastTick = HAL_GetTick();

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

    {
        uint32_t nowTick = HAL_GetTick();
        float Ts = (float)(nowTick - ekfLastTick) / 1000.0f;
        ekfLastTick = nowTick;
        if (Ts <= 0.0f) Ts = 0.01f;

        EKF_SOC_Update(&hekfSoc, current, voltage, Ts);
    }

    if (++displayCounter >= DISPLAY_REFRESH_DIV) {
        displayCounter = 0;
        DrawMainScreen(voltage, current);
    }

    if (++logSampleCounter >= LOG_SAMPLE_DIV) {
        logSampleCounter = 0;
        SD_Logger_AddSample(voltage, current, EKF_SOC_GetSoC(&hekfSoc) * 100.0f);
    }

    HAL_Delay(10);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/* USER CODE BEGIN 4 */

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

static void DrawMainScreen(float v, float i)
{
    char numBuf[12];

    SH1107_DrawFilledRectangle(0, 0, OLED_W, 13, SH1107_COLOR_BLACK);
    SH1107_GotoXY(8, HDR_Y);
    SH1107_Puts("SOC MONITORING", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_DrawLine(0, HDR_LINE_Y, OLED_W - 1, HDR_LINE_Y, SH1107_COLOR_WHITE);

    SH1107_DrawFilledRectangle(0, VLT_LABEL_Y, OLED_W, VLT_LINE_Y - VLT_LABEL_Y + 1,
                               SH1107_COLOR_BLACK);
    SH1107_GotoXY(3, VLT_LABEL_Y);
    SH1107_Puts("VOLTAGE", &Font_7x10, SH1107_COLOR_WHITE);

    FloatToStr(v, numBuf, 2);
    SH1107_GotoXY(3, VLT_VALUE_Y);
    SH1107_Puts(numBuf, &Font_11x18, SH1107_COLOR_WHITE);
    SH1107_GotoXY(116, VLT_UNIT_Y);
    SH1107_Puts("V", &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawLine(0, VLT_LINE_Y, OLED_W - 1, VLT_LINE_Y, SH1107_COLOR_WHITE);

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

    SH1107_DrawFilledRectangle(0, FTR_LINE_Y + 1, OLED_W,
                               OLED_H - FTR_LINE_Y - 1, SH1107_COLOR_BLACK);

    SH1107_GotoXY(3, FTR_LABEL_Y);
    SH1107_Puts("LOGGED", &Font_7x10, SH1107_COLOR_WHITE);
    snprintf(lineBuf, sizeof(lineBuf), "%06lu", (unsigned long)logTotalSamples);
    SH1107_GotoXY(3, FTR_VALUE_Y);
    SH1107_Puts(lineBuf, &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawLine(FTR_COL_SEP_X, FTR_LINE_Y + 1, FTR_COL_SEP_X,
                    OLED_H - 1, SH1107_COLOR_WHITE);

    SH1107_GotoXY(67, FTR_LABEL_Y);
    SH1107_Puts("SD CARD", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_GotoXY(67, FTR_VALUE_Y);
    SH1107_Puts(sdStatusStr, &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_UpdateScreen();
}

static void DrawCalibratingScreen(uint16_t samplesDone, float latestOffset)
{
    const uint16_t BAR_X = 3, BAR_Y = 58, BAR_W = 122, BAR_H = 12;
    char offBuf[12];

    SH1107_Fill(SH1107_COLOR_BLACK);

    SH1107_GotoXY(15, HDR_Y);
    SH1107_Puts("CALIBRATING", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_DrawLine(0, HDR_LINE_Y, OLED_W - 1, HDR_LINE_Y, SH1107_COLOR_WHITE);

    SH1107_GotoXY(3, 18);
    SH1107_Puts("No load on sensor", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_GotoXY(3, 29);
    SH1107_Puts("input. Zero amps.", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_DrawLine(0, 42, OLED_W - 1, 42, SH1107_COLOR_WHITE);

    SH1107_GotoXY(3, 46);
    SH1107_Puts("PROGRESS", &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawRectangle(BAR_X, BAR_Y, BAR_W, BAR_H, SH1107_COLOR_WHITE);

    uint16_t fillW = (uint16_t)((uint32_t)samplesDone * (BAR_W - 2) / AUTOZERO_SAMPLES);
    if (fillW > 0) {
        SH1107_DrawFilledRectangle(BAR_X + 1, BAR_Y + 1, fillW, BAR_H - 2,
                                   SH1107_COLOR_WHITE);
    }

    uint8_t pct = (uint8_t)((uint32_t)samplesDone * 100 / AUTOZERO_SAMPLES);
    snprintf(lineBuf, sizeof(lineBuf), "%3u%%", pct);
    SH1107_GotoXY(50, 74);
    SH1107_Puts(lineBuf, &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawLine(0, 86, OLED_W - 1, 86, SH1107_COLOR_WHITE);

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

static void DrawZeroDoneScreen(float offset)
{
    char offBuf[12];

    SH1107_Fill(SH1107_COLOR_BLACK);

    SH1107_GotoXY(8, HDR_Y);
    SH1107_Puts("ZERO COMPLETE", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_DrawLine(0, HDR_LINE_Y, OLED_W - 1, HDR_LINE_Y, SH1107_COLOR_WHITE);

    SH1107_GotoXY(3, 18);
    SH1107_Puts("Offset captured:", &Font_7x10, SH1107_COLOR_WHITE);

    FloatToStr(offset, offBuf, 4);
    SH1107_GotoXY(3, 32);
    SH1107_Puts(offBuf, &Font_11x18, SH1107_COLOR_WHITE);
    SH1107_GotoXY(72, 40);
    SH1107_Puts("V", &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawLine(0, 55, OLED_W - 1, 55, SH1107_COLOR_WHITE);

    snprintf(lineBuf, sizeof(lineBuf), "Samples : %u",
             (unsigned)(AUTOZERO_SAMPLES * ADC_OVERSAMPLE_COUNT));
    SH1107_GotoXY(3, 59);
    SH1107_Puts(lineBuf, &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_GotoXY(3, 70);
    SH1107_Puts("Quality : GOOD", &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawLine(0, 83, OLED_W - 1, 83, SH1107_COLOR_WHITE);

    SH1107_GotoXY(3, 88);
    SH1107_Puts("Connect load now.", &Font_7x10, SH1107_COLOR_WHITE);
    SH1107_GotoXY(3, 99);
    SH1107_Puts("Starting in 1.5s", &Font_7x10, SH1107_COLOR_WHITE);

    SH1107_DrawLine(0, 113, OLED_W - 1, 113, SH1107_COLOR_WHITE);

    SH1107_GotoXY(8, 117);
    SH1107_Puts("SOC MONITORING", &Font_7x10, SH1107_COLOR_WHITE);
}

static void AutoZero_Compute(void)
{
    double   acc   = 0.0;
    uint16_t valid = 0;

    DrawCalibratingScreen(0, 0.0f);

    for (uint16_t s = 1; s <= AUTOZERO_SAMPLES; s++) {
        uint16_t raw  = ADC_ReadAveraged(&hadc2);
        float    vRaw = (float)raw * ADC_VREF / ADC_MAX_COUNT;
        acc += (double)vRaw;
        valid++;

        float runningOffset = (float)(acc / valid);

        if ((s % 10) == 0 || s == AUTOZERO_SAMPLES) {
            DrawCalibratingScreen(s, runningOffset);
        }

        HAL_Delay(10);
    }

    if (valid > 0) {
        currentSensorOffset = (float)(acc / (double)valid);
    }
}

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
        const char *hdr = "Timestamp_s,Voltage_V,Current_A,SoC_pct\r\n";
        f_write(&logFile, hdr, strlen(hdr), &bw);
        f_sync(&logFile);
    } else {
        f_lseek(&logFile, f_size(&logFile));
    }

    strcpy(sdStatusStr, "READY");
    return 1;
}

static void SD_Logger_AddSample(float v, float i, float soc_pct)
{
    if (!sdReady) return;

    if (logChunkLen + LOG_LINE_MAXLEN >= sizeof(logChunk)) {
        SD_Logger_Flush();
        if (!sdReady) return;
    }

    uint32_t now = HAL_GetTick();

    if (logTotalSamples == 0 && logSampleCount == 0) {
        logTickOffset = now;
    }

    uint32_t elapsed_s = (now - logTickOffset) / 1000UL;

    char vStr[12], iStr[12], socStr[12];
    FloatToStr(v, vStr, 3);
    FloatToStr(i, iStr, 3);
    FloatToStr(soc_pct, socStr, 2);

    int n = snprintf(&logChunk[logChunkLen], sizeof(logChunk) - logChunkLen,
                      "%lu,%s,%s,%s\r\n",
                      (unsigned long)elapsed_s, vStr, iStr, socStr);

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

/* adjustable: EKF battery model / tuning — the one function to edit */
static void EKF_SOC_ConfigureModel(EKF_SOC_HandleTypeDef *hekf)
{
    hekf->Qn_for_rho = 100.0f;

    hekf->rho_lo = 0.0f;
    hekf->rho_hi = 0.05f;

    hekf->A_lo[0] = -0.02f;  hekf->A_lo[1] = 0.0f;
    hekf->A_lo[2] = 0.0f;    hekf->A_lo[3] = 0.0f;
    hekf->A_hi[0] = -0.02f;  hekf->A_hi[1] = 0.0f;
    hekf->A_hi[2] = 0.0f;    hekf->A_hi[3] = 0.0f;

    hekf->B_lo[0] = 0.0002f;
    hekf->B_lo[1] = -1.0f / (3600.0f * hekf->Qn_for_rho);
    hekf->B_hi[0] = 0.0002f;
    hekf->B_hi[1] = hekf->B_lo[1];

    hekf->Rs_lo = 0.02f;
    hekf->Rs_hi = 0.02f;

    hekf->C0[0] = -1.0f;
    hekf->C0[1] =  0.0f;

    hekf->ocvCoeffs[0] = 4.8f;   hekf->ocvCoeffs[1] = 12.0f;
    hekf->ocvOrder      = 1;
    hekf->dOcvCoeffs[0] = 4.8f;
    hekf->dOcvOrder      = 0;

    hekf->Q[0] = 0.000001f; hekf->Q[1] = 0.0f;
    hekf->Q[2] = 0.0f;      hekf->Q[3] = 0.000001f;
    hekf->R    = 0.0001f;

    hekf->P[0] = 0.0001f; hekf->P[1] = 0.0f;
    hekf->P[2] = 0.0f;    hekf->P[3] = 0.01f;
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
