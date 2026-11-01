#pragma once

/* ── State Machine Tuning Parameters ─────────────────────────────────────────
 * Change macros here to adjust behaviour; do not hardcode values in
 * state_machine.c.                                                            */

/** Startup default for target SOC at which a charging session is terminated (%). Adjustable at runtime via CAN 0x6C0. */
#define SM_SOC_CUTOFF_PCT          90.0f

/** SOC hysteresis for restarting a charging session (%). */
#define SM_SOC_RESTART_HYST       2.0f

/** Maximum OBC charge power advertised to VCM via 0x380 (W). */
#define SM_MAX_CHARGE_POWER_W      3300

/** Drive GPIO_PLUG_SIG (OBC pin 21, combination-meter plug indicator) while a plug is present. 0 = never drive it. */
#ifndef SM_PLUG_SIG_ENABLE
#define SM_PLUG_SIG_ENABLE         1
#endif

/** ADC voltage above which no plug is detected (Zone C) (mV). */
#define SM_ADC_NO_PLUG_MV          2300

/** ADC voltage at or above which button/unlock press is signalled (mV). */
#define SM_ADC_BUTTON_MV           700

/** Startup default for Lear OBC DC target voltage (V). Adjustable at runtime via CAN 0x6C0. */
#define SM_LEAR_TARGET_VOLTAGE_V   400

/** Accepted range for target voltage received on 0x6C0 (V). */
#define SM_TARGET_VOLTAGE_MIN_V    300
#define SM_TARGET_VOLTAGE_MAX_V    400

/** Accepted range for target SOC received on 0x6C0 (%). */
#define SM_TARGET_SOC_MIN_PCT      50
#define SM_TARGET_SOC_MAX_PCT      100

/* ── VCM-authorised charging ──────────────────────────────────────────────────
 * The only charging path: with the power switch OFF the bridge wakes the VCM
 * and acts as the stock OBC.  Timings follow the stock OBC in the ZE0
 * AC-charge capture (LeafOBCBridge/docs/OBC-VCM-State-Sequence.md).          */

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

/** After the normal-charge request: AC must be confirmed at the Lear within (ms). */
#define SM_LEAR_AC_TIMEOUT_MS      20000

/** Lear considered alive while a TWAI1 frame arrived within (ms); the Lear broadcasts at ~1 Hz. */
#define SM_LEAR_ALIVE_MS           2500

/** The Lear broadcasts whenever it is powered, also without AC: in a charge wake from ≈ 1.2–2.2 s after State C,
 *  and for ≈ 6–25 s after the VCM sleeps.  A Lear still broadcasting when the VCM wakes again drops off TWAI1 for
 *  ≈ 3.5–4 s, so if it is broadcasting when State C is asserted only an AC voltage reading (0x27D) received this
 *  long after State C counts (ms). */
#define SM_LEAR_AC_SETTLE_MS       2500

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

/** A VCM stop during AUTH_START, or this soon after AUTH_CHARGING starts, is logged as a warning (ms):
 *  the car declined the charge.  With a charge timer set the VCM stops ≈ 2 s after its first power command. */
#define SM_VCM_EARLY_STOP_MS       10000

/** Stop: Lear output must fall below 1 A within (ms) before State C is released. */
#define SM_RAMP_DOWN_MS            1000

/** Stop: wait this long for the VCM to open its main relays before opening the charge contactors (ms). */
#define SM_MAIN_RELAY_WAIT_MS      8000

/** Stop: no stop request or GoToSleep from the VCM this long after the contactors open → acknowledge anyway (ms). */
#define SM_STOP_ACK_TIMEOUT_MS     30000

/** After GoToSleep keep sending 0x5BF ASLEEP (0x38) this long, timed from the first GoToSleep, then go silent (ms).
 *  The stock OBC's last frame is 120 ms after GoToSleep; the VCM transmits for another ~900 ms, so every frame is ACKed. */
#define SM_SLEEP_TAIL_MS           150

/** VCM-authorised states stop transmitting when no 0x50B/0x1F2/0x1D4 arrived for this long (ms).
 *  TWAI0 retries an unacknowledged frame forever, so the bridge must never transmit to an empty bus. */
#define SM_TX_VCM_FRESH_MS         100

/** AC counts as present at the Lear only while its AC input voltage (0x27D) has read at least this within
 *  SM_LEAR_ALIVE_MS (V).  That reading is reported in 0x380 and converts the VCM kW command to Lear AC amps.
 *  Without AC it reads 0, with blips of up to 13.5 V when the charge contactors switch; a 120 V supply reads
 *  113–125 V. */
#define SM_AC_PRESENT_MIN_V        90

/** 0x380 d[0] AC-input-type bit from the measured AC voltage: 0x10 = 200 V class (seen in the capture) at or
 *  above this, else 0x08 = 100 V class (inferred).  Midway between 127 V + 10 % and 200 V − 10 % (V). */
#define SM_AC_200V_CLASS_MIN_V     160

/** 1 = log VCM / LBC / Lear signal changes (0x50B, 0x1F2 incl. VCM MODE, 0x1D4, 0x1DA, Lear) at INFO level. */
#ifndef SM_LOG_VCM_TRACE
#define SM_LOG_VCM_TRACE           1
#endif

/** 1 = capture every Lear (TWAI1) frame, all IDs, and log each payload change at INFO level with the bits that
 *  changed.  For finding undocumented signals such as the AC input voltage: compare a 120 V and a 240 V session.
 *  Extra IDs are only logged, they never reach the state machine. */
#ifndef SM_LOG_LEAR_RAW
#define SM_LOG_LEAR_RAW            0
#endif

/** Minimum time between two raw-capture lines for the same Lear ID (ms); changes in between are ORed into the next line. */
#define SM_LOG_LEAR_RAW_MIN_MS     1000
