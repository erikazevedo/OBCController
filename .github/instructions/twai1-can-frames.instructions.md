---
applyTo: "**/twai*"
---
# TWAI1 CAN Frame Specification — OBCController

TWAI1 carries communication between the ESP32-C6 and the Lear OBC (On-Board Charger). The bus operates at 500 kbit/s. Five message IDs are defined: one TX frame sent by the controller and four RX frames sent by the OBC.

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

> **Source note:** The 0x616 field layout is inferred from observed OBC behavior (ControlEcho mirrors the last received 0x050). No DBC signals are formally defined for this message ID.

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
| 4    | Lear_BusVoltage | Single byte, unit = 0.1 V      | 12 V auxiliary bus voltage       |
| 5–7  | (undefined)     | —                               |                                  |

**Decode examples:**
- Bytes `[0x17, 0x70]` → raw = 6000 → **600.0 V DC**
- Bytes `[0x00, 0xC8]` → raw = 200 → **20.0 A DC**
- Byte `[0x78]` → raw = 120 → **12.0 V auxiliary bus**

---

## 0x618 — Lear_ACData

**Direction:** RX (OBC → controller)  
**DLC:** 8 bytes

| Byte | Field          | Encoding             | Notes                                                                 |
|------|----------------|----------------------|-----------------------------------------------------------------------|
| 0    | Lear_ACCurrent | High byte of raw     | Big-endian 16-bit — **scale factor unverified, see Known Ambiguities** |
| 1    | Lear_ACCurrent | Low byte of raw      |                                                                       |
| 2–7  | (undefined)    | —                    |                                                                       |

---

## Known Ambiguities

### 0x618 Lear_ACCurrent scale factor (UNVERIFIED)

The scale factor for `Lear_ACCurrent` has not been confirmed against a real AC capture for this installation:

| Source                                | Scale             | Notes                                                    |
|---------------------------------------|-------------------|----------------------------------------------------------|
| DBC (dual-charger convention)         | raw × 0.1 = A     | Assumes two Lear OBCs in parallel; over-reports by ~2×   |
| Coda / OVMS single-charger convention | raw × (1 ÷ 18.25) ≈ raw × 0.0548 = A | Documented for single-OBC installations |

This project uses a **single** Lear OBC. The DBC value (×0.1) is expected to over-report AC current by approximately 2× relative to the true reading. **The correct scale MUST be validated against a captured AC session before this value is used for energy accounting or control decisions.**
