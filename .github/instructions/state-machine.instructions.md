---
applyTo: "**/state_machine*"
---
# OBCController State Machine Specification

## Global Configuration

All tunable parameters MUST be defined as macros in a dedicated config header (e.g. `sm_config.h`) so they can be changed without touching logic code.

| Parameter | Default | Macro |
|---|---|---|
| SOC charge cutoff | 80.0 % | `SM_SOC_CUTOFF_PCT 80.0f` |
| Max OBC charge power | 3300 W | `SM_MAX_CHARGE_POWER_W 3300` |
| VCM auth bypass | enabled | `SM_SKIP_VCM_AUTH 1` — set to `0` to re-enable VCM_AUTH state (IDLE_PLUG → VCM_AUTH → EVSE_ENABLE) |
| ADC no-plug threshold | 2300 mV | `SM_ADC_NO_PLUG_MV 2300` — above this = no plug (Zone C) |
| ADC button/cancel threshold | 900 mV | `SM_ADC_BUTTON_MV 900` — at or above this = button pressed or plug removed; cancel charge trigger |
| Lear OBC target voltage | 392 V | `SM_LEAR_TARGET_VOLTAGE_V 392` — fixed OBC DC voltage setpoint; change in `sm_config.h` |
| J1772 AC current limit | derived | Computed each SM cycle from MCPWM pilot duty cycle — see J1772 Current Derivation section |

---

## Plug Detection (ADC — Evaluated Every State Machine Cycle)

**Signal source:** `GPIO_ADC_INPUT` (GPIO2, ADC1_CH2), calibrated via `adc_cali_raw_to_voltage()` in the state machine task.  
**Update rate:** 250 Hz hardware (ISR writes `g_adc_raw_avg`); state machine reads and calibrates every 10 ms.

The ADC input reads a resistor-divider that encodes three operating zones:

| Zone | Voltage range | Meaning |
|---|---|---|
| A | < 900 mV (`SM_ADC_BUTTON_MV`) | Plug connected, no user action |
| B | 900 – 2300 mV | Plug connected, unlock button pressed — **cancel charge** |
| C | > 2300 mV (`SM_ADC_NO_PLUG_MV`) | No plug |

**Cancel charge condition (Zone B or C):** `adc_mv >= SM_ADC_BUTTON_MV`  
From any charging-active state (EVSE_ENABLE, CHARGING), this preempts all other logic and forces immediate entry into CHARGE_DISABLE. It catches both physical plug removal (Zone C) and the handle unlock button press (Zone B — plug body still present).

**Plug present:** `adc_mv <= SM_ADC_NO_PLUG_MV` (Zone A or B)  
**No plug:** `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C)

---

## State Machine Context Variables

These variables are file-scope statics internal to the state machine task. They carry context across state transitions to enable path-dependent behaviour.

| Variable | Type | Initial value | Purpose |
|---|---|---|---|
| `g_arrived_from_sleep` | `bool` | `false` | Set `true` when IDLE_PLUG is entered via WAKE (from SLEEP). Cleared to `false` when IDLE_PLUG is entered from IDLE_NO_PLUG or CHARGE_DISABLE. Controls IDLE_PLUG routing (VCM_AUTH vs EVSE_ENABLE) and plug-removal destination (SLEEP vs IDLE_NO_PLUG). |
| `eva_req_ticks` | `int` | `0` | EVA_REQ 1 s pulse countdown. Set to 100 on WAKE entry (100 × 10 ms SM cycles = 1 s). Decremented each 10 ms cycle while in IDLE_PLUG; when it reaches 0, `GPIO_EVA_REQ` is driven LOW (one-shot). Value is 0 on the awake path — no-op. |
| `idle_plug_ticks` | `int` | `0` | Counts 10 ms cycles from IDLE_PLUG entry to advance `0x5BF` status `0x10 → 0x18` (PLUG_BEEP) after 100 ms. Reset on every IDLE_PLUG entry. |
| `idle_plug_status_adv` | `bool` | `false` | Set `true` once PLUG_BEEP has been broadcast. Gate for awake-path auto-advance to EVSE_ENABLE. Cleared on every IDLE_PLUG entry. |
| `session_amps` | `float` | `0.0f` | J1772 current (A) frozen at the IDLE_PLUG exit transition. Written once per session; read by `sm_enter_charging()` for 0x050 and 0x5BF encoding. |

---

## States

### SLEEP

- **Description**: VCM commanded sleep (0x50B SLEEP); controller is fully quiescent — no CAN transmitted on either bus.
- **Entry actions**:
  - `g_twai0_tx_enabled = false` *(already cleared by RX dispatcher on 0x50B SLEEP per existing `twai-config` architecture — state machine need not write this directly)*
  - Confirm `vTaskSuspend(twai1_tx_task_hdl)` (TWAI1 TX task must not run)
  - `GPIO_CP_ENABLE = LOW`
  - `GPIO_CONT_POS = LOW`
  - `GPIO_CONT_NEG = LOW`
  - `GPIO_STATUS_LED = LOW`
- **Exit actions**: none.
- **TWAI0 TX obligations**: **None.** `g_twai0_tx_enabled = false`; gptimer ISR gate suppresses all TX.
- **TWAI0 RX triggers**:
  - `0x50B d[3][7:6] == 0b11` (RUN) → IDLE_NO_PLUG
    *(Note: the existing 5-second `esp_timer` delay before `g_twai0_tx_enabled` is set true is part of the TWAI0 TX gate architecture, not a state machine delay. The state machine transitions to IDLE_NO_PLUG immediately; TWAI0 TX simply remains gated until the timer fires.)*
- **ADC trigger**:
  - `adc_mv <= SM_ADC_NO_PLUG_MV` AND `g_pwm_signal_valid == true` (plug inserted, CP valid) → WAKE *(autonomous wake — VCM is still asleep; `g_twai0_tx_enabled` set directly by SM without the 5 s gate delay)*
- **TWAI1 actions**: None.
- **Timeout**: None.

---

### WAKE

- **Description**: Autonomous wake triggered from SLEEP when plug is inserted with a valid CP pilot signal. Pass-through state: WAKE-specific entry actions fire (EVA_REQ pulse, context flags, TWAI0 TX enable), then the state machine calls `sm_enter_idle_plug()` and advances to IDLE_PLUG in the same evaluation cycle. The 1 s `GPIO_EVA_REQ` pulse runs concurrently, managed by `eva_req_ticks` inside IDLE_PLUG.
- **Entry actions** (executed before advancing to IDLE_PLUG):
  - `g_twai0_tx_enabled = true` — enable TWAI0 TX immediately (no 5 s delay; VCM is asleep and must see 0x380/0x5BF as it wakes up from the EVA_REQ pulse)
  - `GPIO_EVA_REQ = HIGH` — begin 1 s EVA_REQ pulse
  - `eva_req_ticks = 100` — arm countdown; de-assertion handled each 10 ms cycle in IDLE_PLUG
  - `g_arrived_from_sleep = true` — tag IDLE_PLUG for sleep-path routing
  - `idle_plug_ticks = 0`, `idle_plug_status_adv = false`, `i_max_A = 0.0f`
  - Call `sm_enter_idle_plug()` and set `state = SM_STATE_IDLE_PLUG` (same cycle — zero dwell in WAKE)
- **Exit actions**: None (pass-through; IDLE_PLUG entry actions run as part of the immediate transition).
- **TWAI0 TX obligations**: None of its own — IDLE_PLUG's 100 ms TX obligations (0x380, 0x5BF) apply from the moment `g_twai0_tx_enabled` is set.
- **TWAI0 RX triggers**: None — exits immediately via entry action.
- **TWAI1 actions**: None.
- **Timeout**: None (zero-dwell state).

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
  - `0x50B d[3][7:6] == 0b00` (SLEEP) → SLEEP
- **ADC trigger**:
  - `adc_mv <= SM_ADC_NO_PLUG_MV` (Zone A or B — plug present) → IDLE_PLUG
- **TWAI1 actions**: None.
- **Timeout**: None.

---

### IDLE_PLUG

- **Description**: Plug physically inserted (ADC threshold crossed); controller broadcasts plug-present status on TWAI0 to trigger VCM wake sequence and pilot negotiation. Holds until VCM responds or plug is removed.
- **Entry actions**:
  - Set `frame_5bf.d[4] = 0x10` (TIMER_WAIT2) — triggers VCM wake sequence
  - After exactly **100 ms** (1 CAN frame period), set `frame_5bf.d[4] = 0x18` (PLUG_BEEP). Log analysis of `leaf_ac_charge.txt` confirms this is a fixed single-cycle advance: `0x10` appears for exactly one 100 ms frame in both observed plug-insertion events.
  - `GPIO_STATUS_LED = HIGH`
  - Reset `g_session_ac_current_A = 0.0f` (clears previous session value)
- **Per-cycle actions (each 10 ms SM cycle while in IDLE_PLUG)**:
  - If `g_pwm_signal_valid == true`: compute J1772 current from `g_pwm_duty_us`/`g_pwm_period_us` using the derivation formula below; store locally as `i_max_A`.
  - If `g_pwm_signal_valid == false`: hold `i_max_A = 0.0f`.
  - Write `frame_5bf.d[2] = (uint8_t)(i_max_A * 5.0f)` — broadcasts J1772 available current to VCM via `0x5BF d[2]` (`amps × 5` encoding, per frame spec).
  - **EVA_REQ countdown** (sleep path only — no-op when `eva_req_ticks == 0`): if `eva_req_ticks > 0`, decrement. When `eva_req_ticks` reaches 0: `GPIO_EVA_REQ = LOW` (one-shot de-assertion at exactly 1 s after WAKE entry).
  > **Hardware constraint:** J1772 PWM MUST be sampled here, before `GPIO_CP_ENABLE` is asserted. After `CP_ENABLE` goes HIGH the CP circuit shifts to a lower signal voltage and MCPWM edge detection becomes unreliable. The value derived here is frozen at transition time and used as the fixed current limit for the entire charging session.
- **Exit actions**:
  - `GPIO_STATUS_LED = LOW` (restored to LOW before entering downstream states that set it themselves)
- **TWAI0 TX obligations**:
  - `0x380` at 100 ms: AC relay bit = 0.
  - `0x5BF` at 100 ms: status byte sequence `0x10` (1 frame = 100 ms) → `0x18` (PLUG_BEEP, held until state exit).
- **TWAI0 RX triggers**:
  - `0x50B d[3][7:6] == 0b00` (SLEEP) → SLEEP *(plug remains but VCM slept; force `GPIO_EVA_REQ = LOW`, `eva_req_ticks = 0` if still pulsing)*
  - `0x1F2 d[2] == 0x20` (CHARGING_SLOW — VCM charge command) AND `g_arrived_from_sleep == true` → lock `g_session_ac_current_A = i_max_A`, then → VCM_AUTH *(sleep path always routes through VCM_AUTH)*
  > **Critical:** `d[2] == 0x60` MUST NOT trigger this transition. ZE0 log analysis confirms `0x60` appears during the ~20 s CP pilot handshake window (before VCM authorization) and again at session end. Only `0x20` reliably means the VCM has issued the charge command.
  > **J1772 current lock:** `g_session_ac_current_A` is frozen here — the last `i_max_A` derived in IDLE_PLUG — while `GPIO_CP_ENABLE` is still LOW and the pilot signal is still valid. This is the **only** point per session where `g_session_ac_current_A` is written.
- **ADC trigger**:
  - `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C — plug removed) AND `g_arrived_from_sleep == true` → SLEEP *(de-assert EVA_REQ immediately: `GPIO_EVA_REQ = LOW`, `eva_req_ticks = 0`; VCM never woke — return fully quiescent)*
  - `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C — plug removed) AND `g_arrived_from_sleep == false` → IDLE_NO_PLUG
- **Awake-path auto-advance** (`g_arrived_from_sleep == false` only): once `idle_plug_status_adv == true` (PLUG_BEEP elapsed, 100 ms) AND Zone A (`adc_mv < SM_ADC_BUTTON_MV`): lock `g_session_ac_current_A = i_max_A`, then → EVSE_ENABLE. No VCM charge command is required on this path.
- **TWAI1 actions**: None.
- **Timeout**: None defined — sleep path holds indefinitely awaiting VCM charge command; awake path auto-advances after 100 ms.

> **Note on `SM_SKIP_VCM_AUTH`**: **DEPRECATED.** VCM_AUTH routing is now path-dependent via `g_arrived_from_sleep`, not a compile-time flag. The sleep path (`g_arrived_from_sleep == true`) always uses VCM_AUTH; the awake path (`g_arrived_from_sleep == false`) always bypasses it. The macro remains in `sm_config.h` but must not be evaluated in new logic.

---

### VCM_AUTH

> **This state is skipped entirely when `SM_SKIP_VCM_AUTH != 0`.**

- **Description**: Controller waiting for VCM to grant L2 AC charge authorisation via `0x1F2 d[2] == 0x20` (CHARGING_SLOW).
- **Entry actions**:
  - Set `frame_5bf.d[4] = 0x30` (TIMER_WAIT)
- **Exit actions**: none.
- **TWAI0 TX obligations**:
  - `0x380` at 100 ms: AC relay bit = 0.
  - `0x5BF` at 100 ms: status byte = `0x30` (TIMER_WAIT).
- **TWAI0 RX triggers**:
  - `0x1F2 d[2] == 0x20` (CHARGING_SLOW) → EVSE_ENABLE
  - `0x50B d[3][7:6] == 0b00` (SLEEP) → SLEEP *(high priority — immediate)*
- **ADC trigger**:
  - `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C — plug removed) → SLEEP *(VCM_AUTH is only reachable via the sleep path; de-assert EVA_REQ if still pulsing: `GPIO_EVA_REQ = LOW`, `eva_req_ticks = 0`)*
  - `adc_mv >= SM_ADC_BUTTON_MV` AND `adc_mv <= SM_ADC_NO_PLUG_MV` (Zone B — button pressed, plug still present) → IDLE_PLUG *(re-enter IDLE_PLUG with `g_arrived_from_sleep = true` preserved; `eva_req_ticks` countdown continues)*
- **TWAI1 actions**: None.
- **Timeout**: **OPEN** — see Open Questions #4.

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
  - `0x50B d[3][7:6] == 0b00` (SLEEP) → CHARGE_DISABLE
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
    - `Lear_TargetVoltage = SM_LEAR_TARGET_VOLTAGE_V` (392 V → encoded as `392 × 10 = 3920` big-endian 16-bit in bytes 2–3)
    - `Lear_ACCurrentLimit` = `g_session_ac_current_A` (captured in IDLE_PLUG before `GPIO_CP_ENABLE` asserted — fixed for session; see J1772 Current Derivation section; **NOT re-derived in this state**)
  - `vTaskResume(twai1_tx_task_hdl)` — TWAI1 TX begins immediately at 100 ms cadence *(call **after** `frame_050` is fully written)*
  - `GPIO_STATUS_LED = HIGH`
- **Exit actions**: handled in CHARGE_DISABLE.
- **TWAI0 TX obligations**:
  - `0x380` at 100 ms: AC relay bit = 1; power = `SM_MAX_CHARGE_POWER_W / 100`.
  - `0x5BF` at 100 ms: `d[4]` = `0x60` (CHARGING); `d[2]` = `(uint8_t)(g_session_ac_current_A * 5.0f)` (J1772 available current, `amps × 5` — set on CHARGING entry, held constant for session).
- **TWAI0 RX triggers**:
  - `0x55B` decoded SOC ≥ `SM_SOC_CUTOFF_PCT` → CHARGE_DISABLE *(normal completion)*
  - `0x50B d[3][7:6] == 0b00` (SLEEP) → CHARGE_DISABLE
  - `0x1DB d[1][2:0] != 0b000` (LBC failsafe) → CHARGE_DISABLE
  - `0x1D4 d[6] == 0x8C` (STATUS_INTERRUPTED) → CHARGE_DISABLE *(VCM signalled charge interrupted)*
- **ADC trigger**:
  - `adc_mv >= SM_ADC_BUTTON_MV` (Zone B: button pressed, or Zone C: plug removed) → CHARGE_DISABLE *(immediate preemption)*
- **TWAI1 obligations**:
  - `0x050` at 100 ms (via TWAI1 TX task): `Lear_ChargeEnable = 1`; `Lear_TargetVoltage = SM_LEAR_TARGET_VOLTAGE_V`; `Lear_ACCurrentLimit` = `g_session_ac_current_A` (fixed session value — PWM edge detection unreliable after `GPIO_CP_ENABLE` assertion; **do not re-derive here**).
  - Monitor `0x617` (Lear_DCData) for DC voltage and current telemetry (informational; no control action currently defined).
  - Monitor `0x274` (Lear_Temperature) for OBC temperature (informational; fault action TBD).
- **Timeout**: None — charging runs until an exit trigger fires.

---

### CHARGE_DISABLE

- **Description**: Ordered charge shutdown sequence. Executes once, then transitions to IDLE_PLUG or IDLE_NO_PLUG based on current plug state.

**This is a transitional state, not a steady state.** The state machine task executes steps 1–3 immediately, then performs the contactor open sequence (step 4) with a 500 ms inter-step blocking delay via `vTaskDelay()`. Total minimum duration: ~500 ms. TWAI0 TX continues from the gptimer ISR throughout.

- **Entry actions (sequential)**:
  1. Set `frame_050.d[7] = 0x00` (`Lear_ChargeEnable = 0`) — write to the shared frame struct before the TWAI1 task next fires.
  2. `vTaskSuspend(twai1_tx_task_hdl)` — stops periodic 0x050. *(The frame with `ChargeEnable = 0` may or may not have been sent; that is acceptable since withholding also stops OBC after ~1 s. Sending one explicit disable is best-effort.)*
  3. `GPIO_CP_ENABLE = LOW` — de-asserts EVSE relay command; AC power removed from OBC input.
  4. Open HV charge contactors (see Contactor Open Sequence below).
  5. Set `frame_380.d[4][6] = 0` (AC relay bit cleared).
  6. Set `frame_5bf.d[4] = 0x28` (IDLE).
  7. `GPIO_STATUS_LED = LOW`.
- **Exit transition** (after sequence completes):
  - Clear `g_arrived_from_sleep = false` before any exit — a completed charge session means VCM is now awake; the next IDLE_PLUG entry uses the awake path.
  - `adc_mv <= SM_ADC_NO_PLUG_MV` → IDLE_PLUG *(plug body still present after charge end)*
  - `adc_mv > SM_ADC_NO_PLUG_MV` → IDLE_NO_PLUG *(plug removed)*
- **TWAI0 TX obligations**: `0x380` and `0x5BF` continue uninterrupted (gptimer ISR still running). Content is updated in steps 5–6 above.
- **TWAI1 actions**: One best-effort `frame_050` with `Lear_ChargeEnable = 0` sent before task suspend (step 1–2).
- **Timeout**: None — exits as soon as shutdown sequence completes.

#### Contactor Open Sequence (CHARGE_DISABLE step 4)

Executed by the state machine task using `vTaskDelay()`. Reverse of the close sequence.

1. `GPIO_CONT_POS = LOW` — open positive HV contactor first
2. `vTaskDelay(pdMS_TO_TICKS(500))`
3. `GPIO_CONT_NEG = LOW` — open negative HV contactor

---

## Transitions

| From | To | Guard | Trigger | Priority |
|---|---|---|---|---|
| SLEEP | IDLE_NO_PLUG | — | `0x50B d[3][7:6] == 0b11` (RUN) | Normal |
| SLEEP | WAKE | `adc_mv <= SM_ADC_NO_PLUG_MV` AND `g_pwm_signal_valid == true` | ADC + MCPWM | Normal |
| WAKE | IDLE_PLUG | — | Immediate pass-through (same evaluation cycle) | — |
| IDLE_NO_PLUG | SLEEP | — | `0x50B d[3][7:6] == 0b00` (SLEEP) | Normal |
| IDLE_NO_PLUG | IDLE_PLUG | — | `adc_mv <= SM_ADC_NO_PLUG_MV` (Zone A or B — plug present) | Normal |
| IDLE_PLUG | SLEEP | `g_arrived_from_sleep == true` | `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C — plug removed) | Normal |
| IDLE_PLUG | IDLE_NO_PLUG | `g_arrived_from_sleep == false` | `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C — plug removed) | Normal |
| IDLE_PLUG | SLEEP | — | `0x50B d[3][7:6] == 0b00` | Normal |
| IDLE_PLUG | VCM_AUTH | `g_arrived_from_sleep == true` | `0x1F2 d[2] == 0x20` (CHARGING_SLOW) | Normal |
| IDLE_PLUG | EVSE_ENABLE | `g_arrived_from_sleep == false` AND `idle_plug_status_adv == true` AND Zone A | Auto-advance after PLUG_BEEP (100 ms) | Normal |
| VCM_AUTH | EVSE_ENABLE | — | `0x1F2 d[2] == 0x20` (CHARGING_SLOW) | Normal |
| VCM_AUTH | SLEEP | — | `0x50B d[3][7:6] == 0b00` OR `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C) | High |
| VCM_AUTH | IDLE_PLUG | — | `adc_mv >= SM_ADC_BUTTON_MV` AND Zone B (plug still present) | High |
| EVSE_ENABLE | CHARGING | — | Any TWAI1 frame received (0x616 / 0x617 / 0x618 / 0x274) | Normal |
| EVSE_ENABLE | CHARGE_DISABLE | — | `adc_mv >= SM_ADC_BUTTON_MV`, `0x50B` SLEEP, or `0x1DB` failsafe | High |
| CHARGING | CHARGE_DISABLE | — | `adc_mv >= SM_ADC_BUTTON_MV`, SOC ≥ cutoff, `0x50B` SLEEP, `0x1DB` failsafe, `0x1D4` interrupted | High/Normal |
| CHARGE_DISABLE | IDLE_PLUG | `adc_mv <= SM_ADC_NO_PLUG_MV` | Shutdown sequence complete | Normal |
| CHARGE_DISABLE | IDLE_NO_PLUG | `adc_mv > SM_ADC_NO_PLUG_MV` | Shutdown sequence complete | Normal |

---

## J1772 AC Current Derivation

> **Hardware constraint:** Once `GPIO_CP_ENABLE` is asserted (EVSE_ENABLE entry), the CP circuit shifts to a lower signal voltage and MCPWM edge detection becomes unreliable. The J1772 current derivation is performed **only in IDLE_PLUG** (while `CP_ENABLE` is still LOW), sampled each 10 ms SM cycle. The result is frozen into `g_session_ac_current_A` at the moment `0x1F2 d[2] == 0x20` fires and is not re-derived in any subsequent state.

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
2. **State machine task only updates frame content** in `DRAM_ATTR` frame structs (`frame_380`, `frame_5bf`). Writes happen at the 10 ms task cycle. Because frames are updated every 10 ms and transmitted every 100 ms, a partially-updated frame is possible on a 10 ms/100 ms boundary. If frame integrity issues appear in testing, add a `DRAM_ATTR volatile` dirty flag: ISR skips transmission if flag is set; task clears it after writing.
3. **CHARGE_DISABLE step 1** writes `frame_050.d[7] = 0x00` (TWAI1 frame) before suspending the TWAI1 TX task. This is on a separate bus from TWAI0 and does not affect TWAI0 TX latency.
4. **`vTaskResume(twai1_tx_task_hdl)`** in CHARGING entry re-enables TWAI1 TX from the state machine task (priority 21). Since TWAI1 TX task is priority 22, it will preempt the state machine task immediately on resume. Ensure `frame_050` is fully configured before calling `vTaskResume()`.
5. **SOC** is decoded from `0x55B` by the RX dispatcher (priority 23) and written to a `DRAM_ATTR volatile float` or fixed-point variable. The state machine task reads it without a mutex; on RISC-V, 32-bit aligned reads are atomic.
6. **WAKE TWAI0 TX enable**: In WAKE entry, `g_twai0_tx_enabled` is set directly to `true` by the state machine task — no 5 s gate delay. This differs from the VCM RUN path (which uses `twai0_tx_delay_timer`). The delay is intentionally skipped: VCM is asleep and must see 0x380/0x5BF frames as it starts up in response to the EVA_REQ pulse.

---

## Open Questions

1. **VCM_AUTH timeout**: If no `0x1F2 d[2] == 0x20` (CHARGING_SLOW) is received after plug insertion, should the state machine timeout back to IDLE_PLUG (with a retry count) or enter a FAULT state? If yes, what duration?

2. **EVSE_ENABLE timeout**: If `GPIO_CP_ENABLE` is asserted but no TWAI1 frame arrives within N seconds, should the controller timeout into CHARGE_DISABLE? If yes, what value of N?

~~3. **IDLE_PLUG 0x5BF timing** — RESOLVED: `0x10` is broadcast for exactly **100 ms** (1 frame). ZE0 log analysis (`leaf_ac_charge.txt`) confirmed both plug-insertion events show a fixed single-cycle advance to `0x18` with no variability.~~

~~4. **`0x1F2` entry trigger clarification** — RESOLVED: `d[2] == 0x20` is the only valid trigger. ZE0 log analysis (`leaf_ac_charge.txt`) shows: idle=`0x00`, CP handshake=`0x60` (~20 s, not charge-active), VCM charge command=`0x20` (single-frame step, stable for entire 251.7 s session). `0x60` is explicitly excluded — it appears before authorization and at session end. No `0x88` was observed.~~

5. **J1772 current clamp ceiling**: The derivation formula clamps at 32 A (single Lear OBC max). Confirm this is the correct ceiling for this installation.

---

## Out of Scope

- CHAdeMO DC fast charge (`0x5BF` status `0xB0` / QUICK_CHARGING)
- Drive mode / traction torque demand (`0x1D4` torque fields not decoded)
- VCM ALU security handshake (`0x50C` / `0x55B d[2]`)
- Thermal protection fault (OBC temperature from `0x274` — monitored but no control action defined)
