#include "ekf_soc.h"
#include <math.h>
#include <string.h>

#define P2(hekf,i,j) ((hekf)->P[(i)*2+(j)])

static float EKF_SOC_EvalPoly(const float *coeffs, int order, float xv)
{
    float r = coeffs[0];
    int i;
    for (i = 1; i <= order; i++)
        r = r * xv + coeffs[i];
    return r;
}

static void EKF_SOC_ComputeRhoMu(const EKF_SOC_HandleTypeDef *hekf, float I, float *rho, float *mu)
{
    *rho = fabsf(I) / hekf->Qn_for_rho;
    *mu  = (hekf->rho_hi - *rho) / (hekf->rho_hi - hekf->rho_lo);
    if (*mu > 1.0f) *mu = 1.0f;
    if (*mu < 0.0f) *mu = 0.0f;
}

static void EKF_SOC_CorrectState(EKF_SOC_HandleTypeDef *hekf, const float K[2], float innovation)
{
    hekf->x_hat[0] += K[0] * innovation;
    hekf->x_hat[1] += K[1] * innovation;
}

static void EKF_SOC_CorrectCov(EKF_SOC_HandleTypeDef *hekf, const float K[2], const float H[2])
{
    float IKH[2][2];
    float tmp[2][2];
    float Pnew[4];
    int i, j, k;

    IKH[0][0] = 1.0f - K[0]*H[0];
    IKH[0][1] =      - K[0]*H[1];
    IKH[1][0] =      - K[1]*H[0];
    IKH[1][1] = 1.0f - K[1]*H[1];

    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++) {
            float s = 0.0f;
            for (k = 0; k < 2; k++)
                s += IKH[i][k] * P2(hekf, k, j);
            tmp[i][j] = s;
        }

    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++) {
            float s = 0.0f;
            for (k = 0; k < 2; k++)
                s += tmp[i][k] * IKH[j][k];
            Pnew[i*2+j] = s;
        }

    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++)
            Pnew[i*2+j] += K[i] * hekf->R * K[j];

    for (k = 0; k < 4; k++)
        hekf->P[k] = Pnew[k];
}

static void EKF_SOC_PredictState(EKF_SOC_HandleTypeDef *hekf, float I, float a1, float Ak11, float Bk1, float Bk2)
{
    float R1k = -Bk1 / Ak11;

    float Vc1_next = a1 * hekf->x_hat[0] + R1k * (1.0f - a1) * I;
    float SOC_next = hekf->x_hat[1] + Bk2 * I;

    hekf->x_hat[0] = Vc1_next;
    hekf->x_hat[1] = SOC_next;
}

static void EKF_SOC_PredictCov(EKF_SOC_HandleTypeDef *hekf, float a1)
{
    float Fk[2][2] = { { a1, 0.0f }, { 0.0f, 1.0f } };
    float tmp[2][2];
    float Pnew[4];
    int i, j, k;

    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++) {
            float s = 0.0f;
            for (k = 0; k < 2; k++)
                s += Fk[i][k] * P2(hekf, k, j);
            tmp[i][j] = s;
        }

    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++) {
            float s = 0.0f;
            for (k = 0; k < 2; k++)
                s += tmp[i][k] * Fk[j][k];
            Pnew[i*2+j] = s + hekf->Q[i*2+j];
        }

    for (k = 0; k < 4; k++)
        hekf->P[k] = Pnew[k];
}

void EKF_SOC_Init(EKF_SOC_HandleTypeDef *hekf, const float x0[2])
{
    hekf->x_hat[0] = x0[0];
    hekf->x_hat[1] = x0[1];

    hekf->y_hat_dbg = 0.0f;
    hekf->innov_dbg = 0.0f;
    hekf->rho_dbg   = 0.0f;
    hekf->mu_dbg    = 0.0f;
}

void EKF_SOC_Reset(EKF_SOC_HandleTypeDef *hekf, float soc0, const float P0[4])
{
    hekf->x_hat[0] = 0.0f;
    hekf->x_hat[1] = soc0;

    hekf->P[0] = P0[0]; hekf->P[1] = P0[1];
    hekf->P[2] = P0[2]; hekf->P[3] = P0[3];
}

void EKF_SOC_Update(EKF_SOC_HandleTypeDef *hekf, float I, float V, float Ts)
{
    float rho, mu;
    float Ak11, Bk1, Bk2, Rsk;
    float socClip, OCVk, y_hat;
    float slopeLocal, H[2];
    float innovation, S_innov, Sinv;
    float PHt0, PHt1, K[2];
    float a1;

    EKF_SOC_ComputeRhoMu(hekf, I, &rho, &mu);
    hekf->rho_dbg = rho;
    hekf->mu_dbg  = mu;

    Ak11 = mu * hekf->A_lo[0] + (1.0f - mu) * hekf->A_hi[0];
    Bk1  = mu * hekf->B_lo[0] + (1.0f - mu) * hekf->B_hi[0];
    Bk2  = mu * hekf->B_lo[1] + (1.0f - mu) * hekf->B_hi[1];
    Rsk  = mu * hekf->Rs_lo   + (1.0f - mu) * hekf->Rs_hi;

    socClip = hekf->x_hat[1];
    if (socClip > 1.0f) socClip = 1.0f;
    if (socClip < 0.0f) socClip = 0.0f;

    OCVk  = EKF_SOC_EvalPoly(hekf->ocvCoeffs, hekf->ocvOrder, socClip);
    y_hat = OCVk + hekf->C0[0]*hekf->x_hat[0] + hekf->C0[1]*hekf->x_hat[1] - Rsk*I;
    hekf->y_hat_dbg = y_hat;

    slopeLocal = EKF_SOC_EvalPoly(hekf->dOcvCoeffs, hekf->dOcvOrder, socClip);
    H[0] = hekf->C0[0];
    H[1] = slopeLocal + hekf->C0[1];

    innovation = V - y_hat;
    hekf->innov_dbg = innovation;

    S_innov = H[0]*H[0]*P2(hekf,0,0) + H[0]*H[1]*P2(hekf,0,1)
            + H[1]*H[0]*P2(hekf,1,0) + H[1]*H[1]*P2(hekf,1,1) + hekf->R;
    Sinv = (fabsf(S_innov) < 1e-30f) ? 0.0f : (1.0f / S_innov);

    PHt0 = P2(hekf,0,0)*H[0] + P2(hekf,0,1)*H[1];
    PHt1 = P2(hekf,1,0)*H[0] + P2(hekf,1,1)*H[1];
    K[0] = PHt0 * Sinv;
    K[1] = PHt1 * Sinv;

    EKF_SOC_CorrectState(hekf, K, innovation);
    EKF_SOC_CorrectCov(hekf, K, H);

    a1 = expf(Ts * Ak11);
    EKF_SOC_PredictState(hekf, I, a1, Ak11, Bk1, Bk2 * Ts);
    EKF_SOC_PredictCov(hekf, a1);
}

float EKF_SOC_GetVc1  (const EKF_SOC_HandleTypeDef *hekf) { return hekf->x_hat[0]; }
float EKF_SOC_GetSoC  (const EKF_SOC_HandleTypeDef *hekf) { return hekf->x_hat[1]; }
float EKF_SOC_GetYhat (const EKF_SOC_HandleTypeDef *hekf) { return hekf->y_hat_dbg; }
float EKF_SOC_GetInnov(const EKF_SOC_HandleTypeDef *hekf) { return hekf->innov_dbg; }
float EKF_SOC_GetRho  (const EKF_SOC_HandleTypeDef *hekf) { return hekf->rho_dbg; }
float EKF_SOC_GetMu   (const EKF_SOC_HandleTypeDef *hekf) { return hekf->mu_dbg; }
float EKF_SOC_GetP    (const EKF_SOC_HandleTypeDef *hekf, int i, int j) { return hekf->P[i*2+j]; }