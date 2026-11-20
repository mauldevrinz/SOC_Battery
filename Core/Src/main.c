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
#include "sd_functions.h"
#include "sd_spi.h"
#include "ekf_soc.h"
#include "tjc_hmi.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define HMI_REFRESH_INTERVAL_MS  1000U

/* adjustable: ADC / sensor scaling */
#define ADC_VREF                3.3f
#define ADC_MAX_COUNT           4095.0f
#define ADC_OVERSAMPLE_COUNT    64

#define VOLTAGE_DIVIDER_R1      180000.0f
#define VOLTAGE_DIVIDER_R2      10000.0f
#define VOLTAGE_DIVIDER_RATIO   ((VOLTAGE_DIVIDER_R1 + VOLTAGE_DIVIDER_R2) / VOLTAGE_DIVIDER_R2)

#define CURRENT_SENSOR_OFFSET       1.690f
#define CURRENT_SENSOR_SENSITIVITY  (0.033f * (3.3f/5.0f))

/* Pack LiFePO4 4S 100Ah (12.8 V nominal, 10.0-14.6 V operating range). */
#define BATT_CAPACITY_AH        100.0f  /* kapasitas nominal pack (Ah) */
#define BATT_RS_OHM             0.005f  /* estimasi resistansi pack 4S 100Ah (sel ~0.5 mOhm x4 + busbar/kabel/BMS); ukur untuk tuning */
#define VBATT_MIN_VALID         6.0f    /* di bawah ini dianggap baterai belum terpasang */

/* adjustable: re-sync SOC dari OCV saat baterai istirahat (anti SOC "terkunci") */
#define REST_CURRENT_A          2.0f    /* |I| di bawah ini dianggap istirahat (~C/50) */
#define REST_RESYNC_MS          30000U  /* lama istirahat sebelum dicek */
#define RESYNC_SOC_ERR          0.05f   /* beda SOC EKF vs OCV minimal utk re-sync (5%) */
#define RESYNC_EDGE_LO          0.25f   /* re-sync hanya di ujung kurva (OCV informatif), */
#define RESYNC_EDGE_HI          0.85f   /* bukan di tengah plateau LFP yang datar */

/* adjustable: SD card init */
#define SD_POWERUP_DELAY_MS     300
#define SD_INIT_RETRIES         5
#define SD_RETRY_DELAY_MS       200

/* adjustable: EMA smoothing */
#define VOLTAGE_EMA_ALPHA       0.08f
#define CURRENT_EMA_ALPHA       0.04f

/* adjustable: SD logger timing */
#define LOG_FILENAME             "LOG.CSV"
#define LOG_BUFFER_SAMPLES       1
#define LOG_LINE_MAXLEN          80
#define LOG_SYNC_EVERY_FLUSH     1
#define LOG_SAMPLE_DIV           100

/* adjustable: auto-zero sample count */
#define AUTOZERO_SAMPLES        500
#define AUTOZERO_FALLBACK       CURRENT_SENSOR_OFFSET

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc2;

I2C_HandleTypeDef hi2c1;

SPI_HandleTypeDef hspi1;
SPI_HandleTypeDef hspi2;
DMA_HandleTypeDef hdma_spi2_rx;
DMA_HandleTypeDef hdma_spi2_tx;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
volatile float voltage = 0.0f;
volatile float current = 0.0f;
volatile float soc_pct = 0.0f;   /* SOC (%) untuk Live Expressions */
volatile float soh_pct = 100.0f; /* SOH (%) */
volatile float v_internal_bat = 0.0f;
volatile uint8_t sys_bat_pct = 100;
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

static EKF_SOC_HandleTypeDef hekfSoc;
static uint32_t ekfLastTick = 0;
static uint32_t restStartTick = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC2_Init(void);
static void MX_SPI1_Init(void);
static void MX_SPI2_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_I2C1_Init(void);
/* USER CODE BEGIN PFP */

static uint16_t ADC_ReadChannelAveraged(ADC_HandleTypeDef *hadc, uint32_t channel);
static void     FloatToStr(float value, char *buf, uint8_t decimals);
static uint8_t  SD_Logger_Init(void);
static void     SD_Logger_AddSample(float v, float i, float soc_pct);
static void     SD_Logger_Flush(void);
static void     AutoZero_Compute(void);
static void     EKF_SOC_ConfigureModel(EKF_SOC_HandleTypeDef *hekf);
static float    SocFromOcv_LUT(float v_pack);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  uint16_t logSampleCounter = 0;
  uint32_t hmiLastRefresh = 0;
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_SPI1_Init();
  MX_SPI2_Init();
  MX_USART1_UART_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */
  HAL_ADCEx_Calibration_Start(&hadc1);
  HAL_ADCEx_Calibration_Start(&hadc2);

  (void)TJC_HMI_DrawMonitor(0.0f, 0.0f, 0.0f, 100.0f, 100, 0U, sdStatusStr, "INIT");

  sdReady = SD_Logger_Init();
  HAL_Delay(600);

  (void)TJC_HMI_SetStatus("CALIB");
  AutoZero_Compute();
  (void)TJC_HMI_SetStatus("START");

  HAL_Delay(1000);

  /* EKF: ConfigureModel must run before Init */
  EKF_SOC_ConfigureModel(&hekfSoc);
  {
      uint16_t raw_soc_adc = ADC_ReadChannelAveraged(&hadc1, ADC_CHANNEL_0);
      adc1VoltFiltered = (float)raw_soc_adc * ADC_VREF / ADC_MAX_COUNT;
      adc2VoltFiltered = (float)ADC_ReadChannelAveraged(&hadc2, ADC_CHANNEL_1) * ADC_VREF / ADC_MAX_COUNT;
      emaInitialized = 1U;
      float v0   = adc1VoltFiltered * VOLTAGE_DIVIDER_RATIO;
      float soc0 = SocFromOcv_LUT(v0);   /* FIX: SOC awal dari tabel OCV LiFePO4 (bukan inversi polinomial) */
      float x0[2] = { 0.0f, soc0 };
      EKF_SOC_Init(&hekfSoc, x0);
  }
  ekfLastTick = HAL_GetTick();
  (void)TJC_HMI_SetStatus("READY");

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    uint16_t raw1 = ADC_ReadChannelAveraged(&hadc1, ADC_CHANNEL_0);
    uint16_t raw2 = ADC_ReadChannelAveraged(&hadc2, ADC_CHANNEL_1);

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

    /* Read Internal System Battery on PA2 (10k+10k divider -> 2.0x) */
    {
        uint16_t raw_int = ADC_ReadChannelAveraged(&hadc1, ADC_CHANNEL_2);
        float v_pin2 = (float)raw_int * ADC_VREF / ADC_MAX_COUNT;
        v_internal_bat = v_pin2 * 2.0f;
        if (v_internal_bat >= 4.20f) sys_bat_pct = 100;
        else if (v_internal_bat <= 3.00f) sys_bat_pct = 0;
        else sys_bat_pct = (uint8_t)((v_internal_bat - 3.00f) / (4.20f - 3.00f) * 100.0f);
    }

    voltage  = adc1VoltFiltered * VOLTAGE_DIVIDER_RATIO;
    vAdc2Raw = adc2VoltFiltered;
    current  = (vAdc2Raw - currentSensorOffset) / CURRENT_SENSOR_SENSITIVITY;

    {
        uint32_t nowTick = HAL_GetTick();
        float Ts = (float)(nowTick - ekfLastTick) / 1000.0f;
        ekfLastTick = nowTick;
        if (Ts <= 0.0f) Ts = 0.01f;

        /* FIX: abaikan transien saat baterai belum terpasang / tegangan belum valid */
        if (voltage > VBATT_MIN_VALID)
            EKF_SOC_Update(&hekfSoc, current, voltage, Ts);

        /* FIX: re-sync SOC dari OCV bila istirahat lama & SOC EKF menyimpang jauh.
           Tanpa ini EKF bisa "terkunci" di SOC salah (P sudah mengecil, selisih tegangan
           diserap state Vc1). Hanya aktif di ujung kurva OCV (<25% atau >85%). */
        if (voltage > VBATT_MIN_VALID && fabsf(current) < REST_CURRENT_A) {
            if (restStartTick == 0U) restStartTick = nowTick;
            if ((nowTick - restStartTick) >= REST_RESYNC_MS) {
                float socV = SocFromOcv_LUT(voltage);
                float socE = EKF_SOC_GetSoC(&hekfSoc);
                if ((socV < RESYNC_EDGE_LO || socV > RESYNC_EDGE_HI) &&
                    fabsf(socV - socE) > RESYNC_SOC_ERR) {
                    static const float P0[4] = { 0.0001f, 0.0f, 0.0f, 0.01f };
                    EKF_SOC_Reset(&hekfSoc, socV, P0);
                }
                restStartTick = nowTick;   /* cek lagi 30 detik berikutnya */
            }
        } else {
            restStartTick = 0U;
        }

        soc_pct = EKF_SOC_GetSoC(&hekfSoc) * 100.0f;
        if (soc_pct < 0.0f)   soc_pct = 0.0f;
        if (soc_pct > 100.0f) soc_pct = 100.0f;
    }

    if ((HAL_GetTick() - hmiLastRefresh) >= HMI_REFRESH_INTERVAL_MS) {
        hmiLastRefresh = HAL_GetTick();
        (void)TJC_HMI_DrawMonitor(voltage, current, soc_pct, soh_pct, sys_bat_pct,
                                  logTotalSamples, sdStatusStr, "READY");
    }

    if (++logSampleCounter >= LOG_SAMPLE_DIV) {
        logSampleCounter = 0;
        SD_Logger_AddSample(voltage, current, soc_pct);
    }

    HAL_Delay(10);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_71CYCLES_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief ADC2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC2_Init(void)
{

  /* USER CODE BEGIN ADC2_Init 0 */

  /* USER CODE END ADC2_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC2_Init 1 */

  /* USER CODE END ADC2_Init 1 */

  /** Common config
  */
  hadc2.Instance = ADC2;
  hadc2.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc2.Init.ContinuousConvMode = ENABLE;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc2.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc2.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_71CYCLES_5;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC2_Init 2 */

  /* USER CODE END ADC2_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
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
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief SPI2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI2_Init(void)
{

  /* USER CODE BEGIN SPI2_Init 0 */

  /* USER CODE END SPI2_Init 0 */

  /* USER CODE BEGIN SPI2_Init 1 */

  /* USER CODE END SPI2_Init 1 */
  /* SPI2 parameter configuration*/
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
  if (HAL_SPI_Init(&hspi2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI2_Init 2 */

  /* USER CODE END SPI2_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 9600;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel4_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);
  /* DMA1_Channel5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, CS_Pin|DC_Pin|RST_Pin|GPIO_PIN_11, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(CSB6_GPIO_Port, CSB6_Pin, GPIO_PIN_SET);

  /*Configure GPIO pins : CS_Pin DC_Pin RST_Pin PB11 */
  GPIO_InitStruct.Pin = CS_Pin|DC_Pin|RST_Pin|GPIO_PIN_11;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : CSB6_Pin */
  GPIO_InitStruct.Pin = CSB6_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(CSB6_GPIO_Port, &GPIO_InitStruct);

/* USER CODE BEGIN MX_GPIO_Init_2 */
  /* FIX: PA2 = ADC channel 2 (baterai internal) harus mode analog */
  GPIO_InitStruct.Pin  = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

static uint16_t ADC_ReadChannelAveraged(ADC_HandleTypeDef *hadc, uint32_t channel)
{
    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel = channel;
    sConfig.Rank = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_71CYCLES_5;
    HAL_ADC_ConfigChannel(hadc, &sConfig);

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

/* Tabel OCV (tegangan istirahat) pack LiFePO4 4S -> SOC. Dipakai utk SOC awal. */
static float SocFromOcv_LUT(float v_pack)
{
    static const float V[] = {12.00f, 12.85f, 12.95f, 13.04f, 13.12f, 13.19f,
                              13.25f, 13.29f, 13.33f, 13.38f, 13.55f};
    static const float S[] = { 0.0f,  0.1f,   0.2f,   0.3f,   0.4f,   0.5f,
                               0.6f,  0.7f,   0.8f,   0.9f,   1.0f};
    const uint8_t N = 11;

    if (v_pack <= V[0])   return 0.0f;
    if (v_pack >= V[N-1]) return 1.0f;
    for (uint8_t k = 1; k < N; k++) {
        if (v_pack < V[k])
            return S[k-1] + (S[k] - S[k-1]) * (v_pack - V[k-1]) / (V[k] - V[k-1]);
    }
    return 1.0f;
}

static void AutoZero_Compute(void)
{
    double   acc   = 0.0;
    uint16_t valid = 0;

    for (uint16_t s = 1; s <= AUTOZERO_SAMPLES; s++) {
        uint16_t raw  = ADC_ReadChannelAveraged(&hadc2, ADC_CHANNEL_1);
        float    vRaw = (float)raw * ADC_VREF / ADC_MAX_COUNT;
        acc += (double)vRaw;
        valid++;

        HAL_Delay(10);
    }

    if (valid > 0) {
        currentSensorOffset = (float)(acc / (double)valid);
    }
}

/* FIX: ganti prescaler SPI2 saat runtime (init SD wajib <= 400 kHz) */
static void SD_SetSpiPrescaler(uint32_t prescaler)
{
    __HAL_SPI_DISABLE(&hspi2);
    MODIFY_REG(hspi2.Instance->CR1, SPI_CR1_BR, prescaler);
    __HAL_SPI_ENABLE(&hspi2);
}

static uint8_t SD_Logger_Init(void)
{
    extern char sd_path[4];
    FRESULT res = FR_NOT_READY;
    uint8_t attempt;

    /* FIX: beri waktu kartu SD menyala setelah power-up (cold start lewat USB) */
    HAL_Delay(SD_POWERUP_DELAY_MS);

    for (attempt = 0; attempt < SD_INIT_RETRIES; attempt++) {
        SD_SetSpiPrescaler(SPI_BAUDRATEPRESCALER_32);   /* 8 MHz/32 = 250 kHz */
        res = sd_mount();
        if (res != FR_NOT_READY) break;
        FATFS_UnLinkDriver(sd_path);                    /* supaya sd_mount() bisa dicoba lagi */
        HAL_Delay(SD_RETRY_DELAY_MS);
    }
    SD_SetSpiPrescaler(SPI_BAUDRATEPRESCALER_4);        /* kembali ke 2 MHz utk logging */

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
        const char *hdr = "Timestamp_s,Voltage_V,Current_A,Power_W,SoC_pct,SoH_pct\r\n";
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
    float power_w = v * i;

    char vStr[12], iStr[12], pwrStr[12], socStr[12], sohStr[12];
    FloatToStr(v, vStr, 3);
    FloatToStr(i, iStr, 3);
    FloatToStr(power_w, pwrStr, 2);
    FloatToStr(soc_pct, socStr, 2);
    FloatToStr(soh_pct, sohStr, 2);

    int n = snprintf(&logChunk[logChunkLen], sizeof(logChunk) - logChunkLen,
                      "%lu,%s,%s,%s,%s,%s\r\n",
                      (unsigned long)elapsed_s, vStr, iStr, pwrStr, socStr, sohStr);

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
    /* Kapasitas (Ah) dipakai utk coulomb counting dan C-rate */
    hekf->Qn_for_rho = BATT_CAPACITY_AH;

    hekf->rho_lo = 0.0f;
    hekf->rho_hi = 1.0f;          /* C-rate; A_lo == A_hi jadi interpolasi tidak berpengaruh */

    hekf->A_lo[0] = -0.02f;  hekf->A_lo[1] = 0.0f;
    hekf->A_lo[2] = 0.0f;    hekf->A_lo[3] = 0.0f;
    hekf->A_hi[0] = -0.02f;  hekf->A_hi[1] = 0.0f;
    hekf->A_hi[2] = 0.0f;    hekf->A_hi[3] = 0.0f;

    hekf->B_lo[0] = 0.0002f;
    hekf->B_lo[1] = -1.0f / (3600.0f * hekf->Qn_for_rho);
    hekf->B_hi[0] = 0.0002f;
    hekf->B_hi[1] = hekf->B_lo[1];

    hekf->Rs_lo = BATT_RS_OHM;
    hekf->Rs_hi = BATT_RS_OHM;

    hekf->C0[0] = -1.0f;
    hekf->C0[1] =  0.0f;

    /* FIX: OCV pack LiFePO4 4S 100Ah (tegangan istirahat 12.0 V @0% .. 13.55 V @100%),
       polinomial orde 5, koefisien urut dari pangkat tertinggi.
       Plateau tengah datar (turunan kecil) -> koreksi tegangan lemah,
       coulomb counting dominan (disengaja). */
    hekf->ocvCoeffs[0] = 41.826923f;   hekf->ocvCoeffs[1] = -114.678030f;
    hekf->ocvCoeffs[2] = 116.864073f;  hekf->ocvCoeffs[3] = -54.533872f;
    hekf->ocvCoeffs[4] = 12.062550f;   hekf->ocvCoeffs[5] = 12.017919f;
    hekf->ocvOrder     = 5;
    hekf->dOcvCoeffs[0] = 209.134615f; hekf->dOcvCoeffs[1] = -458.712121f;
    hekf->dOcvCoeffs[2] = 350.592220f; hekf->dOcvCoeffs[3] = -109.067745f;
    hekf->dOcvCoeffs[4] = 12.062550f;
    hekf->dOcvOrder     = 4;

    hekf->Q[0] = 0.000001f; hekf->Q[1] = 0.0f;
    hekf->Q[2] = 0.0f;      hekf->Q[3] = 1.0e-10f;   /* derau proses SOC kecil agar coulomb counting dominan, tegangan hanya koreksi pelan */
    hekf->R    = 0.01f;      /* dinaikkan agar SOC tidak terlalu mengikuti noise tegangan */

    hekf->P[0] = 0.0001f; hekf->P[1] = 0.0f;
    hekf->P[2] = 0.0f;    hekf->P[3] = 0.01f;
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
