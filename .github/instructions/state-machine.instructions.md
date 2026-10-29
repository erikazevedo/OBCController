---
applyTo: "**/state_machine*"
---
# OBCController State Machine Specification

## Global Configuration

All tunable parameters MUST be defined as macros in a dedicated config header (e.g. `sm_config.h`) so they can be changed without touching logic code.

| Parameter | Default | Macro |
|---|---|---|
| SOC charge cutoff | 80.0 % | `SM_SOC_CUTOFF_PCT 80.0f` — startup default for `g_sm_target_soc_pct`; overwritten at runtime by CAN 0x6C0 (range `SM_TARGET_SOC_MIN_PCT`–`SM_TARGET_SOC_MAX_PCT`) |
| Max OBC charge power | 3300 W | `SM_MAX_CHARGE_POWER_W 3300` |
| VCM authorisation mode | bypass only | `SM_SKIP_VCM_AUTH 1` (`#ifndef`-guarded). `1` = bypass only: the car must be ON and the VCM is never asked. `0` = hybrid: power switch OFF → VCM-authorised path ([below](#vcm-authorised-path-sm_skip_vcm_auth--0)); power switch ON → the bypass path, unchanged |
| Plug-in indicator | enabled | `SM_PLUG_SIG_ENABLE 1` — drive `GPIO_PLUG_SIG` (OBC pin 21 → combination meter plug icon) while a plug is present (zone A or B), in both builds |
| ADC no-plug threshold | 2300 mV | `SM_ADC_NO_PLUG_MV 2300` — above this = no plug (Zone C) |
| ADC button/cancel threshold | 700 mV | `SM_ADC_BUTTON_MV 700` — at or above this = button pressed or plug removed; cancel charge trigger |
| Lear OBC target voltage | 392 V | `SM_LEAR_TARGET_VOLTAGE_V 392` — startup default for `g_sm_target_voltage_v`; overwritten at runtime by CAN 0x6C0 (range `SM_TARGET_VOLTAGE_MIN_V`–`SM_TARGET_VOLTAGE_MAX_V`); re-encoded into 0x050 immediately if changed while CHARGING |
| J1772 AC current limit | derived | Computed each SM cycle from MCPWM pilot duty cycle — see J1772 Current Derivation section |
| VCM-authorised path timing | see `sm_config.h` | `SM_EVA_PULSE_MS`, `SM_VCM_WAKE_TIMEOUT_MS`, `SM_VCM_WAKE_ATTEMPTS`, `SM_REWAKE_DELAY_MS`, `SM_PILOT_STABLE_MS`, `SM_VCM_SILENT_MS`, `SM_BEACON_WINDOW_MS`, `SM_POWER_ON_WAIT_MS`, `SM_ANNOUNCE_*_MS`, `SM_CHARGE_REQ_TIMEOUT_MS`, `SM_HV_LIVE_TIMEOUT_MS`, `SM_LEAR_AC_TIMEOUT_MS`, `SM_LEAR_ALIVE_MS`, `SM_HV_SETTLE_MS`, `SM_CONTACTOR_STEP_MS`, `SM_AUTH_STEP_MS`, `SM_POWER_CMD_TIMEOUT_MS`, `SM_RAMP_DOWN_MS`, `SM_MAIN_RELAY_WAIT_MS`, `SM_STOP_ACK_TIMEOUT_MS`, `SM_SLEEP_TAIL_MS`, `SM_TX_VCM_FRESH_MS` |
| Follow the VCM power command | enabled | `SM_FOLLOW_VCM_POWER_CMD 1` — Lear AC limit follows `0x1F2` d[1]; `0` = pilot and `SM_MAX_CHARGE_POWER_W` only |
| EVSE AC voltage | 120 V | `SM_EVSE_AC_NOMINAL_V 120` — reported in `0x380` and used to convert watts to Lear AC amps; `SM_AC_TYPE_MASK` follows it (0x08 = 100 V class, 0x10 = 200 V class) |
| Signal trace | enabled | `SM_LOG_VCM_TRACE 1` — rate-limited INFO log of `0x50B`, `0x1F2` (with the VCM MODE name), `0x1D4`, `0x1DA` and Lear changes |

---

## Plug Detection (ADC — Evaluated Every State Machine Cycle)

**Signal source:** `GPIO_ADC_INPUT` (GPIO2, ADC1_CH2), calibrated via `adc_cali_raw_to_voltage()` in the state machine task.  
**Update rate:** 250 Hz hardware (ISR writes `g_adc_raw_avg`); state machine reads and calibrates every 10 ms.

The ADC input reads a resistor-divider that encodes three operating zones:

| Zone | Voltage range | Meaning |
|---|---|---|
| A | < 700 mV (`SM_ADC_BUTTON_MV`) | Plug connected, no user action |
| B | 700 – 2300 mV | Plug connected, unlock button pressed — **cancel charge** |
| C | > 2300 mV (`SM_ADC_NO_PLUG_MV`) | No plug |

**Cancel charge condition (Zone B or C):** `adc_mv >= SM_ADC_BUTTON_MV`  
From any charging-active state (EVSE_ENABLE, CHARGING), this preempts all other logic and forces immediate entry into CHARGE_DISABLE. It catches both physical plug removal (Zone C) and the handle unlock button press (Zone B — plug body still present).

**Plug present:** `adc_mv <= SM_ADC_NO_PLUG_MV` (Zone A or B)  
**No plug:** `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C)

---

## States

The bypass states (SLEEP … CHARGE_DONE) are used by both builds. The VCM-authorised path (`SM_SKIP_VCM_AUTH == 0`, power switch OFF) has its own states, specified in [VCM-authorised path](#vcm-authorised-path-sm_skip_vcm_auth--0).

> **0x50B in this firmware:** the run/sleep events follow **d[2]**, which is non-zero only while the **power switch is ON** (`0x02` → `0x06`). A charge wake keeps d[2] = `0x00` for the whole session; there only d[3][7:6] (`11` WakeUp / `00` GoToSleep) shows the VCM is awake. The bypass states therefore mean "car ON".

### SLEEP

- **Description**: Power switch OFF (`0x50B d[2] == 0x00`) or EV-CAN silent. Bypass build: fully quiescent. Auth build: also the starting point of the VCM-authorised path.
- **Entry actions**:
  - `g_twai0_tx_enabled = false` *(bypass build: written by the RX dispatcher from 0x50B d[2]; auth build: by the state machine at the end of each cycle)*
  - Confirm `vTaskSuspend(twai1_tx_task_hdl)` (TWAI1 TX task must not run)
  - `GPIO_CP_ENABLE = LOW`
  - `GPIO_CONT_POS = LOW`
  - `GPIO_CONT_NEG = LOW`
  - `GPIO_STATUS_LED = LOW`
- **Exit actions**: none.
- **TWAI0 TX obligations**: **None.** `g_twai0_tx_enabled = false`; gptimer ISR gate suppresses all TX.
- **TWAI0 RX triggers**:
  - `0x50B d[2] != 0x00` (power switch ON) → IDLE_NO_PLUG
  - Auth build only: see the SLEEP row of the [VCM-authorised path](#vcm-authorised-path-sm_skip_vcm_auth--0).
- **TWAI1 actions**: None.
- **Timeout**: None.

---

### IDLE_NO_PLUG

- **Description**: VCM in RUN mode; no charge plug detected on ADC input. TWAI0 TX active, TWAI1 silent.
- **Entry actions**:
  - `GPIO_CP_ENABLE = LOW`
  - `GPIO_CONT_POS = LOW`
  - `GPIO_CONT_NEG = LOW`
  - `GPIO_STATUS_LED = LOW`
  - Set `frame_5bf.d[4] = 0x28` (IDLE)
  - Set `frame_380.d[4][6] = 0` (AC relay bit cleared)
  - `vTaskSuspend(twai1_tx_task_hdl)` (if not already suspended)
- **Exit actions**: none.
- **TWAI0 TX obligations**:
  - `0x380` at 100 ms: power field = `SM_MAX_CHARGE_POWER_W / 100`; AC relay bit = 0.
  - `0x5BF` at 100 ms: status byte = `0x28` (IDLE).
- **TWAI0 RX triggers**:
  - `0x50B d[2] == 0x00` (power switch OFF) → SLEEP
- **ADC trigger**:
  - `adc_mv <= SM_ADC_NO_PLUG_MV` (Zone A or B — plug present) → IDLE_PLUG
- **TWAI1 actions**: None.
- **Timeout**: None.

---

### IDLE_PLUG

- **Description**: Bypass path (car ON). Plug physically inserted (ADC threshold crossed); the controller repeats the OBC start-up states on `0x5BF` and starts charging after 100 ms without asking the VCM.
- **Entry actions**:
  - Set `frame_5bf.d[4] = 0x10` (first OBC start-up state)
  - After exactly **100 ms** (1 CAN frame period), set `frame_5bf.d[4] = 0x18` (second start-up state). The stock OBC sends `0x10` for one frame and then `0x18` at **every** start, with or without a plug — it is not a plug or beep signal.
  - `GPIO_STATUS_LED = HIGH`
  - Reset `g_session_ac_current_A = 0.0f` (clears previous session value)
- **Per-cycle actions (each 10 ms SM cycle while in IDLE_PLUG)**:
  - If `g_pwm_signal_valid == true`: compute J1772 current from `g_pwm_duty_us`/`g_pwm_period_us` using the derivation formula below; store locally as `i_max_A`.
  - If `g_pwm_signal_valid == false`: hold `i_max_A = 0.0f`.
  - Write `frame_5bf.d[2] = (uint8_t)(i_max_A * 5.0f)` — broadcasts J1772 available current to VCM via `0x5BF d[2]` (`amps × 5` encoding, per frame spec).
  > **Hardware constraint:** J1772 PWM MUST be sampled here, before `GPIO_CP_ENABLE` is asserted. After `CP_ENABLE` goes HIGH the CP circuit shifts to a lower signal voltage and MCPWM edge detection becomes unreliable. The value derived here is frozen at transition time and used as the fixed current limit for the entire charging session.
- **Exit actions**:
  - `GPIO_STATUS_LED = LOW` (restored to LOW before entering downstream states that set it themselves)
- **TWAI0 TX obligations**:
  - `0x380` at 100 ms: AC relay bit = 0.
  - `0x5BF` at 100 ms: status byte sequence `0x10` (1 frame = 100 ms) → `0x18` (held until state exit).
- **TWAI0 RX triggers**:
  - `0x50B d[2] == 0x00` (power switch OFF) → SLEEP *(plug remains but the car was switched off)*
- **Advance trigger**:
  - `0x5BF` advanced to `0x18` (100 ms) AND Zone A → lock `g_session_ac_current_A = i_max_A`, then → EVSE_ENABLE. Zone B (button held) keeps the state and restarts the 100 ms advance.
  > **J1772 current lock:** `g_session_ac_current_A` is frozen here — the last `i_max_A` derived in IDLE_PLUG — while `GPIO_CP_ENABLE` is still LOW and the pilot signal is still valid. This is the **only** point per session where `g_session_ac_current_A` is written.
- **ADC trigger**:
  - `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C — plug removed) → IDLE_NO_PLUG
- **TWAI1 actions**: None.
- **Timeout**: None.

> **The VCM never authorises charging with the power switch ON** (FSM EVC-35/40), so this path ignores `0x1F2`. It also defeats the VCM's READY interlock: the VCM is never told that a connector is attached, so the car can be driven off while plugged in.

---

### EVSE_ENABLE

- **Description**: VCM authorised (or auth bypassed). Controller asserts `GPIO_CP_ENABLE` HIGH to command EVSE relay closure and deliver AC to OBC input; simultaneously closes HV DC charging contactors. Waits for OBC to begin transmitting on TWAI1 before advancing to CHARGING.

> **These are HV DC charging contactors** (`CONT_POS` / `CONT_NEG`), isolated from the main battery contactors. They are in series with the main battery contactor. They are NOT the EVSE relay; the EVSE relay is controlled by `GPIO_CP_ENABLE`.

- **Entry actions**:
  - Set `frame_5bf.d[4] = 0x40` (CHARGE_INIT)
  - Set `frame_380.d[4][6] = 1` (AC relay bit — signals OBC relay closed to VCM)
  - `GPIO_CP_ENABLE = HIGH` — commands EVSE to close its relay; AC power begins flowing to OBC
  - Close HV charge contactors (sequencing — see Contactor Sequencing section below)
- **Exit actions**: none; GPIO states carry over into CHARGING.
- **TWAI0 TX obligations**:
  - `0x380` at 100 ms: AC relay bit = 1; power = `SM_MAX_CHARGE_POWER_W / 100`.
  - `0x5BF` at 100 ms: status = `0x40` (CHARGE_INIT).
- **TWAI0 RX triggers** (preemptive — abort before CHARGING):
  - `0x50B d[2] == 0x00` (power switch OFF) → charge-disable sequence, then SLEEP
  - `0x1DB d[1][2:0] != 0b000` (LBC failsafe active) → CHARGE_DISABLE
- **ADC trigger**:
  - `adc_mv >= SM_ADC_BUTTON_MV` (Zone B: button pressed, or Zone C: plug removed) → CHARGE_DISABLE
- **TWAI1 RX trigger**:
  - Any frame received on TWAI1 (ID 0x616, 0x617, 0x618, or 0x274) → CHARGING *(OBC is alive and responding)*
- **TWAI1 actions**: TWAI1 TX task remains suspended in this state.
- **Timeout**: **OPEN** — see Open Questions #5.

#### Contactor Sequencing (EVSE_ENABLE entry)

Executed by the state machine task using `vTaskDelay()`. Blocking for 500 ms is acceptable here; TWAI0 TX continues from the gptimer ISR unaffected.

1. `GPIO_CONT_NEG = HIGH` — close negative HV contactor first
2. `vTaskDelay(pdMS_TO_TICKS(500))`
3. `GPIO_CONT_POS = HIGH` — close positive HV contactor

> **HV safety note:** `CONT_NEG` and `CONT_POS` are the HV DC charging contactors, in series with the main battery contactor. They are NOT the EVSE AC relay (controlled by `GPIO_CP_ENABLE`). Negative closes first to establish the return path before the positive makes contact.

---

### CHARGING

- **Description**: L2 AC charging active. TWAI1 0x050 frames sent with `Lear_ChargeEnable = 1`. Charging continues until SOC reaches cutoff or another exit condition fires.
- **Entry actions**:
  - Set `frame_5bf.d[4] = 0x60` (CHARGING)
  - Set `frame_5bf.d[2] = (uint8_t)(g_session_ac_current_A * 5.0f)` — locks 0x5BF J1772 current field to session value (`amps × 5` encoding); unchanged for duration of session
  - Configure `frame_050`:
    - `Lear_ChargeEnable = 1` (byte 7 bit 0 = 1)
    - `Lear_TargetVoltage = g_sm_target_voltage_v` (default 392 V → encoded as `392 × 10 = 3920` big-endian 16-bit in bytes 2–3)
    - `Lear_ACCurrentLimit` = `g_session_ac_current_A` (captured in IDLE_PLUG before `GPIO_CP_ENABLE` asserted — fixed for session; see J1772 Current Derivation section; **NOT re-derived in this state**)
  - `vTaskResume(twai1_tx_task_hdl)` — TWAI1 TX begins immediately at 100 ms cadence *(call **after** `frame_050` is fully written)*
  - `GPIO_STATUS_LED = HIGH`
- **Exit actions**: handled in CHARGE_DISABLE.
- **TWAI0 TX obligations**:
  - `0x380` at 100 ms: AC relay bit = 1; power = `SM_MAX_CHARGE_POWER_W / 100`.
  - `0x5BF` at 100 ms: `d[4]` = `0x60` (CHARGING); `d[2]` = `(uint8_t)(g_session_ac_current_A * 5.0f)` (J1772 available current, `amps × 5` — set on CHARGING entry, held constant for session).
- **TWAI0 RX triggers**:
  - `0x55B` decoded SOC ≥ `g_sm_target_soc_pct` → set `charge_done_normal = true`, then → CHARGE_DISABLE *(normal completion — flag routes to CHARGE_DONE after shutdown sequence)*
  - `0x50B d[2] == 0x00` (power switch OFF) → charge-disable sequence, then SLEEP
  - `0x1DB d[1][2:0] != 0b000` (LBC failsafe) → CHARGE_DISABLE
  - `0x1D4 d[6] == 0x8C` (DBC "charging interrupted") → CHARGE_DISABLE *(never observed in 16 ZE0 captures; harmless)*
- **ADC trigger**:
  - `adc_mv >= SM_ADC_BUTTON_MV` (Zone B: button pressed, or Zone C: plug removed) → CHARGE_DISABLE *(immediate preemption)*
- **TWAI1 obligations**:
  - `0x050` at 100 ms (via TWAI1 TX task): `Lear_ChargeEnable = 1`; `Lear_TargetVoltage = g_sm_target_voltage_v`; `Lear_ACCurrentLimit` = `g_session_ac_current_A` (fixed session value — PWM edge detection unreliable after `GPIO_CP_ENABLE` assertion; **do not re-derive here**).
  - Monitor `0x617` (Lear_DCData) for DC voltage and current telemetry (informational; no control action currently defined).
  - Monitor `0x274` (Lear_Temperature) for OBC temperature (informational; fault action TBD).
- **Timeout**: None — charging runs until an exit trigger fires.

---

### CHARGE_DISABLE

- **Description**: Ordered charge shutdown sequence. Executes once, then transitions to IDLE_PLUG or IDLE_NO_PLUG based on current plug state.

**This is a transitional state, not a steady state.** The state machine task executes steps 1–3 immediately, then performs the contactor open sequence (step 4) with a 500 ms inter-step blocking delay via `vTaskDelay()`. Total minimum duration: ~500 ms. TWAI0 TX continues from the gptimer ISR throughout.

- **Entry actions (sequential)**:
  1. `twai1_frame_050_set_enable(false)` (`Lear_ChargeEnable = 0`) — clears the shared frame and any 0x050 still queued in the driver, before the TWAI1 task next fires.
  2. `vTaskSuspend(twai1_tx_task_hdl)` — stops periodic 0x050. *(The frame with `ChargeEnable = 0` may or may not have been sent; that is acceptable since withholding also stops OBC after ~1 s. Sending one explicit disable is best-effort.)*
  3. `GPIO_CP_ENABLE = LOW` — de-asserts EVSE relay command; AC power removed from OBC input.
  4. Open HV charge contactors (see Contactor Open Sequence below).
  5. Set `frame_380.d[4][6] = 0` (AC relay bit cleared).
  6. Set `frame_5bf.d[4] = 0x28` (IDLE).
  7. `GPIO_STATUS_LED = LOW`.
- **Exit transition** (after sequence completes — evaluated in priority order):
  1. `vcm_sleep` (`0x50B d[2] == 0x00`, power switch OFF) → SLEEP *(`0x50B` is periodic at 100 ms so the flag re-arms during the 500 ms contactor delay and is reliably set at exit.)*
  2. `adc_mv <= SM_ADC_NO_PLUG_MV` → IDLE_PLUG *(plug body still present after charge end)*
  3. `adc_mv > SM_ADC_NO_PLUG_MV` → IDLE_NO_PLUG *(plug removed)*
- **TWAI0 TX obligations**: `0x380` and `0x5BF` continue uninterrupted (gptimer ISR still running). Content is updated in steps 5–6 above.
- **TWAI1 actions**: One best-effort `frame_050` with `Lear_ChargeEnable = 0` sent before task suspend (step 1–2).
- **Timeout**: None — exits as soon as shutdown sequence completes.

#### Contactor Open Sequence (CHARGE_DISABLE step 4)

Executed by the state machine task using `vTaskDelay()`. Reverse of the close sequence.

1. `GPIO_CONT_POS = LOW` — open positive HV contactor first
2. `vTaskDelay(pdMS_TO_TICKS(500))`
3. `GPIO_CONT_NEG = LOW` — open negative HV contactor

---

### CHARGE_DONE

- **Description**: Steady-state entered after a successful charge session (SOC cutoff reached). Controller remains in a minimal-power, plug-present configuration. TWAI0 TX continues (same content as IDLE_PLUG). TWAI1 TX stays suspended. No HV power active. The EV may show a "charge complete" indication via the 0x5BF status byte and sustained STATUS_LED.
- **Entry actions**:
  - `frame_5bf.d[2] = 0x00` — clear the J1772 current field (session ended).
  - `frame_5bf.d[4] = 0x28` (IDLE) — already set by `sm_enter_charge_disable()` step 6; no additional write needed.
  - `GPIO_STATUS_LED = HIGH` — sustained LED-on to indicate charge complete (distinct from blinking or off states used elsewhere).
  - TWAI1 TX task remains suspended (already done in CHARGE_DISABLE).
- **Exit actions**:
  - `GPIO_STATUS_LED = LOW` — cleared before entering downstream state.
- **TWAI0 TX obligations**:
  - `0x380` at 100 ms: AC relay bit = 0; power = `SM_MAX_CHARGE_POWER_W / 100`.
  - `0x5BF` at 100 ms: status byte = `0x28` (IDLE); J1772 current field = `0x00`.
- **TWAI0 RX triggers**:
  - `0x50B d[2] == 0x00` (power switch OFF) → SLEEP *(high priority)*
- **ADC trigger**:
  - `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C — plug removed) → IDLE_NO_PLUG
- **SOC trigger**:
  - SOC ≤ `g_sm_target_soc_pct − SM_SOC_RESTART_HYST` → IDLE_PLUG (charge again)
- **EV status trigger** *(commented out in `state_machine.c`)*:
  - `g_sm_ev_on == false` (0x50B `d[2] == 0x00` — EV system powers down) → IDLE_NO_PLUG
- **TWAI1 actions**: None. TX task remains suspended.
- **Timeout**: None — holds indefinitely until plug removal, VCM sleep, or EV status off.

> **Re-charge prevention note:** If `g_sm_ev_on` becomes `false` and then `true` again while the plug is still inserted (EV soft-cycles), the SM will transition `IDLE_NO_PLUG → IDLE_PLUG` and could re-arm a new session. If this is undesirable, a "session exhausted" latch cleared only on plug removal should be added.

---

## VCM-authorised path (`SM_SKIP_VCM_AUTH == 0`)

> **Source:** `LeafOBCBridge/docs/OBC-VCM-State-Sequence.md` (a real ZE0 AC-charge capture, verified frame by frame). The VCM authorises normal charging only with the **power switch OFF** (FSM EVC-35/40/41), after the OBC wakes it with the **EV system activation pulse** (OBC pin 18 → VCM pin 96, ≈ 1 s, EVC-78). The bridge then has to behave like the stock OBC on `0x380` / `0x5BF`.

**Path selection.** Power switch ON (`0x50B d[2] != 0x00`) → the bypass states above, unchanged. Power switch OFF → the states below. Every auth state before AUTH_START hands over to IDLE_PLUG (bypass) if the power switch turns ON.

**Decoded inputs.** `rx_dispatcher` writes `g_sm_rx` (`state_machine.h`) and frame counters. The SM ages every source with its own clock: a value counts only while its frame arrived within the stated window, otherwise it reads as "none" / false.

| Input | Source | Fresh window |
|---|---|---|
| VCM awake / GoToSleep | `0x50B` d[3][7:6] = `11` / `00` | `SM_VCM_SILENT_MS` |
| Power switch ON | `0x50B` d[2] != `0x00` | `SM_VCM_SILENT_MS` |
| VCM wake edge | WakeUp after GoToSleep or after `SM_VCM_SILENT_MS` silence (`wake_count`) | — |
| Power-button wake marker | `0x603` / `0x68C` (1-byte frames ≈ 40 ms before a power-button wake; never before a charge wake) | `SM_BEACON_WINDOW_MS` |
| Charge request | `0x1F2` d[2] & `0x60`: `0x20` normal charge, `0x60` stop request. **Always mask** — this car also sets d[2] bits 3 and 7 | 500 ms |
| Maximum charge power | `0x1F2` d[1]: (raw − 100) × 100 W | 500 ms |
| VCM MODE | `0x1F2` d[5] (CONSULT "VCM MODE": 0–7 start-up, 10 no charge, 30 READY, 109–129 entering charge, 130 charging) — trace only | — |
| HV live / main relay+ / charge session | `0x1D4` d[4] bit 2 / d[5] bit 6 / d[6] bit 7 | 500 ms |
| HV bus voltage | `0x1DA` d[0..1], 10 bit × 0.5 V (`d[0] = 0xFF` placeholder ignored) | 500 ms |
| AC present (Lear alive) | any TWAI1 frame — the Lear broadcasts only with AC input | `SM_LEAR_ALIVE_MS` |
| Lear DC voltage / current | `0x617` | `SM_LEAR_ALIVE_MS` |
| SOC | `0x55B` (placeholder `0x3FF` ignored) | last value |

**States.**

| State | Actions | Exits |
|---|---|---|
| SLEEP | — | VCM wake edge with a plug and no marker (charge timer start, immediate-charge switch) → VCM_AUTH. With a marker: wait for power ON (→ IDLE_NO_PLUG); none within `SM_POWER_ON_WAIT_MS` → VCM_AUTH. EV-CAN silent ≥ `SM_REWAKE_DELAY_MS` (re-armed by a failed attempt) + plug zone A + pilot > 0 A for `SM_PILOT_STABLE_MS` + wake attempts < `SM_VCM_WAKE_ATTEMPTS` + session not done + SOC allows → VCM_WAKE |
| VCM_WAKE | `GPIO_EVA_REQ` HIGH for `SM_EVA_PULSE_MS` (non-blocking; `sm_eva_tick()` ends it in any state) | WakeUp → VCM_AUTH. WakeUp with a marker → SLEEP (power-button wake). Power ON → IDLE_PLUG. Plug not in zone A → SLEEP. No WakeUp within `SM_VCM_WAKE_TIMEOUT_MS` → SLEEP (attempt spent) |
| VCM_AUTH | Appear: `0x380` `00 04 10 <chg> 00 00 00` (first frame after a wake), d[0] = `0x02` from 100 ms; `0x5BF` `0x10` → `0x18` after 100 ms. Announce while plug zone A, pilot > 0 A and SOC allows: +`SM_ANNOUNCE_READY_MS` `0x380` d[2] bit 3 (EVSE ready); +`SM_ANNOUNCE_S2_MS` pilot latched, `GPIO_CP_ENABLE` HIGH, `0x380` d[4] bit 4 (S2), chargeable power = min(pilot × V, max); +`SM_ANNOUNCE_PILOT_MS` `0x5BF` d[2] = pilot × 5. When the Lear broadcasts: AC voltage (`0x380` d[5..6]), `0x5BF` d[1] = `0x65`, AC-type bit 200 ms later. SOC at target after State C → withdraw | Request `0x20` + announced + zone A + SOC allows → AUTH_START. Request `0x60` → VCM_STANDBY (acknowledge). Power ON → IDLE_PLUG. GoToSleep → VCM_SHUTDOWN. EV-CAN silent → SLEEP. Plug removed or no request within `SM_CHARGE_REQ_TIMEOUT_MS` → VCM_STANDBY |
| VCM_STANDBY | State C released, announcement withdrawn. On a stop request: `0x380` d[0] bit 0 + d[2] = `0x20`, `0x5BF` `0x30` (acknowledged after `SM_STOP_ACK_TIMEOUT_MS` without a request) | Stop request withdrawn after being seen, plug zone A → VCM_AUTH (VCM restart: immediate-charge switch or timer). GoToSleep → VCM_SHUTDOWN. Power ON → IDLE_PLUG. EV-CAN silent → SLEEP |
| AUTH_START | `0x5BF` `0x40`, chargeable power 0. Then: HV live + relay+ + Lear alive → `SM_HV_SETTLE_MS` → `GPIO_CONT_NEG` → `SM_CONTACTOR_STEP_MS` → `GPIO_CONT_POS`, `0x380` d[4] bit 6, `0x5BF` `0x48` → +`SM_AUTH_STEP_MS` `0x50` + `0x380` d[7] bit 7 → `0x58` → chargeable power re-advertised | VCM power command > 0 (or `SM_FOLLOW_VCM_POWER_CMD == 0`, or `SM_POWER_CMD_TIMEOUT_MS` with a warning) → AUTH_CHARGING. Stop request, button or unplug, LBC failsafe, GoToSleep, HV not live within `SM_HV_LIVE_TIMEOUT_MS`, no AC within `SM_LEAR_AC_TIMEOUT_MS`, HV lost after a contactor closed → AUTH_STOP |
| AUTH_CHARGING | `0x050` ChargeEnable = 1, AC limit = min(pilot, `SM_MAX_CHARGE_POWER_W` / V, VCM command / V), updated as the command ramps; TWAI1 TX resumed; `0x5BF` `0x60`; telemetry: `0x380` output power (Lear V × A), `0x5BF` d[3] = DC V − 263, d[6] bits 7:3 = DC A | Stop request, power command back to 0, `0x1D4` charge session ended → AUTH_STOP (VCM, session done). Button or unplug → AUTH_STOP (release, session done). SOC ≥ target → AUTH_STOP (SOC, session done). `0x1F2` lost, LBC failsafe, HV lost, Lear silent, GoToSleep → AUTH_STOP (fault) |
| AUTH_STOP | Lear disabled; chargeable and output power 0; EVSE ready cleared; `0x5BF` `0x70` (VCM stop) or `0x80` (OBC stop); release-switch bit follows the button, or a 50 ms pulse for a SOC stop. Sequence: Lear DC < 1 A (or `SM_RAMP_DOWN_MS`) → TWAI1 suspended, State C released → VCM opens its main relays (HV live or relay+ cleared, or `SM_MAIN_RELAY_WAIT_MS`) → `GPIO_CONT_POS` → `SM_CONTACTOR_STEP_MS` → `GPIO_CONT_NEG`, `0x5BF` `0x78`. Only NEG closed → it opens at once | Stop request → VCM_STANDBY (acknowledge). GoToSleep → VCM_SHUTDOWN. Power ON → IDLE_PLUG. EV-CAN silent → SLEEP. No request within `SM_STOP_ACK_TIMEOUT_MS` → VCM_STANDBY (acknowledged anyway) |
| VCM_SHUTDOWN | Announcement withdrawn, stop-ack bit cleared, `0x5BF` `0x38` | `SM_SLEEP_TAIL_MS` (150 ms) after the **first** GoToSleep, or EV-CAN silent → SLEEP (TX off; the stock OBC sends one `0x38` frame, 120 ms after GoToSleep). WakeUp again → VCM_STANDBY |

**Plug-session latches**, all cleared when the plug is removed (zone C):
- *Wake attempts* — bridge-initiated wakes since the last charge (`SM_VCM_WAKE_ATTEMPTS`).
- *Fault stops* — reaching `SM_VCM_WAKE_ATTEMPTS` sets *session done*.
- *Session done* — set by VCM, SOC and button stops: no more bridge-initiated wakes. **VCM self-wakes are always answered.**
- *SOC stopped* — after a SOC-target stop a new announcement needs SOC < target − `SM_SOC_RESTART_HYST`.

**TWAI0 TX gate.** In the auth build the state machine owns `g_twai0_tx_enabled`. The auth states transmit only while a VCM frame (`0x50B`, `0x1F2` or `0x1D4`) arrived within `SM_TX_VCM_FRESH_MS` **and** GoToSleep has not lasted `SM_SLEEP_TAIL_MS`. TWAI0 retries an unacknowledged frame forever (a finite `fail_retry_cnt` means single-shot on the ESP32-C6 and would drop frames that lose arbitration), so a frame sent after the last node went to sleep would occupy the sleeping EV-CAN indefinitely; after GoToSleep the VCM keeps transmitting for ≈ 0.9 s, so the 150 ms tail is always acknowledged. Every other state follows `0x50B` d[2], exactly as the bypass build's 0x50B handler does.

**Lear hold-off.** On the first Lear broadcast after State C (AC present) the TWAI1 task is resumed with `0x050` ChargeEnable = 0 and AC limit 0, so the Lear is explicitly disabled — not relying on its ≈ 1 s command timeout — for at least `SM_HV_SETTLE_MS` before a contactor closes. AUTH_CHARGING then sets ChargeEnable = 1 and the limit; every withdrawal of State C suspends the task. Disabling always goes through `twai1_frame_050_set_enable(false)`, which also patches 0x050 frames already queued in the driver.

**Contactor rule.** On this path the charge contactors close only after `0x1D4` reports HV live and main relay+ (the VCM has precharged and closed its main relays) while the Lear is held off, and open only after the Lear output is below 1 A.

---

## Transitions

| From | To | Guard | Trigger | Priority |
|---|---|---|---|---|
| SLEEP | IDLE_NO_PLUG | — | `0x50B d[2] != 0x00` (power switch ON) | Normal |
| SLEEP | VCM_WAKE / VCM_AUTH | `SM_SKIP_VCM_AUTH == 0` | See [VCM-authorised path](#vcm-authorised-path-sm_skip_vcm_auth--0) | Normal |
| IDLE_NO_PLUG | SLEEP | — | `0x50B d[2] == 0x00` (power switch OFF) | Normal |
| IDLE_NO_PLUG | IDLE_PLUG | — | `adc_mv <= SM_ADC_NO_PLUG_MV` (Zone A or B — plug present) | Normal |
| IDLE_PLUG | IDLE_NO_PLUG | — | `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C — plug removed) | Normal |
| IDLE_PLUG | SLEEP | — | `0x50B d[2] == 0x00` | Normal |
| IDLE_PLUG | EVSE_ENABLE | — | `0x5BF` advanced to `0x18` (100 ms) AND Zone A | Normal |
| Auth states | Auth states / SLEEP / IDLE_PLUG | `SM_SKIP_VCM_AUTH == 0` | See [VCM-authorised path](#vcm-authorised-path-sm_skip_vcm_auth--0) | — |
| EVSE_ENABLE | CHARGING | — | Any TWAI1 frame received (0x616 / 0x617 / 0x618 / 0x274) | Normal |
| EVSE_ENABLE | CHARGE_DISABLE | — | `adc_mv >= SM_ADC_BUTTON_MV`, `0x50B` SLEEP, or `0x1DB` failsafe | High |
| CHARGING | CHARGE_DISABLE | — | `adc_mv >= SM_ADC_BUTTON_MV`, SOC ≥ cutoff, `0x50B` SLEEP, `0x1DB` failsafe, `0x1D4` interrupted | High/Normal |
| CHARGE_DISABLE | SLEEP | `vcm_sleep` | Shutdown sequence complete; VCM sleep re-received during 500 ms contactor delay | High |
| CHARGE_DISABLE | CHARGE_DONE | `charge_done_normal` AND `adc_mv <= SM_ADC_NO_PLUG_MV` | Shutdown complete, SOC cutoff path, plug present | Normal |
| CHARGE_DISABLE | IDLE_PLUG | `!vcm_sleep` AND `!charge_done_normal` AND `adc_mv <= SM_ADC_NO_PLUG_MV` | Shutdown complete, aborted/faulted, plug present | Normal |
| CHARGE_DISABLE | IDLE_NO_PLUG | `!vcm_sleep` AND `adc_mv > SM_ADC_NO_PLUG_MV` | Shutdown sequence complete, plug removed | Normal |
| CHARGE_DONE | SLEEP | — | `vcm_sleep` (0x50B SLEEP) | High |
| CHARGE_DONE | IDLE_NO_PLUG | — | `adc_mv > SM_ADC_NO_PLUG_MV` (plug removed) | Normal |
| CHARGE_DONE | IDLE_NO_PLUG | — | `g_sm_ev_on == false` (EV status byte = 0x00) | Normal |

---

## J1772 AC Current Derivation

> **Hardware constraint:** Once `GPIO_CP_ENABLE` is asserted (EVSE_ENABLE entry, or State C on the VCM-authorised path), the CP circuit shifts to a lower signal voltage and MCPWM edge detection becomes unreliable. The J1772 current is therefore frozen while `CP_ENABLE` is still LOW: into `g_session_ac_current_A` at the IDLE_PLUG → EVSE_ENABLE transition (bypass), or into the auth context just before State C is asserted (VCM-authorised path). It is not re-derived while State C is asserted.

**When:** Every 10 ms SM cycle while in `IDLE_PLUG`, provided `g_pwm_signal_valid == true`.  
**Stored in:** `DRAM_ATTR volatile float g_session_ac_current_A` — written once per plug session at IDLE_PLUG exit; read in CHARGING entry for `frame_050` encoding.  
**Inputs:** `g_pwm_duty_us` (DRAM_ATTR volatile), `g_pwm_period_us` (DRAM_ATTR volatile). Both written by the MCPWM `on_cap` ISR.

**Formula (J1772 SAE standard):**

```
duty_pct = (g_pwm_duty_us * 100.0f) / g_pwm_period_us

if (duty_pct >= 10.0f && duty_pct < 85.0f):
    i_max_A = duty_pct * 0.6f          // e.g. 50% duty → 30 A
else if (duty_pct >= 85.0f && duty_pct <= 96.0f):
    i_max_A = (duty_pct - 64.0f) * 2.5f  // e.g. 88% duty → 60 A
else:
    i_max_A = 0.0f                     // invalid pilot; treat as 0 A

// Clamp to OBC safe range
if (i_max_A > 32.0f) i_max_A = 32.0f  // single Lear OBC rated max; adjust if needed
if (i_max_A < 0.0f)  i_max_A = 0.0f
// i_max_A is stored as g_session_ac_current_A at the IDLE_PLUG exit transition

// Encoding — applied in CHARGING entry using the frozen session value:
uint16_t raw = (uint16_t)(g_session_ac_current_A * 10.0f)
frame_050.d[5] = (raw >> 8) & 0xFF
frame_050.d[6] = raw & 0xFF
```

**PWM absent in IDLE_PLUG:** If `g_pwm_signal_valid == false` throughout IDLE_PLUG when the `0x1F2` transition fires, `g_session_ac_current_A` remains `0.0f` and the OBC will be commanded with 0 A current limit. Whether a zero-current reading should block the transition to EVSE_ENABLE is left as a future decision.

**Do NOT read `g_pwm_duty_us`/`g_pwm_period_us` in CHARGING or any subsequent state.** The pilot signal is unreliable after `GPIO_CP_ENABLE` assertion. `g_session_ac_current_A` is the sole current limit source for the remainder of the session.

---

## TWAI0 TX Performance Notes

1. **TWAI0 TX is driven exclusively from the gptimer alarm ISR** (hardware priority, IRAM). The state machine task never calls `twai_node_transmit()` for TWAI0. This ensures sub-millisecond jitter on the 100 ms period regardless of FreeRTOS scheduler activity.
2. **State machine task only updates frame content** in the `DRAM_ATTR` application buffers (`frame_380_data`, `frame_5bf_data`). The gptimer ISR copies both into private TX buffers under `g_twai0_frame_lock` before sealing and transmitting them, so a multi-byte field written under the same lock is never sent half-updated. Single-byte stores need no lock.
3. **CHARGE_DISABLE step 1** writes `frame_050.d[7] = 0x00` (TWAI1 frame) before suspending the TWAI1 TX task. This is on a separate bus from TWAI0 and does not affect TWAI0 TX latency.
4. **`vTaskResume(twai1_tx_task_hdl)`** in CHARGING entry re-enables TWAI1 TX from the state machine task (priority 21). Since TWAI1 TX task is priority 22, it will preempt the state machine task immediately on resume. Ensure `frame_050` is fully configured before calling `vTaskResume()`.
5. **SOC** is decoded from `0x55B` by the RX dispatcher (priority 23) and written to a `DRAM_ATTR volatile float` or fixed-point variable. The state machine task reads it without a mutex; on RISC-V, 32-bit aligned reads are atomic.

---

## Open Questions

~~1. **VCM_AUTH timeout**~~ — RESOLVED by the VCM-authorised path: `SM_VCM_WAKE_TIMEOUT_MS` / `SM_VCM_WAKE_ATTEMPTS` bound the wake attempts, and `SM_CHARGE_REQ_TIMEOUT_MS` ends an announcement without a request (the VCM itself sends a stop request 10.24 s after a wake without a usable EVSE).

2. **EVSE_ENABLE timeout**: If `GPIO_CP_ENABLE` is asserted but no TWAI1 frame arrives within N seconds, should the controller timeout into CHARGE_DISABLE? If yes, what value of N?

~~3. **IDLE_PLUG 0x5BF timing** — RESOLVED: `0x10` is broadcast for exactly **100 ms** (1 frame). ZE0 log analysis (`leaf_ac_charge.txt`) confirmed both plug-insertion events show a fixed single-cycle advance to `0x18` with no variability.~~

~~4. **`0x1F2` entry trigger clarification**~~ — CORRECTED: the request is `0x1F2` d[2] **bits 6:5** (`Charge_StatusTransitionReqest`): `0x20` normal charge, `0x60` **stop request**. `0x60` is not a handshake: it is the stop request the VCM sends 10.24 s after a wake without a usable EVSE (or with a charge timer pending) and at every power-off. Mask with `0x60` — this car also sets d[2] bits 3 and 7.

5. **J1772 current clamp ceiling**: The derivation formula clamps at 32 A (single Lear OBC max). Confirm this is the correct ceiling for this installation.

---

## Out of Scope

- CHAdeMO DC fast charge (`0x5BF` status `0xB0` / QUICK_CHARGING)
- Drive mode / traction torque demand (`0x1D4` torque fields not decoded)
- VCM ALU security handshake (`0x50C` / `0x55B d[2]`)
- Thermal protection fault (OBC temperature from `0x274` — monitored but no control action defined)
