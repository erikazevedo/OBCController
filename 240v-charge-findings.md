# 120 V vs 240 V charging — test findings (2026-11-01)

Summary of the first 240 V EVSE tests with the bridge, what failed, what was fixed, and what the Lear's CAN traffic revealed about its AC input voltage.

## Summary

- A 240 V Grizzl-E Classic first faulted at plug-in ("diode error", 9 beeps) because the bridge's CP circuit used a **shunt** clamp diode. Moving the existing diode **in series** fixed it. A second 240 V run charged successfully.
- The Lear reports its **AC input voltage** in two places: `0x27D` d[0..1] and `0x615` d[6]. The two agree in every run, and read near 0 when no AC is present (brief blips of up to 13.5 V around charge-contactor switching). Neither is documented in the DBC.
- Two fields previously suspected of carrying voltage/current do **not**: `0x618` d[5..6] is the Lear's output power limit in watts (set before current flows; DC V × I settles on it), and `0x618` d[0..1] is DC output current (consistent with 1/32 A), not AC current.
- The firmware still assumed 120 V (`SM_EVSE_AC_NOMINAL_V 120`) during the 240 V charge. It worked, and nothing over-delivered, because the VCM's command reached 3300 W (via 100 W and 2000 W, within 0.5 s) 2–3 s before the Lear delivered any current. The error is exposed whenever the VCM commands less than about 3 kW while current flows. *Since fixed: the firmware uses the measured voltage (§6).*
- The firmware's "AC present" test (any Lear frame after State C) gave a false positive in the failed run, and in the 120 V run it fired while the Lear still reported 0 V. *Since fixed: AC present now needs a measured AC voltage (§6).*

## Test runs

All runs used the same build (`02ea4e7-dirty`, compiled Nov 1 2026 09:49:58, `SM_EVSE_AC_NOMINAL_V 120`, `SM_LOG_LEAR_RAW 1`). The first 240 V log starts mid-boot, so its build header is missing; its behaviour (raw `lear` lines, a 0.8 A limit for 100 W) matches.

| Run | Log (`firmware/main/`) | EVSE | Outcome |
|---|---|---|---|
| 120 V | `120vcharge` | 120 V, pilot 15.1 A | Charged. DC 386.7 V × 3.2 A ≈ 1.24 kW (VCM command 1800 W, bridge AC limit 15.0 A). Firmware "AC at the Lear" 1265 ms after State C, but the Lear's first frames read 0 V; 124 V first appears 2.3 s after State C. |
| 240 V, first | `240vcharge` | Grizzl-E Classic, pilot 32.0 A | EVSE faulted at plug-in, **no AC reached the Lear**. Firmware still declared "AC at the Lear" (1988 ms) and ran to "charging" with 0.0 A, then the Lear went silent and the charge was stopped. |
| 240 V, second (after hardware fix) | `240vcharge_2` | Grizzl-E Classic, pilot 32.0 A | Charged. DC 387.8–388.3 V × 7.8 A ≈ 3.0 kW, flat for about 57 s. Firmware "AC at the Lear" 2199 ms after State C; the Lear's first frame already read 246 V. Clean stop on connector button / unplug. |

These logs are untracked files in `firmware/main/`.

## 1. Hardware: J1772 diode check

**Cause.** A J1772 EVSE expects a diode in **series** with the vehicle's pilot resistors, so the negative half of the pilot stays at about −12 V. The bridge had a **shunt** diode (`D_CLAMP`, anode PE, cathode CP) that held CP at about −0.7 V. The 240 V EVSE detected this and refused to close its contactor. The 120 V EVSE did not check it.

**Fix (Rev B).** The existing 1N4148WS was relocated as a series diode `D_SER` (anode J1 CP, cathode node X). `R_CP1`, `R_CP2` with Q1, and `R_DIV_TOP` all connect to node X, behind `D_SER`. `R_DIV_TOP` stays 33 kΩ.

**Status and margin.**
- Not yet verified with a scope; check TP1 (J1 CP), TP2 (node X) and TP3 (GPIO0).
- State C at GPIO0 falls from about 2.79 V to about 2.6 V nominal (about 2.5 V with a low EVSE). The BOM quotes V_IH(min) = 2.31 V; if the datasheet value is 0.75 × VDD = 2.475 V (not verified), the low-EVSE margin is about 0.01 V.
- **Planned:** `R_DIV_TOP` = 22 kΩ (or 33 kΩ ‖ 68 kΩ) for about 3.1 V. Not fitted yet.
- `D_OVP` / `D_UVP` keep GPIO0 within the supply rails either way. The margin concern is signal detection, not pin damage.

Documents updated in the LeafOBCBridge repo (`hardware/`): `J1772-CP-Signal-Conditioning.net` (Rev B), `J1772-CP-BOM.md`, `Full-BOM.md`, `Hardware-Plan.md`, `Hardware-Testing.md` (new §7a), and the diagram `J1772-CP-Rework.svg`.

## 2. Lear AC input voltage

| Signal | Decode | 120 V run | 240 V run (second) | No AC (first 240 V run) |
|---|---|---|---|---|
| `0x27D` d[0..1] (big-endian) | raw ÷ 32 V (0.5 V steps; low nibble always 0) | 124–125 V idle, 113–114 V loaded (117.5–118.5 V at turn-on) | 242–251 V before current flows, 240–240.5 V loaded | 0 logged; one transient of up to 13.5 V at contactor close |
| `0x615` d[6] | value × 2 V | 124–126 V idle, 114–118 V loaded | 242–248 V idle, 240–242 V loaded | 0–12 V (12 V at contactor close) |

- They agree to within 1 V wherever they were logged close together while AC was on (all of the 120 V run, the first 240 V frame), and to within 1.5 V under load at 240 V. At 240 V idle the samples are about 0.6 s apart and differ by up to 7 V, because the voltage itself wandered (`0x27D` went 244 → 251 → 244 V).
- After AC is removed both decay toward 0 (for example `0x27D` 37 V, then 0).
- **Confidence:** good enough to tell 120 V from 240 V and to detect "AC present" with a threshold well above the 13.5 V contactor blips. Only two voltage points exist, so treat the scale as unproven for metering.
- Both fields are undefined in the DBC and in `twai1-can-frames.instructions.md`.

## 3. Fields that are *not* AC voltage

| Signal | Observation |
|---|---|
| `0x618` d[5..6] | Output power **limit** in watts: 1245 W and 3045 W. Set while DC current was still 0 and bit-for-bit constant for the whole session; DC V × I settles on it (386.7 V × 3.2 A = 1237 W, 388.3 V × 7.8 A = 3029 W). |
| `0x618` d[0..1] | DC output current, consistent with 1/32 A: 102–103 (3.19–3.22 A) at 120 V, 250–252 (7.81–7.88 A) at 240 V, matching d[5..6] ÷ DC volts (3.22 A, 7.84 A). That is about 3.2 × the 0.1 A DC current count in **both** runs; for an AC current that ratio would roughly halve at 240 V, so it is not a measured AC current. Note: a ×0.1 A or ×(1/18.25) A scale for `Lear_ACCurrent` cannot be settled from this field. |
| `0x279` d[3] | Roughly equal to `0x618` d[1] low byte (102–103 at 120 V, 248–249 at 240 V). Same quantity. |
| `0x615` d[2], d[3..4] | d[2] = DC current in 0.1 A (32, then 78). d[3..4] = 4000 = 400.0 V, the target voltage echo. |
| `0x619` | Same values at both voltages (`02 02 00 00 00 02 33 00`, then `04 02 01 …` once the Lear is enabled). The failed run also switched to `04 02 01 …` with no AC, so it follows the enable command, not power flow. |
| `0x277` d[2..7] | Slowly rising 16-bit values after start, falling after stop; probably temperatures (unconfirmed). |

Earlier working hypotheses that the data does **not** support: `0x618` d[5..6] as a voltage, and `0x618` d[0..1] as AC current at ×0.1 A.

## 4. Bridge behaviour at 240 V with `SM_EVSE_AC_NOMINAL_V 120`

| Item | Observed / derived |
|---|---|
| Lear AC current limit sent (`0x050`) | 27.5 A (echo `0x616` = `0x0113`): min(pilot 32 A, 3300 W ÷ 120 V, VCM command ÷ 120 V). |
| VCM maximum charge power | 100 W, then 2000 W, then 3300 W within 0.5 s, and unchanged for the rest of the session. |
| Power actually delivered | About 3.0 kW DC, flat at 7.8 A. This looks like the Lear's own ceiling: its power limit (`0x618` d[5..6] = 3045 W) was set before any current flowed, while the bridge allowed 27.5 A. The cause is unconfirmed. |
| `0x380` AC voltage / type | 120 V and the 100 V-class bit by firmware construction (TWAI0 TX content is not logged). The VCM raised no objection. |

At 120 V the commanded limit converts to the right current. At 240 V the same limit allows twice the power. This had no effect here: the 100 W and 2000 W commands lasted only 0.5 s, and the Lear delivered no current until 2–3 s after the command reached 3300 W. If the VCM commands, say, 2000 W (16.7 A) while current flows, the Lear may draw up to about 3.3 kW. Fixed by using the measured voltage (§6).

## 5. Other observations

- No warnings or errors in the successful run after boot. The boot-time messages (SPI flash size warning, TWAI1 bus-off and a dropped `0x050` before the bus is up) are the same as in the 120 V run.
- The firmware's "AC at the Lear" times (1265 ms at 120 V, 2199 ms at 240 V, 1988 ms with no AC) mark when the Lear started broadcasting, not when AC arrived. At 120 V the Lear's first frames read 0 V and 124 V first appears 2.3 s after State C; at 240 V the first frame (2.2 s) already read 246 V. The logs therefore do not show which EVSE energised sooner. All are well inside `SM_LEAR_AC_TIMEOUT_MS` (8000 ms).
- The Lear broadcasts from its 12 V supply, so "Lear alive" does not mean AC is present. In the failed run it did not, and the firmware went on to "charging" with 0.0 A for about 31 s. The Lear stopped transmitting at about 51.2 s; `0x050` then went unacknowledged (`twai1 err 0x10` = ACK error) and filled the TX queue from 52.2 s, and the firmware declared "Lear silent" at 53.7 s.
- The pilot reads exactly 32.0 A at 240 V, which is the ceiling in `derive_j1772_current`; the EVSE may be offering more.

## 6. Firmware and docs changed in this effort (OBCController)

- `SM_LOG_LEAR_RAW` (`sm_config.h`, default 1): receives every TWAI1 ID and logs a Lear frame when its payload changed, at most once per ID per second (`SM_LOG_LEAR_RAW_MIN_MS`), with the OR of the bits that flipped since the previous line (tag `lear`). The value ranges above come from these samples. Extra IDs are logged only; they never count as "Lear alive".
- Touched: `twai_handler.c/.h` (open filter, core-ID check, queue guard), `rx_dispatcher.c`, `state_machine.h`.
- Instruction files updated: `state-machine`, `twai-config`, `twai1-can-frames` (includes the capture procedure).
- Follow-up (measured AC voltage): `SM_EVSE_AC_NOMINAL_V` and `SM_AC_TYPE_MASK` are removed. The firmware decodes `0x27D` (now a core TWAI1 ID) and:
  - reports the reading in `0x380`, updated live as the stock OBC does, with the class bit from `SM_AC_200V_CLASS_MIN_V` (160 V) 200 ms after the first reading;
  - converts watts to Lear AC amps at the measured voltage; the chargeable power is `SM_MAX_CHARGE_POWER_W` until AC is measured;
  - declares AC present only at ≥ `SM_AC_PRESENT_MIN_V` (90 V), and stops with "AC lost at the Lear" if no such reading arrives for `SM_LEAR_ALIVE_MS` after that.
- `twai1-can-frames.instructions.md` documents `0x27D` and the corrected `0x618`; `state-machine`, `twai-config`, `twai0-can-frames` and `task-architecture` instructions are updated to match.

## 7. Open items

- [ ] Scope TP1–TP3 on the reworked CP circuit with the EVSE connected (negative level near −12 V, State C at GPIO0 as estimated).
- [ ] Fit `R_DIV_TOP` = 22 kΩ, or confirm V_IH(min) from the ESP32-C6 datasheet.
- [x] Use the measured AC voltage in the firmware: set `0x380` voltage and class bit, convert watts to amps, and require AC above about 90 V before declaring "AC present".
- [x] Update `twai1-can-frames.instructions.md` with the findings in sections 2 and 3.
- [ ] Capture more voltage points (for example 208 V or a different EVSE) to validate the scale.
- [ ] Decide whether the 32 A pilot ceiling should be raised.
