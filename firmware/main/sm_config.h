#pragma once

/* ── State Machine Tuning Parameters ─────────────────────────────────────────
 * Change macros here to adjust behaviour; do not hardcode values in
 * state_machine.c.                                                            */

/** Startup default for target SOC at which a charging session is terminated (%). Adjustable at runtime via CAN 0x6C0. */
#define SM_SOC_CUTOFF_PCT          80.0f

/** SOC hysteresis for restarting a charging session (%). */
#define SM_SOC_RESTART_HYST       2.0f

/** Maximum OBC charge power advertised to VCM via 0x380 (W). */
#define SM_MAX_CHARGE_POWER_W      3300

/**
 * When non-zero: skip VCM_AUTH state entirely.
 * IDLE_PLUG transitions directly to EVSE_ENABLE on the VCM charge command.
 * Set to 0 to re-enable: IDLE_PLUG → VCM_AUTH → EVSE_ENABLE.
 */
#define SM_SKIP_VCM_AUTH           1

/** ADC voltage above which no plug is detected (Zone C) (mV). */
#define SM_ADC_NO_PLUG_MV          2300

/** ADC voltage at or above which button/unlock press is signalled (mV). */
#define SM_ADC_BUTTON_MV           700

/** Startup default for Lear OBC DC target voltage (V). Adjustable at runtime via CAN 0x6C0. */
#define SM_LEAR_TARGET_VOLTAGE_V   392

/** Accepted range for target voltage received on 0x6C0 (V). */
#define SM_TARGET_VOLTAGE_MIN_V    300
#define SM_TARGET_VOLTAGE_MAX_V    400

/** Accepted range for target SOC received on 0x6C0 (%). */
#define SM_TARGET_SOC_MIN_PCT      50
#define SM_TARGET_SOC_MAX_PCT      100
