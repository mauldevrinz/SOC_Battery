/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Refresh the OLED every DISPLAY_REFRESH_DIV loop iterations instead of
 * every single iteration (the main loop runs every ~10ms thanks to
 * HAL_Delay(10), a full SH1107 frame push takes a few ms over SPI, so we
 * throttle it to avoid hogging the bus / flicker without slowing the ADC). */
#define DISPLAY_REFRESH_DIV   20

/* ---- SD card data logger tuning -----------------------------------------
 * Logging a CSV line every single 10ms ADC tick would mean opening the
 * FAT layer (and doing an SPI/SD transaction) 100 times a second, which is
 * by far the most expensive thing this loop could do. Instead we:
 *   1) keep the log file OPEN for the whole session (no per-write
 *      open/close - each f_open/f_close walks the FAT directory entries),
 *   2) batch LOG_BUFFER_SAMPLES samples into a RAM buffer and push them to
 *      the card with a single f_write() call,
 *   3) call f_sync() (flush only, file stays open) every
 *      LOG_SYNC_EVERY_FLUSH batches instead of after every batch, since
 *      f_sync() forces an extra SD write of the FAT/dir sector itself.
 * Net effect: an SD card transaction happens roughly once every
 * (LOG_BUFFER_SAMPLES * 10ms) instead of every 10ms - a 20x reduction
 * here - and the FAT metadata is only re-written once every
 * (LOG_BUFFER_SAMPLES * LOG_SYNC_EVERY_FLUSH * 10ms).
 * --------------------------------------------------------------------- */
#define LOG_FILENAME            "LOG.CSV"
#define LOG_BUFFER_SAMPLES      20    /* batch ~200ms of samples per SD write */
#define LOG_LINE_MAXLEN         32    /* "4294967295,-12.345,-12.345\r\n" fits */
#define LOG_SYNC_EVERY_FLUSH    10    /* f_sync() roughly every ~2s          */
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
volatile uint16_t readValue1 = 0;
volatile uint16_t readValue2 = 0;
volatile float voltage = 0.0f;
volatile float current = 0.0f;

static char line_buf[24];

/* SD logger state */
static FIL   logFile;
static uint8_t sdReady = 0;
static char  logChunk[LOG_BUFFER_SAMPLES * LOG_LINE_MAXLEN];
static uint16_t logChunkLen = 0;
static uint8_t  logSampleCount = 0;
static uint16_t logFlushCount = 0;
static uint32_t logTotalSamples = 0;
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
static void FloatToStr(float value, char* buf, uint8_t decimals);
static void UpdateDisplay(float v1, float v2);

static uint8_t SD_Logger_Init(void);
static void SD_Logger_Flush(void);
static void SD_Logger_AddSample(uint32_t timestamp_ms, float v, float i);
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
  uint16_t displayCounter = 0;
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
  MX_FATFS_Init();
  /* USER CODE BEGIN 2 */
  HAL_ADCEx_Calibration_Start(&hadc1);
  HAL_ADCEx_Calibration_Start(&hadc2);

  /* Init OLED and show a splash/title before the loop starts */
  SH1107_Init();
  SH1107_Fill(SH1107_COLOR_BLACK);
  SH1107_GotoXY(5, 5);
  SH1107_Puts("Voltage Monitor", &Font_7x10, SH1107_COLOR_WHITE);
  SH1107_DrawLine(0, 18, SH1107_WIDTH - 1, 18, SH1107_COLOR_WHITE);
  SH1107_UpdateScreen();

  /* Mount the SD card and open the log file once. If this fails (no card,
   * bad wiring, unformatted card, ...) we simply skip logging for the rest
   * of the session instead of retrying on every loop iteration - retrying
   * a full SD init/mount sequence every 10ms would stall the ADC loop. */
  sdReady = SD_Logger_Init();

  SH1107_GotoXY(5, 100);
  SH1107_Puts(sdReady ? "SD: READY" : "SD: NOT FOUND", &Font_7x10, SH1107_COLOR_WHITE);
  SH1107_UpdateScreen();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
	    /* ADC1 */
	    HAL_ADC_Start(&hadc1);

	    if(HAL_ADC_PollForConversion(&hadc1, 100) == HAL_OK)
	    {
	        readValue1 = HAL_ADC_GetValue(&hadc1);
	    }

	    HAL_ADC_Stop(&hadc1);

	    /* ADC2 */
	    HAL_ADC_Start(&hadc2);

	    if(HAL_ADC_PollForConversion(&hadc2, 100) == HAL_OK)
	    {
	        readValue2 = HAL_ADC_GetValue(&hadc2);
	    }

	    HAL_ADC_Stop(&hadc2);

	    /* Konversi ke tegangan & arus */
	    voltage = ((float)readValue1 * 3.3f) / 4095.0f;
	    current = ((float)readValue2 * 3.3f) / 4095.0f;

	    /* USER CODE BEGIN OLED */
	    if (++displayCounter >= DISPLAY_REFRESH_DIV)
	    {
	        displayCounter = 0;
	        UpdateDisplay(voltage, current);
	    }
	    /* USER CODE END OLED */

	    /* USER CODE BEGIN SDLOG */
	    SD_Logger_AddSample(HAL_GetTick(), voltage, current);
	    /* USER CODE END SDLOG */

	    HAL_Delay(10);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/* USER CODE BEGIN 4 */

/**
  * @brief  Converts a float to a string with a fixed number of decimals,
  *         without relying on the C library's float printf support
  *         (keeps the build working even if -u _printf_float / the
  *         float-enabled newlib-nano spec isn't set up in the project).
  * @param  value: value to convert
  * @param  buf:   destination buffer, must be large enough
  * @param  decimals: number of decimal digits (supports up to 3 cleanly)
  */
static void FloatToStr(float value, char* buf, uint8_t decimals)
{
	int32_t scale = 1;
	for (uint8_t i = 0; i < decimals; i++) { scale *= 10; }

	int32_t scaled = (int32_t)(value * scale + (value >= 0 ? 0.5f : -0.5f));
	int32_t whole  = scaled / scale;
	int32_t frac   = scaled % scale;
	if (frac < 0) { frac = -frac; }

	sprintf(buf, "%ld.%0*ld", (long)whole, decimals, (long)frac);
}

/**
  * @brief  Redraws the two sensor readouts + logger status on the OLED.
  */
static void UpdateDisplay(float v1, float v2)
{
	char numStr[12];

	/* Clear just the data area, keep the title/line at the top and the
	 * SD status line at the bottom untouched */
	SH1107_DrawFilledRectangle(0, 20, SH1107_WIDTH - 1, 75, SH1107_COLOR_BLACK);

	FloatToStr(v1, numStr, 2);
	snprintf(line_buf, sizeof(line_buf), "Volt: %s V", numStr);
	SH1107_GotoXY(5, 30);
	SH1107_Puts(line_buf, &Font_7x10, SH1107_COLOR_WHITE);

	FloatToStr(v2, numStr, 2);
	snprintf(line_buf, sizeof(line_buf), "Curr: %s A", numStr);
	SH1107_GotoXY(5, 50);
	SH1107_Puts(line_buf, &Font_7x10, SH1107_COLOR_WHITE);

	if (sdReady) {
		snprintf(line_buf, sizeof(line_buf), "Logged: %lu", (unsigned long)logTotalSamples);
		SH1107_GotoXY(5, 70);
		SH1107_Puts(line_buf, &Font_7x10, SH1107_COLOR_WHITE);
	}

	SH1107_UpdateScreen();
}

/**
  * @brief  Mounts the SD card and opens (or creates) the CSV log file once.
  *         The file is left open for the entire session - SD_Logger_Flush()
  *         writes batches into it and SD_Logger_AddSample() is what callers
  *         use every loop tick.
  * @retval 1 if the card is mounted and the file is open and ready to log,
  *         0 otherwise (caller should just skip logging for this session).
  */
static uint8_t SD_Logger_Init(void)
{
	if (sd_mount() != FR_OK) {
		return 0;
	}

	FRESULT res = f_open(&logFile, LOG_FILENAME, FA_OPEN_ALWAYS | FA_WRITE);
	if (res != FR_OK) {
		printf("SD_Logger: f_open failed (%d)\r\n", res);
		return 0;
	}

	/* Always append from where we left off across power cycles */
	if (f_size(&logFile) == 0) {
		/* Brand new / empty file: write the CSV header once */
		UINT bw;
		const char *header = "Timestamp_ms,Voltage_V,Current_A\r\n";
		f_write(&logFile, header, strlen(header), &bw);
		f_sync(&logFile);
	} else {
		f_lseek(&logFile, f_size(&logFile));
	}

	return 1;
}

/**
  * @brief  Appends one CSV line ("timestamp,voltage,current\r\n") into the
  *         RAM batch buffer. Only triggers an actual SD write once
  *         LOG_BUFFER_SAMPLES samples have piled up - see the tuning
  *         comment above LOG_BUFFER_SAMPLES for why.
  */
static void SD_Logger_AddSample(uint32_t timestamp_ms, float v, float i)
{
	if (!sdReady) {
		return;
	}

	char vStr[12], iStr[12];
	FloatToStr(v, vStr, 3);
	FloatToStr(i, iStr, 3);

	int n = snprintf(&logChunk[logChunkLen], sizeof(logChunk) - logChunkLen,
	                  "%lu,%s,%s\r\n", (unsigned long)timestamp_ms, vStr, iStr);

	if (n > 0) {
		logChunkLen += (uint16_t)n;
	}

	if (++logSampleCount >= LOG_BUFFER_SAMPLES) {
		SD_Logger_Flush();
	}
}

/**
  * @brief  Pushes the accumulated RAM batch to the SD card in one f_write()
  *         call, and syncs (without closing) every LOG_SYNC_EVERY_FLUSH
  *         batches. If a write ever fails (e.g. card removed), logging is
  *         disabled for the rest of the session rather than retried every
  *         loop tick.
  */
static void SD_Logger_Flush(void)
{
	if (!sdReady || logChunkLen == 0) {
		logSampleCount = 0;
		return;
	}

	UINT bw;
	FRESULT res = f_write(&logFile, logChunk, logChunkLen, &bw);

	if (res != FR_OK || bw != logChunkLen) {
		printf("SD_Logger: f_write failed (%d) - disabling logging\r\n", res);
		sdReady = 0;
	} else {
		logTotalSamples += logSampleCount;

		if (++logFlushCount >= LOG_SYNC_EVERY_FLUSH) {
			f_sync(&logFile);
			logFlushCount = 0;
		}
	}

	logChunkLen = 0;
	logSampleCount = 0;
}
/* USER CODE END 4 */

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
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL2;
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

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV2;
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
  /* CubeMX's default prescaler (/32) gives ~250 KBit/s on this APB1 bus,
   * which is needlessly conservative once the card is past identification
   * - it roughly quadruples every SD transaction's duration for no benefit.
   * Bump it to /4 (~2 MBit/s on this 8MHz APB1 clock), matching the speed
   * the reference tutorial itself settled on as a safe-but-fast value for
   * the exact same wiring. Re-running HAL_SPI_Init() here is safe since
   * SPI2 hasn't been used yet at this point in the boot sequence. */
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  if (HAL_SPI_Init(&hspi2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE END SPI2_Init 2 */

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
/* USER CODE END MX_GPIO_Init_2 */
}

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
