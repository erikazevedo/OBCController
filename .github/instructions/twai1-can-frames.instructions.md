---
applyTo: "**/twai*"
---
# TWAI1 CAN Frame Specification — OBCController

TWAI1 carries communication between the ESP32-C6 and the Lear OBC (On-Board Charger). The bus operates at 500 kbit/s. Six message IDs are defined: one TX frame sent by the controller and five RX frames sent by the OBC.

> **The Lear broadcasts whenever it is powered** (≈ 1 Hz) — also with no AC (car in READY, no plug) and for ≈ 6–25 s after the VCM sleeps — so a Lear frame on its own does not mean AC is present. See `state-machine.instructions.md`, **Lear hold-off**.

---

## 0x050 — Lear_Control

**Direction:** TX (controller → OBC)  
**DLC:** 8 bytes  
**Cycle:** Must be sent at least once per second; withholding frames causes the OBC to stop charging.

| Byte | Field               | Encoding                        | Notes                                     |
|------|---------------------|---------------------------------|-------------------------------------------|
| 0    | (reserved)          | Always `0x00`                   |                                           |
| 1    | Lear_ControlByte1   | Always `0xDC`                   | Fixed protocol byte, purpose unknown      |
| 2    | Lear_TargetVoltage  | High byte of (voltage_V × 10)   | Big-endian 16-bit, unit = 0.1 V           |
| 3    | Lear_TargetVoltage  | Low byte of (voltage_V × 10)    |                                           |
| 4    | (reserved)          | Always `0x00`                   |                                           |
| 5    | Lear_ACCurrentLimit | High byte of (amps × 10)        | Big-endian 16-bit, unit = 0.1 A           |
| 6    | Lear_ACCurrentLimit | Low byte of (amps × 10)         |                                           |
| 7    | Lear_ChargeEnable   | bit 0: `1` = enable, `0` = disable | Bits 7–1 are zero                      |

**Lear_ChargeEnable behavior:** Setting bit 0 of byte 7 to `1` commands the OBC to charge; setting it to `0` commands the OBC to stop. When sending a disable frame, bit 0 MUST be `0` — it is not sufficient to merely withhold frames (though withholding frames also causes the OBC to stop after ~1 s).

**Example — 400 V, 16 A AC limit, charging enabled:**

| Byte | Value  | Derivation                         |
|------|--------|------------------------------------|
| 0    | `0x00` | reserved                           |
| 1    | `0xDC` | fixed                              |
| 2    | `0x0F` | high byte of 4000 (400 × 10)       |
| 3    | `0xA0` | low byte of 4000                   |
| 4    | `0x00` | reserved                           |
| 5    | `0x00` | high byte of 160 (16 × 10)         |
| 6    | `0xA0` | low byte of 160                    |
| 7    | `0x01` | charge enable bit set              |

---

## 0x274 — Lear_Temperature

**Direction:** RX (OBC → controller)  
**DLC:** 8 bytes

| Byte | Field           | Encoding                    | Notes                            |
|------|-----------------|-----------------------------|----------------------------------|
| 0–5  | (undefined)     | —                           | Not documented in DBC            |
| 6    | Lear_MasterTemp | `raw − 40 = °C`             | `0xFF` = not available / invalid |
| 7    | Lear_SlaveTemp  | `raw − 40 = °C`             | `0xFF` = not available / invalid |

**Decode example:** raw byte value `0x5A` (90) → 90 − 40 = **50 °C**.

---

## 0x27D — Lear AC input voltage

**Direction:** RX (OBC → controller)  
**DLC:** 8 bytes  
**Cycle:** ≈ 1 s

| Byte | Field            | Encoding         | Notes                                                                       |
|------|------------------|------------------|-----------------------------------------------------------------------------|
| 0    | AC input voltage | High byte of raw | Big-endian 16-bit, V = raw ÷ 32 (0.5 V steps; the low nibble of d[1] is always 0) |
| 1    | AC input voltage | Low byte of raw  |                                                                             |
| 2–7  | (undefined)      | —                | Vary with load; not decoded                                                 |

**Decode examples:** `0F 80` → 3968 ÷ 32 = **124.0 V**; `1E 10` → 7696 ÷ 32 = **240.5 V**.

**Observed (2026-11-01):** 120 V EVSE 124–125 V idle, 113–114 V at 1.24 kW; 240 V EVSE 242–251 V idle, 240–240.5 V at 3.0 kW; no AC 0, with blips of up to 13.5 V when the charge contactors switch. After AC is removed it decays to 0 within ≈ 1–2 s.

> **Source note:** Not in the DBC (`Lear_Unkown4` has no signals). Identified by comparing 120 V and 240 V sessions with the raw capture below. `0x615` d[6] × 2 V carries the same voltage at 2 V resolution and is not used. Only two voltage points exist: the scale is good for AC presence, the 100 / 200 V class and watts-to-amps conversion, but unproven for metering. Decoded by `lear27d_ac_dv()` (`obc_frames.h`); the state machine requires `SM_AC_PRESENT_MIN_V` for AC present and reports the reading in `0x380`.

---

## 0x616 — Lear_ControlEcho

**Direction:** RX (OBC → controller)  
**DLC:** 8 bytes

No signals are defined in the DBC for this message. The layout is inferred from observed behavior: 0x616 mirrors the last received 0x050 Lear_Control frame.

| Byte  | Field                 | Encoding                        | Notes                                        |
|-------|-----------------------|---------------------------------|----------------------------------------------|
| 0     | (undefined)           | —                               | Likely mirrors 0x050 byte 0 (reserved)       |
| 1     | (undefined)           | —                               | Likely mirrors 0x050 byte 1 (`0xDC`)         |
| 2     | echoed TargetVoltage  | High byte of (voltage_V × 10)   | Big-endian 16-bit, unit = 0.1 V              |
| 3     | echoed TargetVoltage  | Low byte of (voltage_V × 10)    |                                              |
| 4     | (undefined)           | —                               | Likely mirrors 0x050 byte 4 (reserved)       |
| 5     | echoed ACCurrentLimit | High byte of (amps × 10)        | Big-endian 16-bit, unit = 0.1 A              |
| 6     | echoed ACCurrentLimit | Low byte of (amps × 10)         |                                              |
| 7     | echoed ChargeEnable   | bit 0: `1` = enabled, `0` = disabled |                                        |

> **Source note:** No DBC signals are defined for this message ID. Confirmed on this Lear (2026-10-31): d[2..3], d[5..6] and d[7] bit 0 echo the last 0x050 target voltage, AC limit and ChargeEnable (e.g. `400.0 V`, `14.1 A`, enable `1`), and read all zero before the first 0x050. Bytes 0, 1 and 4 are still inferred.

---

## 0x617 — Lear_DCData

**Direction:** RX (OBC → controller)  
**DLC:** 8 bytes

| Byte | Field           | Encoding                        | Notes                            |
|------|-----------------|---------------------------------|----------------------------------|
| 0    | Lear_DCVoltage  | High byte of raw                | Big-endian 16-bit, unit = 0.1 V  |
| 1    | Lear_DCVoltage  | Low byte of raw                 |                                  |
| 2    | Lear_DCCurrent  | High byte of raw                | Big-endian 16-bit, unit = 0.1 A  |
| 3    | Lear_DCCurrent  | Low byte of raw                 |                                  |
| 4    | Lear_BusVoltage | Single byte (DBC: unit = 0.1 V) | **Not a 12 V reading on this Lear** — see note |
| 5–7  | (undefined)     | —                               |                                  |

**Decode examples:**
- Bytes `[0x17, 0x70]` → raw = 6000 → **600.0 V DC**
- Bytes `[0x00, 0xC8]` → raw = 200 → **20.0 A DC**

> **d[4] meaning unconfirmed.** The DBC calls it `Lear_BusVoltage` (0.1 V, 12 V auxiliary bus), but this Lear reports `0x0A`–`0x10` while powered (1.0–1.6 V at that scale). An earlier LeafOBCBridge build decoded d[4..5] as a PFC bus voltage (big-endian × 0.1 V: 204.8 → 409.6 V, d[5] = `0x00`). Not used by the firmware.

---

## 0x618 — Lear_ACData

**Direction:** RX (OBC → controller)  
**DLC:** 8 bytes

| Byte | Field          | Encoding             | Notes                                                                 |
|------|----------------|----------------------|-----------------------------------------------------------------------|
| 0    | Lear_ACCurrent | High byte of raw     | Big-endian 16-bit. **Despite the DBC name this is DC output current**, consistent with raw ÷ 32 = A — see Known Ambiguities |
| 1    | Lear_ACCurrent | Low byte of raw      |                                                                       |
| 2–4  | (undefined)    | —                    |                                                                       |
| 5    | Output power limit | High byte of raw | Big-endian 16-bit, W. Set before current flows and constant through a session; DC V × A settles on it (1245 W at 120 V, 3045 W at 240 V) |
| 6    | Output power limit | Low byte of raw  |                                                                       |
| 7    | (undefined)    | —                    |                                                                       |

---

## Raw capture — finding the AC input voltage

The Lear's documentation has no AC voltage signal (the DBC lists IDs `0x054`, `0x277`, `0x279`, `0x27D`, `0x615`, `0x619` with no signals, and `0x274` d[0–5], `0x616` d[0,1,4], `0x617` d[5–7], `0x618` d[2–7] are undefined). With `SM_LOG_LEAR_RAW 1` (default) the firmware receives every TWAI1 ID and logs, with tag `lear`:

Line format (values illustrative):

```
I (22705) lear: 0x618 dlc=8 data 00 78 00 00 00 00 00 00 (first frame)
I (23705) lear: 0x618 dlc=8 data 00 7A 00 00 00 00 00 00 chg 00 02 00 00 00 00 00 00
```

`chg` is the OR of the bits that flipped between consecutive frames since the previous line. Bytes that carry a counter, a checksum or a measurement show busy `chg` bits; a byte that follows the supply voltage is steady within a session and differs between sessions.

Capture procedure, each into its own log file, noting the EVSE voltage and the time of each action:
1. Car READY, no plug (Lear powered, no AC).
2. 120 V EVSE plugged in: before and during charging.
3. 240 V EVSE plugged in: before and during charging.

Then compare, per ID, the steady bytes between the 120 V and 240 V logs. A byte that is identical across sessions of the same voltage and differs between voltages (a 240 V reading should be about twice a 120 V one, possibly offset or scaled) is the candidate. IDs outside `0x274`, `0x27D` and `0x616`–`0x618` never feed the state machine.

**Result (2026-11-01):** the AC input voltage is `0x27D` d[0..1] (see above); `0x615` d[6] × 2 V carries the same value at 2 V resolution.

---

## Known Ambiguities

### 0x618 d[0..1] is DC output current, not AC current

The DBC calls d[0..1] `Lear_ACCurrent` with ×0.1 A (dual-charger convention); the Coda / OVMS single-charger convention uses × (1 ÷ 18.25). Neither fits this Lear (2026-11-01 captures):

| Session | d[0..1] | DC output (`0x617`) | d[0..1] ÷ DC 0.1 A count | d[0..1] ÷ 32 | d[5..6] ÷ DC V |
|---|---|---|---|---|---|
| 120 V | 102–103 | 386.7 V, 3.2 A | 3.19–3.22 | 3.19–3.22 A | 3.22 A |
| 240 V | 250–252 | 388.3 V, 7.8 A | 3.21–3.23 | 7.81–7.88 A | 7.84 A |

The ratio to the DC current stays ≈ 3.2 while the AC voltage doubles; for an AC current it would roughly halve. At 120 V both AC scales also imply less input power than the DC output. The field therefore tracks the DC output current (consistent with 1/32 A). It is not used by the firmware.
