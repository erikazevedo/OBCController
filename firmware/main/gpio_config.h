#pragma once
#include "driver/gpio.h"

/* ── TWAI ──────────────────────────────────────────── */
#define GPIO_TWAI0_TX        GPIO_NUM_4
#define GPIO_TWAI0_RX        GPIO_NUM_5
#define GPIO_TWAI1_TX        GPIO_NUM_6
#define GPIO_TWAI1_RX        GPIO_NUM_7

/* ── MCPWM Capture ─────────────────────────────────── */
#define GPIO_PWM_CAPTURE     GPIO_NUM_0

/* ── ADC ────────────────────────────────────────────── */
#define GPIO_ADC_INPUT       GPIO_NUM_2   /* ADC1_CHANNEL_2 */

/* ── Digital Outputs ───────────────────────────────── */
#define GPIO_CP_ENABLE       GPIO_NUM_9   /* WARNING: strapping pin — see gpio-outputs-config */
#define GPIO_PLUG_SIG        GPIO_NUM_8   /* WARNING: strapping pin — see gpio-outputs-config */
#define GPIO_STATUS_LED      GPIO_NUM_15
#define GPIO_EVA_REQ         GPIO_NUM_19
#define GPIO_CONT_POS        GPIO_NUM_22
#define GPIO_CONT_NEG        GPIO_NUM_23
