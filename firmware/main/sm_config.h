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
 * VCM authorisation mode.
 *   1 — bypass only (default): the car must be ON; IDLE_PLUG starts charging
 *       100 ms after the plug is seen and the VCM is never asked.
 *   0 — hybrid: power switch OFF → the bridge wakes the VCM with the EV
 *       activation pulse and acts as the stock OBC until the VCM authorises
 *       (VCM_WAKE → VCM_AUTH → AUTH_START → AUTH_CHARGING).  Power switch ON
 *       (0x50B d[2] != 0) → the bypass path above, unchanged.
 */
#ifndef SM_SKIP_VCM_AUTH
#define SM_SKIP_VCM_AUTH           1
#endif

/** Drive GPIO_PLUG_SIG (OBC pin 21, combination-meter plug indicator) while a plug is present. 0 = never drive it. */
#ifndef SM_PLUG_SIG_ENABLE
#define SM_PLUG_SIG_ENABLE         1
#endif

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

/* ── VCM authorisation path (SM_SKIP_VCM_AUTH == 0) ───────────────────────────
 * Timings follow the stock OBC in the ZE0 AC-charge capture
 * (LeafOBCBridge/docs/OBC-VCM-State-Sequence.md).                             */

/** EV system activation pulse on GPIO_EVA_REQ (ms). The FSM scope trace shows ~1 s. */
#define SM_EVA_PULSE_MS            1000

/** No 0x50B WakeUp this long after the pulse starts → wake attempt failed (ms). */
#define SM_VCM_WAKE_TIMEOUT_MS     2000

/** Bridge-initiated wakes per plug session that may end without charging. */
#define SM_VCM_WAKE_ATTEMPTS       3

/** Minimum time asleep before the bridge pulses the activation line again (ms). */
#define SM_REWAKE_DELAY_MS         3000

/** Pilot PWM must be valid (> 0 A) this long, plug in zone A, before a wake (ms). */
#define SM_PILOT_STABLE_MS         500

/** No 0x50B for this long → VCM asleep / EV-CAN silent (ms). */
#define SM_VCM_SILENT_MS           500

/** 0x603/0x68C seen this recently at a VCM wake → power-button wake, not a charge wake (ms). */
#define SM_BEACON_WINDOW_MS        1000

/** A power-button wake reaches 0x50B power ON within ~1.5 s; after this long it is treated as a charge wake (ms). */
#define SM_POWER_ON_WAIT_MS        3000

/** Announcement after the bridge appears on EV-CAN (ms): EVSE ready, then State C + S2, then pilot amps. */
#define SM_ANNOUNCE_READY_MS       50
#define SM_ANNOUNCE_S2_MS          200
#define SM_ANNOUNCE_PILOT_MS       220

/** No normal-charge or stop request this long after appearing → withdraw (ms). The VCM's own limit is 10.24 s. */
#define SM_CHARGE_REQ_TIMEOUT_MS   12000

/** After the normal-charge request: HV bus must be live within (ms). */
#define SM_HV_LIVE_TIMEOUT_MS      5000

/** After the normal-charge request: Lear must report AC (any TWAI1 frame) within (ms). */
#define SM_LEAR_AC_TIMEOUT_MS      8000

/** Lear considered alive while a TWAI1 frame arrived within (ms); the Lear broadcasts at ~1 Hz. */
#define SM_LEAR_ALIVE_MS           2500

/** Delay between HV live (0x1D4) and closing the charge contactors (ms). Stock OBC: ~0.8 s. */
#define SM_HV_SETTLE_MS            700

/** Gap between CONT_NEG and CONT_POS when closing, and POS and NEG when opening (ms). */
#define SM_CONTACTOR_STEP_MS       500

/** Step time for 0x5BF 0x48 → 0x50 → 0x58 → chargeable power re-advertised (ms). */
#define SM_AUTH_STEP_MS            300

/** Wait this long for the VCM power command (0x1F2 d[1]) before charging on the pilot limit alone (ms). */
#define SM_POWER_CMD_TIMEOUT_MS    10000

/** 1 = Lear AC limit follows the VCM maximum-charge-power command; 0 = pilot and SM_MAX_CHARGE_POWER_W only. */
#ifndef SM_FOLLOW_VCM_POWER_CMD
#define SM_FOLLOW_VCM_POWER_CMD    1
#endif

/** Stop: Lear output must fall below 1 A within (ms) before State C is released. */
#define SM_RAMP_DOWN_MS            1000

/** Stop: wait this long for the VCM to open its main relays before opening the charge contactors (ms). */
#define SM_MAIN_RELAY_WAIT_MS      8000

/** Stop: no stop request or GoToSleep from the VCM this long after the contactors open → acknowledge anyway (ms). */
#define SM_STOP_ACK_TIMEOUT_MS     30000

/** After GoToSleep keep sending 0x5BF ASLEEP (0x38) this long, then go silent (ms). */
#define SM_SLEEP_TAIL_MS           900

/** Nominal EVSE AC voltage (V): reported in 0x380 and used to convert the VCM kW command to Lear AC amps. */
#ifndef SM_EVSE_AC_NOMINAL_V
#define SM_EVSE_AC_NOMINAL_V       120
#endif

/** 0x380 d[0] AC-input-type bit: 0x08 = 100 V class (inferred), 0x10 = 200 V class (seen in the capture). */
#ifndef SM_AC_TYPE_MASK
#define SM_AC_TYPE_MASK            ((SM_EVSE_AC_NOMINAL_V) < 180 ? 0x08u : 0x10u)
#endif

/** 1 = log VCM / LBC / Lear signal changes (0x50B, 0x1F2 incl. VCM MODE, 0x1D4, 0x1DA, Lear) at INFO level. */
#ifndef SM_LOG_VCM_TRACE
#define SM_LOG_VCM_TRACE           1
#endif
