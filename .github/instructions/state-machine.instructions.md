---
applyTo: "**/state_machine*"
---
# OBCController State Machine Specification

## Global Configuration

All tunable parameters MUST be defined as macros in a dedicated config header (e.g. `sm_config.h`) so they can be changed without touching logic code.

| Parameter | Default | Macro |
|---|---|---|
| SOC charge cutoff | 90.0 % | `SM_SOC_CUTOFF_PCT 90.0f` — startup default for `g_sm_target_soc_pct`; overwritten at runtime by CAN 0x6C0 (range `SM_TARGET_SOC_MIN_PCT`–`SM_TARGET_SOC_MAX_PCT`) |
| Max OBC charge power | 3300 W | `SM_MAX_CHARGE_POWER_W 3300` |
| Plug-in indicator | enabled | `SM_PLUG_SIG_ENABLE 1` — drive `GPIO_PLUG_SIG` (OBC pin 21 → combination meter plug icon) while a plug is present (zone A or B), in every state |
| ADC no-plug threshold | 2300 mV | `SM_ADC_NO_PLUG_MV 2300` — above this = no plug (Zone C) |
| ADC button/cancel threshold | 700 mV | `SM_ADC_BUTTON_MV 700` — at or above this = button pressed or plug removed; cancel charge trigger |
| Lear OBC target voltage | 400 V | `SM_LEAR_TARGET_VOLTAGE_V 400` — startup default for `g_sm_target_voltage_v`; overwritten at runtime by CAN 0x6C0 (range `SM_TARGET_VOLTAGE_MIN_V`–`SM_TARGET_VOLTAGE_MAX_V`); re-encoded into 0x050 immediately if changed in AUTH_CHARGING |
| J1772 AC current limit | derived | Computed each SM cycle from MCPWM pilot duty cycle — see J1772 Current Derivation section |
| VCM-authorised charging timing | see `sm_config.h` | `SM_EVA_PULSE_MS`, `SM_VCM_WAKE_TIMEOUT_MS`, `SM_VCM_WAKE_ATTEMPTS`, `SM_REWAKE_DELAY_MS`, `SM_PILOT_STABLE_MS`, `SM_VCM_SILENT_MS`, `SM_BEACON_WINDOW_MS`, `SM_POWER_ON_WAIT_MS`, `SM_ANNOUNCE_*_MS`, `SM_CHARGE_REQ_TIMEOUT_MS`, `SM_HV_LIVE_TIMEOUT_MS`, `SM_LEAR_AC_TIMEOUT_MS`, `SM_LEAR_ALIVE_MS`, `SM_LEAR_AC_SETTLE_MS`, `SM_HV_SETTLE_MS`, `SM_CONTACTOR_STEP_MS`, `SM_AUTH_STEP_MS`, `SM_POWER_CMD_TIMEOUT_MS`, `SM_VCM_EARLY_STOP_MS`, `SM_RAMP_DOWN_MS`, `SM_MAIN_RELAY_WAIT_MS`, `SM_STOP_ACK_TIMEOUT_MS`, `SM_SLEEP_TAIL_MS`, `SM_TX_VCM_FRESH_MS` |
| Follow the VCM power command | enabled | `SM_FOLLOW_VCM_POWER_CMD 1` — Lear AC limit follows `0x1F2` d[1]; `0` = pilot and `SM_MAX_CHARGE_POWER_W` only |
| AC present threshold | 90 V | `SM_AC_PRESENT_MIN_V 90` — AC counts as present at the Lear only while its measured AC input voltage (`0x27D`) read at least this within `SM_LEAR_ALIVE_MS`; that voltage is reported in `0x380` and converts watts to Lear AC amps |
| AC input class boundary | 160 V | `SM_AC_200V_CLASS_MIN_V 160` — `0x380` d[0] AC-input-type bit from the measured voltage: 0x10 = 200 V class at or above, else 0x08 = 100 V class |
| Signal trace | enabled | `SM_LOG_VCM_TRACE 1` — rate-limited INFO log of `0x50B`, `0x1F2` (with the VCM MODE name), `0x1D4`, `0x1DA` and Lear changes |
| Lear raw capture | enabled | `SM_LOG_LEAR_RAW 1` — every TWAI1 ID is received and logged (tag `lear`) when its payload changes, at most once per `SM_LOG_LEAR_RAW_MIN_MS` per ID, with the bits that flipped; extra IDs never count as Lear alive. See `twai1-can-frames.instructions.md` |

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
In AUTH_START and AUTH_CHARGING this stops the charge (→ AUTH_STOP, release-switch stop, session done); before State C it holds off the EVSE announcement. It catches both physical plug removal (Zone C) and the handle unlock button press (Zone B — plug body still present).

**Plug present:** `adc_mv <= SM_ADC_NO_PLUG_MV` (Zone A or B)  
**No plug:** `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C)

---

## States

Charging needs VCM authorisation, and the VCM authorises normal charging only with the **power switch OFF** (FSM EVC-35/40/41: "Li-ion battery charge does not start when the power switch is ON"). With the power switch OFF the bridge wakes the VCM with the **EV system activation pulse** (OBC pin 18 → VCM pin 96, ≈ 1 s, EVC-78) and behaves like the stock OBC on `0x380` / `0x5BF`. With the power switch ON it only sends idle OBC frames and never charges: a plug inserted then charges after the car is switched off, once the VCM has gone to sleep and the bridge wakes it. A charge already authorised continues if the power switch is turned ON.

> **Source:** `LeafOBCBridge/docs/OBC-VCM-State-Sequence.md` (a real ZE0 AC-charge capture, verified frame by frame).

> **0x50B in this firmware:** d[2] is non-zero only while the **power switch is ON** (`0x02` → `0x06`). A charge wake keeps d[2] = `0x00` for the whole session; there only d[3][7:6] (`11` WakeUp / `00` GoToSleep) shows the VCM is awake.

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
| Lear alive | any frame with a core ID (`0x274`, `0x27D`, `0x616`, `0x617`, `0x618`) on TWAI1 — the Lear broadcasts whenever it is powered: in a charge wake from ≈ 1.2–2.2 s after State C, also with the car in READY and no plug, and for ≈ 6–25 s after the VCM sleeps | `SM_LEAR_ALIVE_MS` |
| AC present at the Lear | Lear AC input voltage ≥ `SM_AC_PRESENT_MIN_V` after State C; if the Lear was already broadcasting when State C was asserted, only readings from `SM_LEAR_AC_SETTLE_MS` after State C count (see **Lear hold-off**) | reported until 400 ms after State C is released; while reported, AC stays present as long as a reading ≥ `SM_AC_PRESENT_MIN_V` is less than `SM_LEAR_ALIVE_MS` old |
| Lear DC voltage / current | `0x617` | `SM_LEAR_ALIVE_MS` |
| Lear AC input voltage | `0x27D` d[0..1] ÷ 32 V (0.5 V steps) | `SM_LEAR_ALIVE_MS` |
| SOC | `0x55B` (placeholder `0x3FF` ignored) | last value |

**States.**

| State | Actions | Exits |
|---|---|---|
| SLEEP | Nothing driven; TWAI0 TX off, TWAI1 suspended | Power switch ON → POWER_ON. VCM wake edge with a plug and no marker (charge timer start, immediate-charge switch) → VCM_AUTH. With a marker: wait for power ON (→ POWER_ON); none within `SM_POWER_ON_WAIT_MS` → VCM_AUTH. EV-CAN silent ≥ `SM_REWAKE_DELAY_MS` (re-armed by a failed attempt) + plug zone A + pilot > 0 A for `SM_PILOT_STABLE_MS` + wake attempts < `SM_VCM_WAKE_ATTEMPTS` + session not done + SOC allows → VCM_WAKE |
| POWER_ON | Nothing driven (State C released, contactors open, TWAI1 suspended, LED off); `0x380` / `0x5BF` at their power-up content (`02 04 10 21 …`, `0x5BF` `0x28`) while the power switch is ON. The plug only drives `GPIO_PLUG_SIG` | Power switch OFF or EV-CAN silent → SLEEP |
| VCM_WAKE | `GPIO_EVA_REQ` HIGH for `SM_EVA_PULSE_MS` (non-blocking; `sm_eva_tick()` ends it in any state) | WakeUp → VCM_AUTH. WakeUp with a marker → SLEEP (power-button wake). Power ON → POWER_ON. Plug not in zone A → SLEEP. No WakeUp within `SM_VCM_WAKE_TIMEOUT_MS` → SLEEP (attempt spent) |
| VCM_AUTH | Appear: `0x380` `00 04 10 <chg> 00 00 00` (first frame after a wake), d[0] = `0x02` from 100 ms; `0x5BF` `0x10` → `0x18` after 100 ms. Announce while plug zone A, pilot > 0 A and SOC allows: +`SM_ANNOUNCE_READY_MS` `0x380` d[2] bit 3 (EVSE ready); +`SM_ANNOUNCE_S2_MS` pilot latched, `GPIO_CP_ENABLE` HIGH, `0x380` d[4] bit 4 (S2), chargeable power = min(pilot × V, max); +`SM_ANNOUNCE_PILOT_MS` `0x5BF` d[2] = pilot × 5. When AC is present at the Lear: AC voltage (`0x380` d[5..6]), `0x5BF` d[1] = `0x65`, AC-type bit 200 ms later. SOC at target after State C → withdraw | Request `0x20` + announced + zone A + SOC allows → AUTH_START. Request `0x60` → VCM_STANDBY (acknowledge). Power ON → POWER_ON. GoToSleep → VCM_SHUTDOWN. EV-CAN silent → SLEEP. Plug removed or no request within `SM_CHARGE_REQ_TIMEOUT_MS` → VCM_STANDBY |
| VCM_STANDBY | State C released, announcement withdrawn. On a stop request: `0x380` d[0] bit 0 + d[2] = `0x20`, `0x5BF` `0x30` (acknowledged after `SM_STOP_ACK_TIMEOUT_MS` without a request) | Stop request withdrawn after being seen, plug zone A → VCM_AUTH (VCM restart: immediate-charge switch or timer). GoToSleep → VCM_SHUTDOWN. Power ON → POWER_ON. EV-CAN silent → SLEEP |
| AUTH_START | `0x5BF` `0x40`, chargeable power 0. Then: HV live + relay+ + AC present at the Lear (Lear held off) → `SM_HV_SETTLE_MS` → `GPIO_CONT_NEG` → `SM_CONTACTOR_STEP_MS` → `GPIO_CONT_POS`, `0x380` d[4] bit 6, `0x5BF` `0x48` → +`SM_AUTH_STEP_MS` `0x50` + `0x380` d[7] bit 7 → `0x58` → chargeable power re-advertised | VCM power command > 0 (or `SM_FOLLOW_VCM_POWER_CMD == 0`, or `SM_POWER_CMD_TIMEOUT_MS` with a warning) → AUTH_CHARGING. Stop request, button or unplug, LBC failsafe, GoToSleep, HV not live within `SM_HV_LIVE_TIMEOUT_MS`, no AC within `SM_LEAR_AC_TIMEOUT_MS`, HV lost after a contactor closed, AC lost after it was confirmed → AUTH_STOP |
| AUTH_CHARGING | `0x050` ChargeEnable = 1, AC limit = min(pilot, `SM_MAX_CHARGE_POWER_W` / V, VCM command / V) with V the measured AC input voltage, updated as the command ramps; TWAI1 TX resumed; `0x5BF` `0x60`; telemetry: `0x380` output power (Lear V × A), `0x5BF` d[3] = DC V − 263, d[6] bits 7:3 = DC A | Stop request, power command back to 0, `0x1D4` charge session ended → AUTH_STOP (VCM, session done). Button or unplug → AUTH_STOP (release, session done). SOC ≥ target → AUTH_STOP (SOC, session done). `0x1F2` lost, LBC failsafe, HV lost, Lear silent, AC lost, GoToSleep → AUTH_STOP (fault) |
| AUTH_STOP | Lear disabled; chargeable and output power 0; EVSE ready cleared; `0x5BF` `0x70` (VCM stop) or `0x80` (OBC stop); release-switch bit follows the button, or a 50 ms pulse for a SOC stop. Sequence: Lear DC < 1 A (or `SM_RAMP_DOWN_MS`) → TWAI1 suspended, State C released → VCM opens its main relays (HV live or relay+ cleared, or `SM_MAIN_RELAY_WAIT_MS`) → `GPIO_CONT_POS` → `SM_CONTACTOR_STEP_MS` → `GPIO_CONT_NEG`, `0x5BF` `0x78`. Only NEG closed → it opens at once | Stop request → VCM_STANDBY (acknowledge). GoToSleep → VCM_SHUTDOWN. Power ON → POWER_ON. EV-CAN silent → SLEEP. No request within `SM_STOP_ACK_TIMEOUT_MS` → VCM_STANDBY (acknowledged anyway) |
| VCM_SHUTDOWN | Announcement withdrawn, stop-ack bit cleared, `0x5BF` `0x38` | `SM_SLEEP_TAIL_MS` (150 ms) after the **first** GoToSleep, or EV-CAN silent → SLEEP (TX off; the stock OBC sends one `0x38` frame, 120 ms after GoToSleep). WakeUp again → VCM_STANDBY |

**Early VCM stop.** A VCM-initiated stop (stop request, power command back to 0, or `0x1D4` charge session ended) during AUTH_START, or within `SM_VCM_EARLY_STOP_MS` of entering AUTH_CHARGING, logs `SM: VCM ended the charge … — check the car's charge timer and charge level`. The car is declining the charge: with a charge timer set (80 % limit) the VCM runs the whole start sequence to MODE 130 and a power command, then stops ≈ 2 s later. Observed: `0x1F2` d[0] then stays `0x10`, where an accepted charge moves to `0x30` with the first power step.

**Plug-session latches**, all cleared when the plug is removed (zone C):
- *Wake attempts* — bridge-initiated wakes since the last charge (`SM_VCM_WAKE_ATTEMPTS`).
- *Fault stops* — reaching `SM_VCM_WAKE_ATTEMPTS` sets *session done*.
- *Session done* — set by VCM, SOC and button stops: no more bridge-initiated wakes. **VCM self-wakes are always answered.**
- *SOC stopped* — after a SOC-target stop a new announcement needs SOC < target − `SM_SOC_RESTART_HYST`.

**TWAI0 TX gate.** The state machine owns `g_twai0_tx_enabled`. VCM_AUTH, VCM_STANDBY, AUTH_START, AUTH_CHARGING, AUTH_STOP and VCM_SHUTDOWN transmit only while a VCM frame (`0x50B`, `0x1F2` or `0x1D4`) arrived within `SM_TX_VCM_FRESH_MS` **and** GoToSleep has not lasted `SM_SLEEP_TAIL_MS`. TWAI0 retries an unacknowledged frame forever (a finite `fail_retry_cnt` means single-shot on the ESP32-C6 and would drop frames that lose arbitration), so a frame sent after the last node went to sleep would occupy the sleeping EV-CAN indefinitely; after GoToSleep the VCM keeps transmitting for ≈ 0.9 s, so the 150 ms tail is always acknowledged. SLEEP and POWER_ON transmit while the power switch is ON (`0x50B` d[2] != `0x00`, fresh within `SM_VCM_SILENT_MS`).

**Lear hold-off.** When AC is first present at the Lear after State C the TWAI1 task is resumed with `0x050` ChargeEnable = 0 and AC limit 0, so the Lear is explicitly disabled — not relying on its ≈ 1 s command timeout — for at least `SM_HV_SETTLE_MS` before a contactor closes. AUTH_CHARGING then sets ChargeEnable = 1 and the limit; every withdrawal of State C suspends the task. Disabling always goes through `twai1_frame_050_set_enable(false)`, which also patches 0x050 frames already queued in the driver.

> **A Lear still broadcasting at State C is not AC.** The Lear broadcasts whenever it is powered — also without AC (car in READY, no plug, or an EVSE that never closes its relay) and for ≈ 6–25 s after the VCM sleeps — so AC is judged from its measured AC input voltage, never from its frames alone. A Lear still broadcasting when the VCM wakes again drops off TWAI1 for ≈ 3.5–4 s; a cold Lear first broadcasts ≈ 1.2–2.2 s after State C. If the Lear is already alive when State C is asserted, AC counts only from a voltage reading received `SM_LEAR_AC_SETTLE_MS` after State C. Starting `0x050` at State C instead sends it to a Lear that is not on the bus: TWAI1 retries it forever (`fail_retry_cnt = -1`) and every retry raises a bus error.

**AC voltage.** From AC confirmation until 400 ms after State C is released, `0x380` carries the Lear's measured AC input voltage (`0x27D`), updated with every reading as the stock OBC does (229 V idle, 222.5 V at 3.3 kW in the ZE0 capture). The AC-input-type bit follows 200 ms after the first reading: 200 V class at or above `SM_AC_200V_CLASS_MIN_V`, else 100 V class. The same reading converts watts to Lear AC amps. No voltage is known before AC is confirmed, so until then the chargeable power is `SM_MAX_CHARGE_POWER_W`; the pilot limit (amps × V) first applies to the re-advertisement at the end of the relay sequence.

**Contactor rule.** The charge contactors close only after `0x1D4` reports HV live and main relay+ (the VCM has precharged and closed its main relays) while the Lear is held off, and open only after the Lear output is below 1 A.

---

## J1772 AC Current Derivation

> **Hardware constraint:** Once `GPIO_CP_ENABLE` is asserted (State C), the CP circuit shifts to a lower signal voltage and MCPWM edge detection becomes unreliable. The J1772 current is therefore latched while `CP_ENABLE` is still LOW — into the auth context (`s_auth.amps`) just before VCM_AUTH asserts State C — and is not re-derived while State C is asserted.

**When:** Every 10 ms SM cycle (`pilot_a`), provided `g_pwm_signal_valid == true`; 0 A otherwise. It gates the bridge-initiated wake (`SM_PILOT_STABLE_MS`) and the EVSE announcement.  
**Used for:** `0x5BF` d[2] (amps × 5); `0x380` chargeable power = min(amps × V, `SM_MAX_CHARGE_POWER_W`) once the AC voltage V is measured (`SM_MAX_CHARGE_POWER_W` before); `0x050` AC limit = min(amps, `SM_MAX_CHARGE_POWER_W` / V, VCM power command / V), V = measured AC input voltage (`0x27D`).  
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
// i_max_A is latched into s_auth.amps just before State C is asserted

// Encoding into 0x050 in AUTH_CHARGING, after the OBC and VCM power-command limits:
uint16_t raw = (uint16_t)(limit_A * 10.0f)
frame_050.d[5] = (raw >> 8) & 0xFF
frame_050.d[6] = raw & 0xFF
```

**No pilot:** without a valid pilot (> 0 A) the bridge neither wakes the VCM nor announces the EVSE, so a session never starts with a 0 A limit.

**Do not use the pilot reading while State C is asserted.** The latched `s_auth.amps` is the only current-limit source for the rest of the session.

---

## TWAI0 TX Performance Notes

1. **TWAI0 TX is driven exclusively from the gptimer alarm ISR** (hardware priority, IRAM). The state machine task never calls `twai_node_transmit()` for TWAI0. This ensures sub-millisecond jitter on the 100 ms period regardless of FreeRTOS scheduler activity.
2. **State machine task only updates frame content** in the `DRAM_ATTR` application buffers (`frame_380_data`, `frame_5bf_data`). The gptimer ISR copies both into private TX buffers under `g_twai0_frame_lock` before sealing and transmitting them, so a multi-byte field written under the same lock is never sent half-updated. Single-byte stores need no lock.
3. **Disabling the Lear** (`twai1_frame_050_set_enable(false)`: AUTH_STOP and every release of State C) clears `frame_050.d[7]` — and any 0x050 still queued — before the TWAI1 TX task is suspended. This is on a separate bus from TWAI0 and does not affect TWAI0 TX latency.
4. **`vTaskResume(twai1_tx_task_hdl)`** (Lear hold-off and AUTH_CHARGING entry) re-enables TWAI1 TX from the state machine task (priority 21). Since TWAI1 TX task is priority 22, it will preempt the state machine task immediately on resume. Ensure `frame_050` is fully configured before calling `vTaskResume()`.
5. **SOC** is decoded from `0x55B` by the RX dispatcher (priority 23) into `g_sm_rx.soc_x10` (0.1 %; the `0x3FF` placeholder is ignored). The state machine task reads it without a mutex; on RISC-V, aligned reads up to 32 bits are atomic.

---

## Open Questions

~~1. **VCM_AUTH timeout**~~ — RESOLVED by the VCM-authorised path: `SM_VCM_WAKE_TIMEOUT_MS` / `SM_VCM_WAKE_ATTEMPTS` bound the wake attempts, and `SM_CHARGE_REQ_TIMEOUT_MS` ends an announcement without a request (the VCM itself sends a stop request 10.24 s after a wake without a usable EVSE).

~~2. **EVSE_ENABLE timeout**~~ — OBSOLETE: the bypass path was removed. AUTH_START stops with a fault if AC is not confirmed at the Lear within `SM_LEAR_AC_TIMEOUT_MS`.

~~3. **0x5BF start-up timing** — RESOLVED: `0x10` is broadcast for exactly **100 ms** (1 frame). ZE0 log analysis (`leaf_ac_charge.txt`) confirmed both plug-insertion events show a fixed single-cycle advance to `0x18` with no variability.~~

~~4. **`0x1F2` entry trigger clarification**~~ — CORRECTED: the request is `0x1F2` d[2] **bits 6:5** (`Charge_StatusTransitionReqest`): `0x20` normal charge, `0x60` **stop request**. `0x60` is not a handshake: it is the stop request the VCM sends 10.24 s after a wake without a usable EVSE (or with a charge timer pending) and at every power-off. Mask with `0x60` — this car also sets d[2] bits 3 and 7.

5. **J1772 current clamp ceiling**: The derivation formula clamps at 32 A (single Lear OBC max). Confirm this is the correct ceiling for this installation.

---

## Out of Scope

- CHAdeMO DC fast charge (`0x5BF` status `0xB0` / QUICK_CHARGING)
- Drive mode / traction torque demand (`0x1D4` torque fields not decoded)
- VCM ALU security handshake (`0x50C` / `0x55B d[2]`)
- Thermal protection fault (OBC temperature on `0x274` — not decoded)
