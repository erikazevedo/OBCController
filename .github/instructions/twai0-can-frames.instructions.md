---
applyTo: "**/twai*"
---
# Nissan LEAF ZE0 EV-CAN Frame Specification

**Bus:** EV-CAN | **Baud rate:** 500 kbps | **ID format:** Standard 11-bit | **Model years:** 2011–2012 (ZE0, 24 kWh)

> **Source of the bit maps:** a real ZE0 AC-charge capture checked frame by frame against 15 no-plug captures, the 2012 LEAF service manual and the DBC — see `LeafOBCBridge/docs/OBC-VCM-State-Sequence.md` §5. Fields marked *(inferred)* are consistent in every capture but undocumented. The encoders and decoders live in `firmware/main/obc_frames.h`.

---

## Table of Contents

| Section | CAN ID | Direction | Name |
|---------|--------|-----------|------|
| [1](#1-shared-crc-8-integrity-algorithm) | — | — | Shared CRC-8 Integrity Algorithm |
| [2](#2-0x380--obc-status-tx) | 0x380 | TX | OBC Status |
| [3](#3-0x5bf--obc-charge-state-tx) | 0x5BF | TX | OBC Charge State |
| [4](#4-0x1db--lbc-voltage--current--failsafe-rx) | 0x1DB | RX | LBC Voltage / Current / Failsafe |
| [5](#5-0x1dc--lbc-power-limits-rx) | 0x1DC | RX | LBC Power Limits |
| [6](#6-0x1f2--vcm-charge-command-rx) | 0x1F2 | RX | VCM Charge Command |
| [7](#7-0x1d4--vcm-torque-request--charge-status-rx) | 0x1D4 | RX | VCM Torque Request / Charge Status |
| [8](#8-0x50b--vcm-wake--sleep-command-rx) | 0x50B | RX | VCM Wake / Sleep Command |
| [9](#9-0x55b--lbc-state-of-charge-rx) | 0x55B | RX | LBC State of Charge |
| [10](#10-0x5bc--lbc-gids--capacity--soh-rx) | 0x5BC | RX | LBC GIDS / Capacity / SOH |
| [11](#11-0x6c0--0x6c1--custom-display-settings) | 0x6C0 / 0x6C1 | RX / TX | Custom display settings (not a stock ZE0 frame) |
| [12](#12-0x1da--inverter-hv-bus-voltage-rx) | 0x1DA | RX | Inverter — HV bus voltage |
| [13](#13-0x603--0x68c--power-switch-wake-markers-rx) | 0x603 / 0x68C | RX | Power-switch wake markers |

---

## 1. Shared CRC-8 Integrity Algorithm

Applies to received messages: **0x1DA, 0x1DB, 0x1DC, 0x55B, 0x1D4** (verified on every frame of the AC-charge capture; on 0x50C it covers d[0..4] only)

| Property | Value |
|---|---|
| Polynomial | 0x85 |
| Bit order | Left-shift, D0-first |
| Input | d[0] through d[6] (7 bytes) |
| Initial value | 0x00 |
| Final XOR | None |
| Output location | d[7] |
| Implementation | 256-entry pre-computed lookup table |

---

## 2. 0x380 — OBC Status *(TX)*

| Property | Value |
|---|---|
| CAN ID | 0x380 |
| Common name | OBC_Status |
| DLC | 8 |
| Transmitting node | OBCpd (simulated by bridge) |
| Period | 100 ms |
| Integrity | Nibble CSUM in d[7][3:0] |

### Byte Layout

| Byte | Bits | Field | Encoding Rule |
|------|------|-------|---------------|
| d[0] | [7:5] | Reserved | 0 |
| d[0] | [4] | AC input type, 200 V class | 1 while 200 V-class AC is present (FSM "AC input type signal") |
| d[0] | [3] | AC input type, 100 V class *(inferred)* | 1 while 100 V-class AC is present (`SM_AC_TYPE_MASK`) |
| d[0] | [2] | Reserved | 0 |
| d[0] | [1] | Base bit | 1 in every frame except the first after an OBC start (`0x00` then `0x02`) |
| d[0] | [0] | Stop request acknowledged *(inferred)* | 1 together with d[2] mode = standby |
| d[1] | [7:0] | Fixed | `0x04` |
| d[2] | [7:6] | Reserved | 0 |
| d[2] | [5:4] | Mode *(inferred)* | `01` normal, `10` standby (stop request acknowledged) |
| d[2] | [3] | EVSE ready *(inferred)* | 1 = EVSE announced (plug in, pilot valid); never set in a power-button wake |
| d[2] | [2] | Connector release switch pressed *(inferred)* | 1 while the handle button is pressed |
| d[2] | [1] | Reserved | 0 |
| d[2] | [0] | Chargeable power bit 8 | See below |
| d[3] | [7:0] | Chargeable power bits [7:0] | 9-bit value × 0.1 kW (DBC `Charger_Output_Power`, FSM "on-board charger chargeable power") |
| d[4] | [7] | Reserved | 0 |
| d[4] | [6] | Normal-charge relay closed | 1 = charge contactors closed |
| d[4] | [5] | Quick-charge relay closed | 0 (CHAdeMO only) |
| d[4] | [4] | S2 closed / State C *(inferred)* | 1 while State C is asserted |
| d[4] | [3:0] | Output power bits [8:5] *(inferred)* | 9-bit value × 0.1 kW, LSBs in d[5] |
| d[5] | [7:3] | Output power bits [4:0] *(inferred)* | |
| d[5] | [2:0] | AC voltage bits [8:6] | 9-bit raw; V = raw × 0.5 + 70; raw 0 = no AC (DBC) |
| d[6] | [7:2] | AC voltage bits [5:0] | |
| d[6] | [1:0] | Reserved | 0 |
| d[7] | [7] | Unknown | Set ≈ 0.5 s after the relay closes, cleared with the stop acknowledge |
| d[7] | [6] | Reserved | 0 |
| d[7] | [5:4] | PRUN | 2-bit rolling counter — see PRUN procedure |
| d[7] | [3:0] | CSUM | Nibble checksum — see CSUM procedure |

**Chargeable power encoding:** 9 bits × 0.1 kW — bit 8 in d[2][0], bits 7:0 in d[3] (3.3 kW → `0x21`). The stock OBC sends 0 between the normal-charge request and the end of its relay sequence, then re-advertises the full value.

**Observed sequences (stock OBC):** start `00 04 10 21` → `02 04 10 21`; announce `02 04 18 21` (EVSE ready) → `02 04 18 21 10` (S2); charging `12 04 18 21 51 0C C0`; release switch `12 04 14 21 …`; stop acknowledged `03 04 20 21 00 00 00`.

### PRUN Procedure (executed once per 100 ms tick, before CSUM)

1. Increment the PRUN counter: `counter = (counter + 1) mod 4` — values cycle 0, 1, 2, 3.
2. Clear d[7] bits [5:4], then write `counter` into d[7][5:4].
3. PRUN must be written **before** the CSUM is calculated.

> The counter initialises to 0. The first value transmitted on the bus is therefore **1**.

### CSUM Procedure (executed after PRUN, once per 100 ms tick)

1. Accumulate the sum `S` of all nibbles of d[0] through d[6] — that is, the high nibble plus the low nibble of each of the 7 bytes (14 nibbles total).
2. Add the high nibble of d[7] (which contains the PRUN value and reserved flag bits) to `S`.
3. Write `(S − 5) & 0x0F` into d[7] bits [3:0].

---

## 3. 0x5BF — OBC Charge State *(TX)*

| Property | Value |
|---|---|
| CAN ID | 0x5BF |
| Common name | OBC_ChargeState |
| DLC | 8 |
| Transmitting node | OBCpd (simulated by bridge) |
| Period | 100 ms |
| Integrity | Nibble CSUM in d[7][3:0] |

### Byte Layout

| Byte | Bits | Field | Encoding Rule |
|------|------|-------|---------------|
| d[0] | [7:0] | Reserved | Always 0x00 |
| d[1] | [7:0] | Unknown | ≈ `0x65` while AC is present, otherwise 0 |
| d[2] | [7:0] | J1772 pilot current limit | `amps × 5` — e.g. 18 A → 90 (0x5A). Keeps its last value after unplug |
| d[3] | [7:0] | DC output voltage *(inferred)* | `volts − 263` (matches the LBC pack voltage within ±1 V over 2,460 frames); 0 when the relay is open |
| d[4] | [7:0] | Charger status | Enumerated — see Charger Status table |
| d[5] | [7:0] | Reserved | Always 0x00 |
| d[6] | [7:3] | DC output current *(inferred)* | Whole amps |
| d[6] | [2:0] | Reserved | 0 |
| d[7] | [7:6] | Reserved | Always 0 |
| d[7] | [5:4] | PRUN | 2-bit rolling counter — see PRUN procedure |
| d[7] | [3:0] | CSUM | Nibble checksum — see CSUM procedure |

### Charger Status Enum (d[4])

All values are multiples of 8. Names below replace the revision-1 names (`TIMER_WAIT2`, `PLUG_BEEP`, `TIMER_WAIT`, `FINISHED`, `CHARGE_START`), which were wrong.

| Value | When (stock OBC) |
|-------|------------------|
| 0x10 | First frame of every OBC start, with or without a plug |
| 0x18 | 100 ms later, every start — not a plug or beep event |
| 0x20 → 0x28 | Power-button wake, no charge (0x28 = idle) |
| 0x30 | Stop request acknowledged (charge standby; also at every power-off) |
| 0x38 | After GoToSleep |
| 0x40 | Normal-charge request received |
| 0x48 | Normal-charge relay closed |
| 0x50 → 0x58 | Charge start-up steps |
| 0x60 | Charging |
| 0x70 | End of charge after the VCM stop request |
| 0x78 | Relay open after the end of charge |
| 0x80 | OBC-initiated stop (connector release switch) |
| 0xB0 | Quick charging (OVMS; not in the ZE0 captures) |

### PRUN Procedure (executed once per 100 ms tick, before CSUM)

1. Increment the PRUN counter: `counter = (counter + 1) mod 4` — values cycle 0, 1, 2, 3.
2. Clear d[7] bits [5:4], then write `counter` into d[7][5:4].
3. PRUN must be written **before** the CSUM is calculated.

> The counter initialises to 0. The first value transmitted on the bus is therefore **1**.

### CSUM Procedure (executed after PRUN, once per 100 ms tick)

Verified against every frame in ZE0 AC-charge captures.

1. Accumulate the sum `S` of all nibbles of d[0] through d[6] (14 nibbles total).
2. Add the high nibble of d[7] (which contains the PRUN value) to `S`.
3. Write `(S − 1) & 0x0F` into d[7] bits [3:0].

> The PRUN nibble **must** be included in step 2. The subtraction offset for 0x5BF is **−1**, which differs from 0x380 (offset **−5**).

---

## 4. 0x1DB — LBC Voltage / Current / Failsafe *(RX)*

| Property | Value |
|---|---|
| CAN ID | 0x1DB |
| Common name | LBC_VoltCurr |
| DLC | 8 |
| Transmitting node | HVBAT (LBC) |
| Period | 10 ms |
| Integrity | CRC-8 (poly 0x85) in d[7] — see [Section 1](#1-shared-crc-8-integrity-algorithm) |

### Byte Layout

| Byte | Bits | Field | Decoding Rule |
|------|------|-------|---------------|
| d[0] | [7:0] | Battery current bits [10:3] | Combined with d[1][7:5] to form 11-bit signed value |
| d[1] | [7:5] | Battery current bits [2:0] | Combined with d[0] |
| d[1] | [4:3] | Relay cut request | 0b00 = no request; any non-zero = Main Relay OFF request |
| d[1] | [2:0] | Failsafe status | 3-bit field — see Failsafe Status table |
| d[2] | [7:0] | Battery voltage bits [9:2] | Combined with d[3][7:6] to form 10-bit unsigned value |
| d[3] | [7:6] | Battery voltage bits [1:0] | Combined with d[2] |
| d[3] | [5] | Main relay on flag | 1 = main relay on (LB_MainRelayOn) |
| d[3] | [4] | Full charge flag | `LB_Full_CHARGE_flag` (DBC); 0 in all 28,595 frames of the AC-charge capture (SOC 74.5 %). Not decoded |
| d[3] | [3] | HV interlock | 1 = interlock chain closed (required before relay-on is granted) |
| d[4] | [6:0] | Instrument SOC | 0–100; value 0x7F = invalid (not ZE0 pack) |
| d[6] | [1:0] | PRUN | 2-bit rolling counter |
| d[7] | [7:0] | CRC-8 | See Section 1 |

### Battery Current Decoding

1. Form the raw 11-bit field: bits [10:3] from d[0], bits [2:0] from d[1][7:5].
2. Sign-extend from 11 bits to a signed integer: if bit 10 of the raw field is set, set the upper 5 bits to 1.
3. Physical current (A) = −(raw_signed × 0.5).
4. Sign convention: **positive = battery discharging (output); negative = battery charging (input)**.

### Battery Voltage Decoding

1. Form the raw 10-bit field: bits [9:2] from d[2], bits [1:0] from d[3][7:6].
2. Physical voltage (V) = raw × 0.5.

### Failsafe Status Values (d[1][2:0])

| Bits | Meaning |
|------|---------|
| 0b000 | Normal |
| 0b001 | Normal Stop Request |
| 0b010 | Charging Mode Stop Request |
| 0b011 | Charging Mode Stop Request + Normal Stop Request |
| 0b100 | Caution Lamp Request |
| 0b101 | Caution Lamp Request + Normal Stop Request |
| 0b110 | Caution Lamp Request + Charging Mode Stop Request |
| 0b111 | Caution Lamp Request + Charging Mode Stop Request + Normal Stop Request |

---

## 5. 0x1DC — LBC Power Limits *(RX)*

| Property | Value |
|---|---|
| CAN ID | 0x1DC |
| Common name | LBC_PowerLim |
| DLC | 8 |
| Transmitting node | HVBAT (LBC) |
| Period | 10 ms |
| Integrity | CRC-8 (poly 0x85) in d[7] — see [Section 1](#1-shared-crc-8-integrity-algorithm) |

### Byte Layout

| Byte | Bits | Field | Decoding Rule |
|------|------|-------|---------------|
| d[0] | [7:0] | Discharge power limit bits [9:2] | Combined with d[1][7:6] |
| d[1] | [7:6] | Discharge power limit bits [1:0] | Combined with d[0] |
| d[1] | [5:0] | Regen/charge power limit bits [7:2] | Combined with d[2][7:4] |
| d[2] | [7:4] | Regen/charge power limit bits [1:0] | Combined with d[1][5:0] |
| d[2] | [3:0] | Max charger power bits [9:6] | Combined with d[3][7:2] |
| d[3] | [7:2] | Max charger power bits [5:0] | Combined with d[2][3:0] |
| d[3] | [1:0] | Charge power status | `LB_Charge_Power_Status` (DBC); `3` for ≈ 0.7 s after the LBC wakes, then `1` (AC-charge capture and this car). Not decoded |
| d[7] | [7:0] | CRC-8 | See Section 1 |

### Field Decoding

**Max discharge power (kW):**
1. Raw = (d[0] << 2) OR (d[1] >> 6) — 10-bit unsigned.
2. Physical (kW) = raw ÷ 4.0.

**Max regen / charge-input power (kW):**
1. Raw = ((d[1] AND 0x3F) << 2) OR (d[2] >> 4) — 10-bit unsigned.
2. Physical (kW) = raw ÷ 4.0.

**Max charger power — LB_MAX_POWER_FOR_CHARGER / LB_BPCMAX (kW):**
1. Raw = ((d[2] AND 0x0F) << 6) OR (d[3] >> 2) — 10-bit unsigned.
2. Physical (kW) = raw ÷ 10.0.

> **Critical distinction:** `LB_MAX_POWER_FOR_CHARGER` is the battery's OBC charge power ceiling. The regen/charge-input power field (`LB_Charge_Power_Limit`) reflects the regen braking limit, which may reach ~102 kW — it is **not** the OBC charge limit and must not be used to gate OBC output power.

---

## 6. 0x1F2 — VCM Charge Command *(RX)*

| Property | Value |
|---|---|
| CAN ID | 0x1F2 |
| Common name | VCM_ChargeCmd |
| DLC | 8 |
| Transmitting node | VCM |
| Period | 10 ms |
| Integrity | Nibble CSUM in d[7][3:0]: (Σ nibbles of d[0..6] + high nibble of d[7] + 0x1 + 0xF + 0x2) & 0xF — verified on 28,024 frames |

### Byte Layout

| Byte | Bits | Field | Decoding Rule |
|------|------|-------|---------------|
| d[0] | [7] | `TargetChargeSOC` (DBC) | 1 = charge to 80 % ("deterioration restraint"), 0 = 100 % |
| d[0] | [6:0] | VCM state *(inferred)* | `0x08` HV bus not live, `0x10` HV live, `0x30` charging with a power command, `0x70` → `0x60` → `0x68` shutdown / standby |
| d[1] | [7:0] | Maximum charge power *(inferred)* | (raw − 100) × 0.1 kW: `0x64` = 0 kW, `0x85` = 3.3 kW (FSM "maximum charge power signal"); ramps 0 → 3.3 kW when charging starts |
| d[2] | [6:5] | `Charge_StatusTransitionReqest` | `00` none, `01` **normal charge** (`0x20`), `10` quick charge, `11` **stop request** (`0x60`). **Decode with `d[2] & 0x60`** |
| d[2] | [7], [3] | Unknown | Set on this car (`0x08`, `0x88`, `0xE8`, `0x68` seen in `chargtest.log`); never set on the GVRET reference car |
| d[5] | [7:0] | VCM MODE *(sequence step)* | CONSULT "VCM MODE" (FSM EVC-56): 0–7 start-up, 10 power ON / no charge, 11–29 to READY, 30 READY, 31–68 shutdown, 109–129 entering charge mode, 130 charging, 131–144 shutdown after charge, 211–246 12 V / remote charge |
| d[6] | [1:0] | MPRUN | 2-bit rolling counter |
| d[7] | [3:0] | CSUM | See Integrity |

### Request Values (`d[2] & 0x60`)

| Value | Meaning | When |
|-------|---------|------|
| 0x00 | No request | Start-up, waiting |
| 0x20 | Normal charge | From the VCM's charge-path decision until the end of charge |
| 0x40 | Quick charge | CHAdeMO only |
| 0x60 | **Stop request** | 10.24 s after a wake without a usable EVSE (or with a charge timer pending), at the end of every charge and at every power-off |

> **Corrections:** `0x60` is not "CHARGING_IDLE" or a pilot handshake, and `0x08` / `0x68` / `0x88` / `0xE8` are not authorisation codes — bits 6:5 alone carry the request.

---

## 7. 0x1D4 — VCM Torque Request / Charge Status *(RX)*

| Property | Value |
|---|---|
| CAN ID | 0x1D4 |
| Common name | VCM_TorqChg |
| DLC | 8 |
| Transmitting node | VCM |
| Period | 10 ms |
| Integrity | CRC-8 (poly 0x85) in d[7] — see [Section 1](#1-shared-crc-8-integrity-algorithm) |

### Byte Layout

| Byte | Bits | Field | Decoding Rule |
|------|------|-------|---------------|
| d[0–3] | — | Torque / motor demand fields | Not decoded |
| d[4] | [2] | `StatusOfHighVoltagePowerSupply` | 1 = HV bus live (main relays closed, precharge done) |
| d[5] | [6] | `Relay_Plus_Output_Status` | 1 = VCM drives the system main relay + |
| d[6] | [7:0] | Charge status | See Charge Status Values |
| d[7] | [7:0] | CRC-8 | See Section 1 |

### Charge Status Values (d[6])

| Value | When |
|-------|------|
| 0x40 → 0x60 | Charge wake (power-button wake: 0x50 → 0x70 → …) |
| 0xE0 | Charge session active (bit 7) — from the EVSE announcement until the end of charge |
| 0x60 | After the charge session |
| 0x00 – 0xB0 | Varies while driving |

> **`0x8C` ("charging interrupted" in the DBC) never appears in 16 ZE0 captures.** Use the `0x1F2` stop request, the power command returning to 0 and d[6] bit 7 clearing to detect the end of a session.

---

## 8. 0x50B — VCM Wake / Sleep Command *(RX)*

| Property | Value |
|---|---|
| CAN ID | 0x50B |
| Common name | VCM_WakeSleep |
| DLC | 7 (ZE0) |
| Transmitting node | VCM |
| Period | 100 ms |
| Integrity | None documented |

### Byte Layout

| Byte | Bits | Field | Decoding Rule |
|------|------|-------|---------------|
| d[0–1] | — | Not decoded | — |
| d[2] | [7:0] | Power-switch state | `0x00` = power switch OFF — **including the whole of a charge wake**; `0x02` (+1.5 s) → `0x06` (+3.6 s) after power ON; `0x04` is 2013+ only |
| d[3] | [7:6] | `VCM_WakeUpSleepCmd` | `11` = WakeUp (VCM awake), `00` = GoToSleep |
| d[3] | [5:0] | Not decoded | — |
| d[4] | — | Not decoded | — |
| d[5] | [7:0] | Power switch ON *(inferred)* | `0x60` while the power switch is ON, otherwise `0x00` |

> **A charge wake is visible only in d[3].** During charging with the power switch OFF the frame stays `00 00 00 C0 00 00` (2,793 frames in the capture). Firmware that treats d[2] = 0 as "VCM asleep" stays silent through a charge wake. OBCController uses d[2] only to tell that the power switch is ON (POWER_ON state) and d[3] for the VCM's wake / sleep state.

> DLC is 6 on the 2011–12 ZE0. Minimum required DLC to decode: 4 bytes (d[0]–d[3]).

---

## 9. 0x55B — LBC State of Charge *(RX)*

| Property | Value |
|---|---|
| CAN ID | 0x55B |
| Common name | LBC_SOC |
| DLC | 8 |
| Transmitting node | HVBAT (LBC) |
| Period | 100 ms |
| Integrity | CRC-8 (poly 0x85) in d[7] — see [Section 1](#1-shared-crc-8-integrity-algorithm) |

### Byte Layout

| Byte | Bits | Field | Decoding Rule |
|------|------|-------|---------------|
| d[0] | [7:0] | SOC bits [9:2] | Combined with d[1][7:6] to form 10-bit value |
| d[1] | [7:6] | SOC bits [1:0] | Combined with d[0] |
| d[2] | [7:0] | ALU security answer | LBC response to VCM security challenge from 0x50C |
| d[4] | [7:1] | Battery SOH | `d[4] >> 1` — direct percentage (ZE0 only) |
| d[6] | [7] | Capacity empty flag | 1 = battery empty |
| d[7] | [7:0] | CRC-8 | See Section 1 |

### SOC Decoding

1. Raw = (d[0] << 2) OR (d[1] >> 6) — 10-bit unsigned; range 0–1000.
2. Physical SOC (%) = raw ÷ 10.0 — range 0.0–100.0 %.

### ALU Security Answer (d[2])

| VCM challenge value (from 0x50C d[4]) | Expected LBC answer |
|---------------------------------------|---------------------|
| 0xB2 | 0xAA |
| 0x5D | 0x55 |

---

## 10. 0x5BC — LBC GIDS / Capacity / SOH *(RX)*

| Property | Value |
|---|---|
| CAN ID | 0x5BC |
| Common name | LBC_GIDS |
| DLC | 8 |
| Transmitting node | HVBAT (LBC) |
| Period | 500 ms |
| Integrity | None — d[7] contains data, no CRC |

### Byte Layout

| Byte | Bits | Field | Decoding Rule |
|------|------|-------|---------------|
| d[0] | [7:0] | GIDS bits [9:2] | Combined with d[1][7:6] to form 10-bit value |
| d[1] | [7:6] | GIDS bits [1:0] | Combined with d[0] |
| d[3] | [7:0] | Average battery temperature | `(signed byte) d[3] − 40` = °C |
| d[4] | [7:1] | SOH % | `(d[4] >> 1) AND 0x7F` — valid range 0–100 (ZE0 only) |
| d[4] | [0] | ZE0 battery type flag | 1 = ZE0 24 kWh pack |
| d[5] | [7:5] | Output power limit reason | 3-bit field — see Power Limit Reason table |
| d[5] | [4:0] | Charge time mux index bits [4:0] | Combined with d[6][7:5] |
| d[6] | [7:5] | Charge time mux index bits (LSBs) | Combined with d[5][4:0] |
| d[6] | [4:0] | Charge time value bits [12:8] | Combined with d[7] |
| d[7] | [7:0] | Charge time value bits [7:0] | Combined with d[6][4:0] |

### Field Decoding

**GIDS (remaining capacity in 80 Wh units):**
1. Raw = (d[0] << 2) OR ((d[1] AND 0xC0) >> 6) — 10-bit unsigned.

**Charge time mux index (5-bit):**
1. Raw = ((d[5] << 3) OR (d[6] >> 5)) AND 0x1F.

**Charge time value (minutes, 13-bit):**
1. Raw = ((d[6] << 8) OR d[7]) AND 0x1FFF.
2. Value 0x1FFF = unavailable.

### Power Limit Reason Values (d[5][7:5])

| Value | Meaning |
|-------|---------|
| 0b000 | Normal |
| 0b001 | Capacity drop |
| 0b010 | LBC malfunction |
| 0b011 | High temperature |
| 0b100 | Low temperature |
| 0b101 | Reserved |
| 0b110 | Reserved |
| 0b111 | Reserved |

### Charge Time Mux Index Values (ZE0)

| Index | Meaning |
|-------|---------|
| 0 | Quick charge remaining time |
| 9 | Normal 200 V charge to 100% SOC |
| 10 | Normal 200 V charge to 80% SOC |
| 17 | Normal 100 V charge to 100% SOC |
| 18 | Normal 100 V charge to 80% SOC |
| 31 | Invalid |

---

## 11. 0x6C0 / 0x6C1 — Custom Display Settings

Not part of the stock ZE0 EV-CAN bus (not seen in any captured log or DBC). Both frames share one layout, DLC 8:

| Byte | Content |
|------|---------|
| d[0] | Target voltage, raw × 2 V (default 392 V = 196) |
| d[1] | Target SOC, 1 % per bit |
| d[2..6] | Reserved, 0 |
| d[7] | Checksum = `0xA5 ^ d[0] ^ d[1] ^ ... ^ d[6]` |

- **0x6C0 (RX, display → controller):** overwrites `g_sm_target_voltage_v` / `g_sm_target_soc_pct`. Dropped whole (previous values kept) if DLC ≠ 8, checksum wrong, a reserved byte ≠ 0, or a value is outside `SM_TARGET_VOLTAGE_MIN_V`–`MAX_V` / `SM_TARGET_SOC_MIN_PCT`–`MAX_PCT`.
- **0x6C1 (TX, controller → display):** current values, every 5th gptimer tick (500 ms), only while `g_twai0_tx_enabled`.

---

## 12. 0x1DA — Inverter HV Bus Voltage *(RX)*

| Property | Value |
|---|---|
| CAN ID | 0x1DA |
| Transmitting node | Inverter / motor controller |
| Period | 10 ms |
| Integrity | CRC-8 (poly 0x85) in d[7] — see [Section 1](#1-shared-crc-8-integrity-algorithm) |

| Byte | Bits | Field | Decoding Rule |
|------|------|-------|---------------|
| d[0] | [7:0] | HV bus voltage bits [9:2] *(inferred)* | 10-bit raw × 0.5 V (≈ d[0] × 2 V) |
| d[1] | [7:6] | HV bus voltage bits [1:0] | |

`d[0] = 0xFF` (≈ 510 V) is a placeholder sent before the first measurement — ignore it. This is the reliable main-relay indicator: 0–2 V with the main relays open, 194 → 380 → 388 V during the precharge after the normal-charge request. The `0x1DB` pack voltage (≈ 386 V) is the battery's internal voltage and does not show the relays. The DBC's unattached `InverterVoltage` signal does not match the data.

---

## 13. 0x603 / 0x68C — Power-Switch Wake Markers *(RX)*

1-byte frames (`00`, DLC 1 — the DBC lists 8) sent ≈ 40 ms before the VCM's first frame in **every power-button wake** (13 of 13 captures that include the wake) and **never** before a charge wake. The VCM-authorised path uses them to tell a power-button wake from a VCM self-wake (charge timer, immediate-charge switch); a marked wake that does not reach `0x50B` power ON within `SM_POWER_ON_WAIT_MS` is still treated as a charge wake.
