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
static int16_t s_last_soc_display = -1;
static int16_t s_last_soh_display = -1;
static int16_t s_last_bat_display = -1;

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
    s_last_soc_display = -1;
    s_last_soh_display = -1;
    s_last_bat_display = -1;

    /* Matikan touch event & respon touch pabrikan */
    (void)TJC_HMI_SendCommand("bkcmd=0");
    (void)TJC_HMI_SendCommand("tsw 255,0");
    (void)TJC_HMI_SendCommand("thup=0");
    (void)TJC_HMI_SendCommand("sendxy=0");
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

/* Menggambar simbol '%' menggunakan elemen grafis vektor primitif agar selalu muncul
   meskipun font HMI tidak menyertakan karakter ASCII '%' */
static HAL_StatusTypeDef TJC_HMI_DrawPercentSymbol(uint16_t x, uint16_t y, uint16_t color)
{
    char command[48];
    HAL_StatusTypeDef status;

    /* Lingkaran atas: kotak kecil 4x4 */
    status = TJC_HMI_FillRect(x, y, 4U, 4U, color);
    if (status != HAL_OK) return status;

    /* Garis miring persen: dari (x+1, y+13) ke (x+9, y) */
    (void)snprintf(command, sizeof(command), "line %u,%u,%u,%u,%u",
                   (unsigned)(x + 1U), (unsigned)(y + 13U),
                   (unsigned)(x + 9U), (unsigned)y, (unsigned)color);
    status = TJC_HMI_SendCommand(command);
    if (status != HAL_OK) return status;

    /* Garis tebal ganda */
    (void)snprintf(command, sizeof(command), "line %u,%u,%u,%u,%u",
                   (unsigned)(x + 2U), (unsigned)(y + 13U),
                   (unsigned)(x + 10U), (unsigned)y, (unsigned)color);
    status = TJC_HMI_SendCommand(command);
    if (status != HAL_OK) return status;

    /* Lingkaran bawah: kotak kecil 4x4 */
    return TJC_HMI_FillRect((uint16_t)(x + 7U), (uint16_t)(y + 9U), 4U, 4U, color);
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
    magnitude = value;
    if (negative) magnitude = -magnitude;
    scaled = (int32_t)(magnitude * 100.0f + 0.5f);

    if (s_last_measurement[index] == scaled && s_last_measurement_sign[index] == negative) {
        return HAL_OK;
    }
    s_last_measurement[index] = scaled;
    s_last_measurement_sign[index] = negative;

    (void)snprintf(whole, sizeof(whole), "%ld", (long)(scaled / 100));
    (void)snprintf(fraction, sizeof(fraction), "%02ld", (long)(scaled % 100));

    status = TJC_HMI_FillRect((uint16_t)(card_x + 3U), (uint16_t)(y + 2U),
                              130U, 41U, HMI_COLOR_CARD);
    if (status != HAL_OK) return status;

    if (negative) {
        status = TJC_HMI_FillRect((uint16_t)(card_x + 6U), (uint16_t)(y + 20U),
                                  8U, 3U, HMI_COLOR_TEXT);
        if (status != HAL_OK) return status;
    }

    status = TJC_HMI_DrawText((uint16_t)(card_x + 14U), y, 46U, 45U, 1U,
                              HMI_COLOR_TEXT, HMI_COLOR_CARD, 2U, whole);
    if (status != HAL_OK) return status;

    status = TJC_HMI_FillRect((uint16_t)(card_x + 62U), (uint16_t)(y + 30U),
                              4U, 4U, HMI_COLOR_TEXT);
    if (status != HAL_OK) return status;
    status = TJC_HMI_FillRect((uint16_t)(card_x + 61U), (uint16_t)(y + 34U),
                              3U, 4U, HMI_COLOR_TEXT);
    if (status != HAL_OK) return status;

    status = TJC_HMI_DrawText((uint16_t)(card_x + 68U), y, 36U, 45U, 1U,
                              HMI_COLOR_TEXT, HMI_COLOR_CARD, 0U, fraction);
    if (status != HAL_OK) return status;

    return TJC_HMI_DrawText((uint16_t)(card_x + 106U), y, 26U, 45U, 1U,
                            HMI_COLOR_TEXT, HMI_COLOR_CARD, 0U, unit);
}

/* Header Status (CALIB / START / READY / INIT): Posisi di tengah (x=115..205, y=4) */
HAL_StatusTypeDef TJC_HMI_SetStatus(const char *status_text)
{
    uint16_t text_color = HMI_COLOR_TEXT;
    HAL_StatusTypeDef status;

    if (status_text == NULL) status_text = "UNKNOWN";

    if (strcmp(status_text, "READY") == 0) text_color = HMI_COLOR_GREEN;
    else if (strcmp(status_text, "INIT") == 0) text_color = HMI_COLOR_ACCENT;
    else if (strcmp(status_text, "CALIB") == 0) text_color = HMI_COLOR_ACCENT;
    else if (strcmp(status_text, "START") == 0) text_color = HMI_COLOR_GREEN;
    else text_color = HMI_COLOR_RED;

    if (s_screen_initialized && (strcmp(s_last_status, status_text) == 0)) {
        return HAL_OK;
    }

    status = TJC_HMI_FillRect(115, 4, 90, 30, HMI_COLOR_HEADER);
    if (status != HAL_OK) return status;
    status = TJC_HMI_DrawText(115, 4, 90, 30, 1, text_color,
                              HMI_COLOR_HEADER, 1, status_text);
    if (status == HAL_OK) {
        (void)snprintf(s_last_status, sizeof(s_last_status), "%s", status_text);
    }
    return status;
}

HAL_StatusTypeDef TJC_HMI_DrawMonitor(float voltage, float current,
                                      float soc_percent, float soh_percent,
                                      uint8_t sys_bat_percent,
                                      uint32_t logged_samples,
                                      const char *sd_status,
                                      const char *run_status)
{
    char value[24];
    char sd_label[12];
    char footer[24];
    uint8_t soc_display;
    uint8_t soh_display;
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
        /* Nonaktifkan touch pabrikan agar sentuhan tidak memicu pindah halaman default */
        (void)TJC_HMI_SendCommand("tsw 255,0");
        (void)TJC_HMI_SendCommand("thup=0");
        (void)TJC_HMI_SendCommand("sendxy=0");

        status = TJC_HMI_SendCommand("cls 0");
        if (status != HAL_OK) return status;
        status = TJC_HMI_FillRect(0U, 0U, 320U, 240U, HMI_COLOR_BG);
        if (status != HAL_OK) return status;
        status = TJC_HMI_SendCommand("line 12,37,308,37,31");
        if (status != HAL_OK) return status;
        
        /* Header:
           - Kiri  (x=12..110)  : SOC LFP
           - Tengah(x=115..205) : Status (INIT / CALIB / READY)
           - Kanan (x=210..308) : BAT xx%
        */
        status = TJC_HMI_DrawText(12, 4, 100, 30, 1, HMI_COLOR_TEXT, HMI_COLOR_HEADER, 0, "SOC LFP");
        if (status != HAL_OK) return status;
        
        /* Kotak kartu: Voltage (kiri atas), Current (kanan atas), SOC/SOH Container (bawah) */
        status = TJC_HMI_DrawBox(12, 48, 144, 88, HMI_COLOR_CARD);
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawBox(164, 48, 144, 88, HMI_COLOR_CARD);
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawBox(12, 145, 296, 76, HMI_COLOR_CARD);
        if (status != HAL_OK) return status;
        
        /* Label teks di dalam kotak */
        status = TJC_HMI_DrawText(24, 52, 120, 24, 1, HMI_COLOR_TEXT, HMI_COLOR_CARD, 0, "VOLTAGE");
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText(176, 52, 120, 24, 1, HMI_COLOR_TEXT, HMI_COLOR_CARD, 0, "CURRENT");
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText(24, 150, 50, 26, 1, HMI_COLOR_TEXT, HMI_COLOR_CARD, 0, "SOC");
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText(160, 150, 50, 26, 1, HMI_COLOR_TEXT, HMI_COLOR_CARD, 0, "SOH");
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText(24, 185, 150, 28, 1, HMI_COLOR_TEXT, HMI_COLOR_CARD, 0, "LOG: 0");
        if (status != HAL_OK) return status;
        status = TJC_HMI_DrawText(184, 185, 112, 28, 1, HMI_COLOR_TEXT, HMI_COLOR_CARD, 0, sd_label);
        if (status != HAL_OK) return status;
        s_screen_initialized = 1U;
    }

    /* Update System Battery status in header: Pojok Kanan (x = 210 .. 308, y = 4) */
    if (s_last_bat_display != (int16_t)sys_bat_percent) {
        s_last_bat_display = (int16_t)sys_bat_percent;
        (void)snprintf(value, sizeof(value), "BAT %u", sys_bat_percent);
        uint16_t bat_color = (sys_bat_percent >= 20) ? HMI_COLOR_GREEN : HMI_COLOR_RED;
        (void)TJC_HMI_FillRect(210, 4, 98, 30, HMI_COLOR_HEADER);
        /* Teks BAT xx rata kanan hingga x=293 */
        (void)TJC_HMI_DrawText(210, 4, 83, 30, 1, bat_color, HMI_COLOR_HEADER, 2, value);
        /* Simbol grafis % di x=296, y=10 */
        (void)TJC_HMI_DrawPercentSymbol(296, 10, bat_color);
    }

    status = TJC_HMI_DrawMeasurement(voltage, 12U, 78U, "V");
    if (status != HAL_OK) return status;
    status = TJC_HMI_DrawMeasurement(current, 164U, 78U, "A");
    if (status != HAL_OK) return status;

    /* SOC display */
    if (!isfinite(soc_percent)) {
        if (s_last_soc_display != -2) {
            s_last_soc_display = -2;
            (void)TJC_HMI_FillRect(76, 150, 70, 28, HMI_COLOR_CARD);
            (void)TJC_HMI_DrawText(76, 150, 70, 28, 1, HMI_COLOR_RED, HMI_COLOR_CARD, 0, "ERR");
        }
    } else {
        if (soc_percent < 0.0f) soc_display = 0U;
        else if (soc_percent > 100.0f) soc_display = 100U;
        else soc_display = (uint8_t)(soc_percent + 0.5f);

        if (s_last_soc_display != (int16_t)soc_display) {
            s_last_soc_display = (int16_t)soc_display;
            (void)snprintf(value, sizeof(value), "%u", soc_display);
            (void)TJC_HMI_FillRect(76, 150, 70, 28, HMI_COLOR_CARD);
            (void)TJC_HMI_DrawText(76, 150, 40, 28, 1, HMI_COLOR_GREEN, HMI_COLOR_CARD, 0, value);
            uint16_t pct_x = (soc_display >= 100) ? 122 : ((soc_display >= 10) ? 110 : 98);
            (void)TJC_HMI_DrawPercentSymbol(pct_x, 156, HMI_COLOR_GREEN);
        }
    }

    /* SOH display */
    if (!isfinite(soh_percent)) {
        if (s_last_soh_display != -2) {
            s_last_soh_display = -2;
            (void)TJC_HMI_FillRect(215, 150, 70, 28, HMI_COLOR_CARD);
            (void)TJC_HMI_DrawText(215, 150, 70, 28, 1, HMI_COLOR_RED, HMI_COLOR_CARD, 0, "ERR");
        }
    } else {
        if (soh_percent < 0.0f) soh_display = 0U;
        else if (soh_percent > 100.0f) soh_display = 100U;
        else soh_display = (uint8_t)(soh_percent + 0.5f);

        if (s_last_soh_display != (int16_t)soh_display) {
            s_last_soh_display = (int16_t)soh_display;
            (void)snprintf(value, sizeof(value), "%u", soh_display);
            uint16_t soh_color = (soh_display >= 80) ? HMI_COLOR_GREEN : HMI_COLOR_RED;
            (void)TJC_HMI_FillRect(215, 150, 70, 28, HMI_COLOR_CARD);
            (void)TJC_HMI_DrawText(215, 150, 40, 28, 1, soh_color, HMI_COLOR_CARD, 0, value);
            uint16_t pct_x = (soh_display >= 100) ? 261 : ((soh_display >= 10) ? 249 : 237);
            (void)TJC_HMI_DrawPercentSymbol(pct_x, 156, soh_color);
        }
    }

    (void)snprintf(footer, sizeof(footer), "LOG: %lu", (unsigned long)logged_samples);
    if (strcmp(s_last_footer, footer) != 0) {
        status = TJC_HMI_DrawText(24, 185, 150, 28, 1, HMI_COLOR_TEXT,
                                  HMI_COLOR_CARD, 0, footer);
        if (status != HAL_OK) return status;
        (void)snprintf(s_last_footer, sizeof(s_last_footer), "%s", footer);
    }
    if (strcmp(s_last_sd_label, sd_label) != 0) {
        status = TJC_HMI_DrawText(184, 185, 112, 28, 1, HMI_COLOR_TEXT,
                                  HMI_COLOR_CARD, 0, sd_label);
        if (status != HAL_OK) return status;
        (void)snprintf(s_last_sd_label, sizeof(s_last_sd_label), "%s", sd_label);
    }

    return TJC_HMI_SetStatus(run_status);
}
