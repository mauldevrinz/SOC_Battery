#ifndef TJC_HMI_H
#define TJC_HMI_H

#include "stm32f1xx_hal.h"

/* Small UART command driver for TJC/Nextion-compatible serial HMIs. */
void TJC_HMI_Init(UART_HandleTypeDef *huart);
HAL_StatusTypeDef TJC_HMI_SendCommand(const char *command);
HAL_StatusTypeDef TJC_HMI_SetStatus(const char *status_text);
HAL_StatusTypeDef TJC_HMI_DrawMonitor(float voltage, float current,
                                      float soc_percent,
                                      uint32_t logged_samples,
                                      const char *sd_status,
                                      const char *run_status);

#endif
