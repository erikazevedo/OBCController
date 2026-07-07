---
applyTo: "**/twai*"
---
# Nissan LEAF ZE0 EV-CAN Frame Specification

**Bus:** EV-CAN | **Baud rate:** 500 kbps | **ID format:** Standard 11-bit | **Model years:** 2011–2012 (ZE0, 24 kWh)

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

---

## 1. Shared CRC-8 Integrity Algorithm

Applies to received messages: **0x1DB, 0x1DC, 0x55B, 0x1D4**

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
| d[0] | [7:0] | Status byte | Fixed: 0x02 |
| d[1] | [7:0] | Fixed byte | Fixed: 0x04 |
| d[2] | [7:5] | Reserved | Always 0 |
| d[2] | [4] | Mode flag | Fixed: 1 |
| d[2] | [3:1] | Reserved | Always 0 |
| d[2] | [0] | Max charge power bit 8 | `(power_W ÷ 100) >> 8`, masked to 1 bit; all other bits of d[2] preserved |
| d[3] | [7:0] | Max charge power bits [7:0] | `(power_W ÷ 100) & 0xFF` |
| d[4] | [7] | Reserved | Always 0 |
| d[4] | [6] | AC relay | 0 = open, 1 = closed |
| d[4] | [5] | QC relay | 0 = open, 1 = closed |
| d[4] | [4:0] | Reserved | Always 0 |
| d[5] | [7:0] | AC grid voltage (high) | Always 0x00 — encoding not specified |
| d[6] | [7:0] | AC grid voltage (low) | Always 0x00 — encoding not specified |
| d[7] | [7:6] | Reserved status flags | Always 0 |
| d[7] | [5:4] | PRUN | 2-bit rolling counter — see PRUN procedure |
| d[7] | [3:0] | CSUM | Nibble checksum — see CSUM procedure |

**Max charge power encoding:** Divide `power_W` by 100 (integer, resolution = 100 W, range 0–51100 W). The resulting 9-bit value has its bit 8 placed in d[2][0] and its lower 8 bits placed in d[3]. All other bits of d[2] must be preserved when writing d[2][0].

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
| d[1] | [7:0] | Reserved | Always 0x00 |
| d[2] | [7:0] | J1772 pilot current limit | `amps × 5` — e.g. 32 A → 160 (0xA0) |
| d[3] | [7:0] | Reserved | Always 0x00 |
| d[4] | [7:0] | Charger status | Enumerated — see Charger Status table |
| d[5] | [7:0] | Reserved | Always 0x00 |
| d[6] | [7:0] | Reserved | Always 0x00 |
| d[7] | [7:6] | Reserved | Always 0 |
| d[7] | [5:4] | PRUN | 2-bit rolling counter — see PRUN procedure |
| d[7] | [3:0] | CSUM | Nibble checksum — see CSUM procedure |

### Charger Status Enum (d[4])

| Value | Symbolic Name | Description |
|-------|---------------|-------------|
| 0x10 | TIMER_WAIT2 | OBC waking; VCM not yet confirmed. Broadcast **before** PLUG_BEEP to trigger VCM wake sequence |
| 0x18 | PLUG_BEEP | Cable just inserted; triggers VCM plug-detection beep and pilot |
| 0x28 | IDLE | Vehicle ON, no active charge session |
| 0x30 | TIMER_WAIT | Cable plugged; waiting for timer expiry or pilot authorisation |
| 0x38 | ASLEEP | Vehicle off; OBC goes silent |
| 0x40 | CHARGE_INIT | First step of charge ramp-up; follows VCM CHARGING_SLOW authorisation |
| 0x60 | CHARGING | L2 AC charging active |
| 0x70 | CHARGE_START | Transitional — charge starting |
| 0xB0 | QUICK_CHARGING | CHAdeMO DC fast charge active |

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
| Integrity | Nibble CSUM in d[7][3:0] — formula not verified |

### Byte Layout

| Byte | Bits | Field | Decoding Rule |
|------|------|-------|---------------|
| d[0] | [7] | 80% charge target flag | 1 = charge to 80% SOC; 0 = charge to 100% SOC |
| d[2] | [7:0] | Charge state | Enumerated — see Charge State table |
| d[7] | [3:0] | CSUM | Not consumed; formula not documented |

### Charge State Enum (d[2])

| Value | Symbolic Name | Notes |
|-------|---------------|-------|
| 0x00 | Unknown / uninitialised | VCM initial or silent state |
| 0x08 | CHARGING_EV_SYS_ON | EV system waking after EVA pulse — transitional |
| 0x20 | CHARGING_SLOW | L2 AC charge authorised |
| 0x40 | CHARGING_QUICK_START | CHAdeMO start |
| 0x60 | CHARGING_IDLE | Normal ~20 s intermediate hold state |
| 0x68 | CHARGING_L1_AUTH | L1 (120 V) charge authorisation — observed on ZE0 |
| 0x88 | CHARGING_PREP | VCM pre-charge preparation — transitional |
| 0xC0 | CHARGING_QUICK | CHAdeMO active |
| 0xE0 | CHARGING_QUICK_END | CHAdeMO end |
| 0xE8 | (transitional) | Observed alternating with 0x68 during ZE0 L1 charging |

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
| d[4] | [2] | HV supply status | 1 = HV power supplied |
| d[5] | [6] | Relay+ output status | 1 = MainRelayPlus contactor closed |
| d[6] | [7:0] | Charge status | Enumerated — see Charge Status table |
| d[7] | [7:0] | CRC-8 | See Section 1 |

### Charge Status Values (d[6])

| Value | Meaning |
|-------|---------|
| 0xE0 | STATUS_CHARGING — VCM confirmed charge session active |
| 0x8C | STATUS_INTERRUPTED — charge interrupted |

**Cable removal detection:** d[6] bit 7 equal to 0 indicates the charge was interrupted by the user. Any d[6] value with bit 7 = 1 (including 0xE0) means the session is active and uninterrupted.

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
| d[2] | [7:0] | EV_SystemState | 0x00 = OFF; 0x04 = ON |
| d[3] | [7:6] | WakeUpSleepCmd | 0b11 = RUN; 0b00 = SLEEP; any non-zero value = vehicle on |
| d[3] | [5:0] | Not decoded | — |
| d[4–6] | — | Not decoded | — |

### EV_SystemState Values (d[2])

| Value | Meaning |
|-------|---------|
| 0x00 | OFF |
| 0x02 | BOOT1 — early boot; TX broadcast starts |
| 0x04 | ON |
| 0x06 | BOOT2 |

> Minimum required DLC to decode: 4 bytes (d[0]–d[3]).

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
