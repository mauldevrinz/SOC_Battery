#ifndef EKF_SOC_H
#define EKF_SOC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define EKF_SOC_OCV_MAX_ORDER 8

typedef struct
{
    float Qn_for_rho;

    float rho_lo;
    float rho_hi;

    float A_lo[4];
    float A_hi[4];
    float B_lo[2];
    float B_hi[2];

    float Rs_lo;
    float Rs_hi;

    float C0[2];

    float ocvCoeffs[EKF_SOC_OCV_MAX_ORDER + 1];
    int   ocvOrder;
    float dOcvCoeffs[EKF_SOC_OCV_MAX_ORDER];
    int   dOcvOrder;

    float Q[4];
    float R;

    float x_hat[2];
    float P[4];

    float y_hat_dbg;
    float innov_dbg;
    float rho_dbg;
    float mu_dbg;

} EKF_SOC_HandleTypeDef;

void EKF_SOC_Init(EKF_SOC_HandleTypeDef *hekf, const float x0[2]);
void EKF_SOC_Reset(EKF_SOC_HandleTypeDef *hekf, float soc0, const float P0[4]);
void EKF_SOC_Update(EKF_SOC_HandleTypeDef *hekf, float I, float V, float Ts);

float EKF_SOC_GetVc1  (const EKF_SOC_HandleTypeDef *hekf);
float EKF_SOC_GetSoC  (const EKF_SOC_HandleTypeDef *hekf);
float EKF_SOC_GetYhat (const EKF_SOC_HandleTypeDef *hekf);
float EKF_SOC_GetInnov(const EKF_SOC_HandleTypeDef *hekf);
float EKF_SOC_GetRho  (const EKF_SOC_HandleTypeDef *hekf);
float EKF_SOC_GetMu   (const EKF_SOC_HandleTypeDef *hekf);
float EKF_SOC_GetP    (const EKF_SOC_HandleTypeDef *hekf, int i, int j);

#ifdef __cplusplus
}
#endif

#endif