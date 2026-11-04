#include "tjc_hmi.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define TJC_HMI_TIMEOUT_MS  100U

#define HMI_COLOR_BG        0U
#define HMI_COLOR_HEADER    0U
#define HMI_COLOR_CARD      0U
#define HMI_COLOR_BORDER    31U
#define HMI_COLOR_ACCENT    31U
#define HMI_COLOR_TEXT      65535U
#define HMI_COLOR_MUTED     65535U
#define HMI_COLOR_GREEN     2016U
#define HMI_COLOR_RED       63488U

static UART_HandleTypeDef *s_huart;
static uint8_t s_screen_initialized;
static char s_last_status[16];
static int32_t s_last_measurement[2] = {-1, -1};
static int8_t s_last_measurement_sign[2] = {0, 0};
static char s_last_footer[24];
static char s_last_sd_label[12];

void TJC_HMI_Init(UART_HandleTypeDef *huart)
{
    s_huart = huart;
    s_screen_initialized = 0U;
    s_last_status[0] = '\0';
    s_last_measurement[0] = -1;
    s_last_measurement[1] = -1;
    s_last_measurement_sign[0] = 0;
    s_last_measurement_sign[1] = 0;
    s_last_footer[0] = '\0';
    s_last_sd_label[0] = '\0';
}

HAL_StatusTypeDef TJC_HMI_SendCommand(const char *command)
{
    static const uint8_t terminator[3] = {0xFF, 0xFF, 0xFF};
    HAL_StatusTypeDef status;

    if ((s_huart == NULL) || (command == NULL)) return HAL_ERROR;

    status = HAL_UART_Transmit(s_huart, (uint8_t *)command,
                               (uint16_t)strlen(command), TJC_HMI_TIMEOUT_MS);
    if (status != HAL_OK) return status;

    return HAL_UART_Transmit(s_huart, (uint8_t *)terminator,
                             sizeof(terminator), TJC_HMI_TIMEOUT_MS);
}

static HAL_StatusTypeDef TJC_HMI_DrawText(uint16_t x, uint16_t y,
                                           uint16_t w, uint16_t h,
                                           uint8_t font, uint16_t fg,
                                           uint16_t bg, uint8_t align,
                                           const char *text)
{
    char command[96];
    int n;

    /* xstr draws text directly; no HMI project or component names required. */
    n = snprintf(command, sizeof(command),
                 "xstr %u,%u,%u,%u,%u,%u,%u,%u,1,1,\"%s\"",
                 x, y, w, h, font, fg, bg, align, text);
    if (n <= 0 || (size_t)n >= sizeof(command)) return HAL_ERROR;
    return TJC_HMI_SendCommand(command);
}

static HAL_StatusTypeDef TJC_HMI_DrawBox(uint16_t x, uint16_t y,
                                          uint16_t w, uint16_t h,
                                          uint16_t fill_color)
{
    char command[48];
    int n;

    n = snprintf(command, sizeof(command), "fill %u,%u,%u,%u,%u",
                 x, y, w, h, fill_color);
    if (n <= 0 || (size_t)n >= sizeof(command)) return HAL_ERROR;
    if (TJC_HMI_SendCommand(command) != HAL_OK) return HAL_ERROR;

    n = snprintf(command, sizeof(command), "draw %u,%u,%u,%u,%u",
                 x, y, (uint16_t)(x + w - 1U), (uint16_t)(y + h - 1U),
                 HMI_COLOR_BORDER);
    if (n <= 0 || (size_t)n >= sizeof(command)) return HAL_ERROR;
    return TJC_HMI_SendCommand(command);
}

static HAL_StatusTypeDef TJC_HMI_FillRect(uint16_t x, uint16_t y,
                                           uint16_t w, uint16_t h,
                                           uint16_t color)
{
    char command[48];
    int n = snprintf(command, sizeof(command), "fill %u,%u,%u,%u,%u",
                     x, y, w, h, color);
    if (n <= 0 || (size_t)n >= sizeof(command)) return HAL_ERROR;
    return TJC_HMI_SendCommand(command);
}

/* Draw minus and decimal comma as graphics so missing font glyphs cannot hide them. */
static HAL_StatusTypeDef TJC_HMI_DrawMeasurement(float value, uint16_t card_x,
                                                  uint16_t y, const char *unit)
{
    float magnitude;
    int32_t scaled;
    uint8_t index = (card_x == 12U) ? 0U : 1U;
    int8_t negative = (value < 0.0f) ? 1 : 0;
    char whole[16];
    char fraction[4];
    HAL_StatusTypeDef status;

    if (!isfinite(value) || value > 999.99f || value < -999.99f) {
        status = TJC_HMI_FillRect((uint16_t)(card_x + 3U), (uint16_t)(y + 2U),
                                  130U, 41U, HMI_COLOR_CARD);
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText((uint16_t)(card_x + 14U), y, 80U, 45U, 1U,
                                  HMI_COLOR_TEXT, HMI_COLOR_CARD, 2U,
                                  isfinite(value) ? "OVR" : "ERR");
        if (status != HAL_OK) return status;
        return TJC_HMI_DrawText((uint16_t)(card_x + 106U), y, 26U, 45U, 1U,
                                HMI_COLOR_TEXT, HMI_COLOR_CARD, 0U, unit);
    }
    magnitude = value < 0.0f ? -value : value;
    scaled = (int32_t)(magnitude * 100.0f + 0.5f);
    if (s_last_measurement[index] == scaled &&
        s_last_measurement_sign[index] == negative)
        return HAL_OK;
    (void)snprintf(whole, sizeof(whole), "%ld", (long)(scaled / 100));
    (void)snprintf(fraction, sizeof(fraction), "%02ld", (long)(scaled % 100));

    if (s_last_measurement[index] < 0 ||
        s_last_measurement_sign[index] != negative) {
        status = TJC_HMI_FillRect((uint16_t)(card_x + 3U), (uint16_t)(y + 19U),
                                  9U, 5U, HMI_COLOR_CARD);
        if (status != HAL_OK) return status;
        if (negative) {
            status = TJC_HMI_FillRect((uint16_t)(card_x + 4U), (uint16_t)(y + 21U),
                                      7U, 2U, HMI_COLOR_TEXT);
            if (status != HAL_OK) return status;
        }
    }

    if (s_last_measurement[index] != scaled) {
        status = TJC_HMI_DrawText((uint16_t)(card_x + 14U), y, 50U, 45U, 1U,
                                  HMI_COLOR_TEXT, HMI_COLOR_CARD, 2U, whole);
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText((uint16_t)(card_x + 74U), y, 32U, 45U, 1U,
                                  HMI_COLOR_TEXT, HMI_COLOR_CARD, 0U, fraction);
        if (status != HAL_OK) return status;
    }

    if (s_last_measurement[index] < 0) {
        status = TJC_HMI_FillRect((uint16_t)(card_x + 65U), (uint16_t)(y + 29U),
                                  8U, 13U, HMI_COLOR_CARD);
        if (status != HAL_OK) return status;
        status = TJC_HMI_FillRect((uint16_t)(card_x + 66U), (uint16_t)(y + 31U),
                                  4U, 4U, HMI_COLOR_TEXT);
        if (status != HAL_OK) return status;
        status = TJC_HMI_FillRect((uint16_t)(card_x + 68U), (uint16_t)(y + 34U),
                                  2U, 4U, HMI_COLOR_TEXT);
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText((uint16_t)(card_x + 108U), y, 24U, 45U, 1U,
                                  HMI_COLOR_TEXT, HMI_COLOR_CARD, 0U, unit);
        if (status != HAL_OK) return status;
    }

    s_last_measurement[index] = scaled;
    s_last_measurement_sign[index] = negative;
    return HAL_OK;
}

HAL_StatusTypeDef TJC_HMI_SetStatus(const char *status_text)
{
    uint16_t text_color;
    HAL_StatusTypeDef status;

    if (status_text == NULL) status_text = "UNKNOWN";
    if (s_last_status[0] != '\0' && strcmp(s_last_status, status_text) == 0)
        return HAL_OK;
    if (strncmp(status_text, "READY", 5U) == 0)
        text_color = HMI_COLOR_GREEN;
    else if (strncmp(status_text, "CALIB", 5U) == 0)
        text_color = HMI_COLOR_RED;
    else
        text_color = HMI_COLOR_TEXT;

    status = TJC_HMI_FillRect(229U, 2U, 79U, 34U, HMI_COLOR_HEADER);
    if (status != HAL_OK) return status;
    status = TJC_HMI_DrawText(230, 2, 77, 34, 1, text_color,
                              HMI_COLOR_HEADER, 1, status_text);
    if (status == HAL_OK) {
        (void)snprintf(s_last_status, sizeof(s_last_status), "%s", status_text);
    }
    return status;
}

HAL_StatusTypeDef TJC_HMI_DrawMonitor(float voltage, float current,
                                      float soc_percent,
                                      uint32_t logged_samples,
                                      const char *sd_status,
                                      const char *run_status)
{
    char value[24];
    char sd_label[12];
    char footer[24];
    uint8_t soc_display;
    HAL_StatusTypeDef status;

    if (sd_status == NULL) sd_status = "UNKNOWN";
    if (run_status == NULL) run_status = "UNKNOWN";

    if (strcmp(sd_status, "READY") == 0) (void)snprintf(sd_label, sizeof(sd_label), "SD OK");
    else if (strcmp(sd_status, "NO CARD") == 0) (void)snprintf(sd_label, sizeof(sd_label), "SD NC");
    else if (strcmp(sd_status, "TIMEOUT") == 0) (void)snprintf(sd_label, sizeof(sd_label), "SD TO");
    else if (strcmp(sd_status, "NO FAT") == 0) (void)snprintf(sd_label, sizeof(sd_label), "SD FAT");
    else if (strcmp(sd_status, "OPEN ERR") == 0) (void)snprintf(sd_label, sizeof(sd_label), "SD OPEN");
    else if (strcmp(sd_status, "WRITE ERR") == 0) (void)snprintf(sd_label, sizeof(sd_label), "SD WRITE");
    else (void)snprintf(sd_label, sizeof(sd_label), "SD ERR");

    if (!s_screen_initialized) {
        status = TJC_HMI_SendCommand("cls 0");
        if (status != HAL_OK) return status;
        status = TJC_HMI_FillRect(0U, 0U, 320U, 240U, HMI_COLOR_BG);
        if (status != HAL_OK) return status;
        status = TJC_HMI_SendCommand("line 12,37,308,37,31");
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText(12, 2, 210, 34, 1, HMI_COLOR_TEXT, HMI_COLOR_HEADER, 0, "SOC BATTERY");
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawBox(12, 48, 144, 88, HMI_COLOR_CARD);
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawBox(164, 48, 144, 88, HMI_COLOR_CARD);
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawBox(12, 145, 296, 76, HMI_COLOR_CARD);
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText(24, 48, 124, 30, 1, HMI_COLOR_TEXT, HMI_COLOR_CARD, 0, "VOLTAGE");
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText(176, 48, 124, 30, 1, HMI_COLOR_TEXT, HMI_COLOR_CARD, 0, "CURRENT");
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText(24, 145, 120, 32, 1, HMI_COLOR_TEXT, HMI_COLOR_CARD, 0, "SOC");
        if (status != HAL_OK) return status;
        status = TJC_HMI_FillRect(274U, 153U, 5U, 5U, HMI_COLOR_TEXT);
        if (status != HAL_OK) return status;
        status = TJC_HMI_SendCommand("line 276,170,287,153,65535");
        if (status != HAL_OK) return status;
        status = TJC_HMI_FillRect(284U, 164U, 5U, 5U, HMI_COLOR_TEXT);
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText(24, 184, 150, 32, 1, HMI_COLOR_TEXT, HMI_COLOR_CARD, 0, "LOG: 0");
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText(184, 184, 112, 32, 1, HMI_COLOR_TEXT, HMI_COLOR_CARD, 0, sd_label);
        if (status != HAL_OK) return status;
        s_screen_initialized = 1U;
    }

    status = TJC_HMI_DrawMeasurement(voltage, 12U, 78U, "V");
    if (status != HAL_OK) return status;
    status = TJC_HMI_DrawMeasurement(current, 164U, 78U, "A");
    if (status != HAL_OK) return status;

    if (!isfinite(soc_percent)) (void)snprintf(value, sizeof(value), "ERR");
    else {
        if (soc_percent < 0.0f) soc_display = 0U;
        else if (soc_percent > 100.0f) soc_display = 100U;
        else soc_display = (uint8_t)(soc_percent + 0.5f);
        (void)snprintf(value, sizeof(value), "%u", soc_display);
    }
    status = TJC_HMI_DrawText(190, 145, 67, 34, 1, HMI_COLOR_TEXT, HMI_COLOR_CARD, 2, value);
    if (status != HAL_OK) return status;

    (void)snprintf(footer, sizeof(footer), "LOG: %lu", (unsigned long)logged_samples);
    if (strcmp(s_last_footer, footer) != 0) {
        status = TJC_HMI_DrawText(24, 184, 150, 32, 1, HMI_COLOR_TEXT,
                                  HMI_COLOR_CARD, 0, footer);
        if (status != HAL_OK) return status;
        (void)snprintf(s_last_footer, sizeof(s_last_footer), "%s", footer);
    }
    if (strcmp(s_last_sd_label, sd_label) != 0) {
        status = TJC_HMI_DrawText(184, 184, 112, 32, 1, HMI_COLOR_TEXT,
                                  HMI_COLOR_CARD, 0, sd_label);
        if (status != HAL_OK) return status;
        (void)snprintf(s_last_sd_label, sizeof(s_last_sd_label), "%s", sd_label);
    }

    return TJC_HMI_SetStatus(run_status);
}
