#pragma once

/* ── State Machine Tuning Parameters ─────────────────────────────────────────
 * Change macros here to adjust behaviour; do not hardcode values in
 * state_machine.c.                                                            */

/** SOC level at which a charging session is terminated (%). */
#define SM_SOC_CUTOFF_PCT          80.0f

/** Maximum OBC charge power advertised to VCM via 0x380 (W). */
#define SM_MAX_CHARGE_POWER_W      3300

/**
 * DEPRECATED — do not use in new logic.
 * VCM_AUTH routing is now path-dependent via the g_arrived_from_sleep context
 * flag, not this compile-time switch:
 *   - Sleep path (SLEEP → WAKE → IDLE_PLUG): always uses VCM_AUTH.
 *   - Awake path (IDLE_NO_PLUG → IDLE_PLUG): always skips VCM_AUTH.
 * Retained here only to avoid breaking any legacy references during transition.
 */
#define SM_SKIP_VCM_AUTH           1   /* DEPRECATED */

/** ADC voltage above which no plug is detected (Zone C) (mV). */
#define SM_ADC_NO_PLUG_MV          2300

/** ADC voltage at or above which button/unlock press is signalled (mV). */
#define SM_ADC_BUTTON_MV           700

/** Lear OBC DC target voltage setpoint (V). */
#define SM_LEAR_TARGET_VOLTAGE_V   392
