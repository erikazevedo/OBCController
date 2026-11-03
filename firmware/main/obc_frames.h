#pragma once
/*
 * Pure encode/decode helpers for the Nissan LEAF ZE0 EV-CAN frames the bridge
 * transmits (0x380, 0x5BF) or decodes (0x1F2, 0x1D4, 0x50B, 0x1DA) and for the
 * Lear charger frames on TWAI1 (0x050, 0x27D, 0x617).
 *
 * Header-only with no ESP-IDF dependencies so the same code can be checked on a
 * host against real captures.  Every helper is force-inlined because the
 * checksum helpers run inside the IRAM gptimer ISR.
 *
 * Bit positions come from the AC-charge capture analysed in
 * LeafOBCBridge/docs/OBC-VCM-State-Sequence.md (section 5).
 */
#include <stdbool.h>
#include <stdint.h>

#define OBC_INLINE static inline __attribute__((always_inline))

/* ── 0x380 OBC status (TX, 100 ms) ─────────────────────────────────────────── */
#define OBC380_D0_STOP_ACK        0x01u   /* stop request acknowledged          */
#define OBC380_D0_BASE            0x02u   /* set in every frame after the first */
#define OBC380_D0_AC_100V         0x08u   /* AC input type 100 V class (inferred) */
#define OBC380_D0_AC_200V         0x10u   /* AC input type 200 V class          */
#define OBC380_D1_FIXED           0x04u
#define OBC380_D2_MODE_MASK       0x30u
#define OBC380_D2_MODE_NORMAL     0x10u
#define OBC380_D2_MODE_STANDBY    0x20u
#define OBC380_D2_EVSE_READY      0x08u
#define OBC380_D2_RELEASE_SW      0x04u
#define OBC380_D4_NORMAL_RELAY    0x40u
#define OBC380_D4_QUICK_RELAY     0x20u
#define OBC380_D4_S2              0x10u
#define OBC380_D7_HV_OUTPUT       0x80u   /* set ~0.5 s after the relay closes; meaning unknown */
#define OBC380_CSUM_OFFSET        5u

/* Chargeable power: 9 bits x 0.1 kW in d[2] bit 0 (MSB) and d[3]. */
OBC_INLINE void obc380_set_chargeable(uint8_t *d, uint16_t raw)
{
    if (raw > 511u) raw = 511u;
    d[2] = (uint8_t)((d[2] & 0xFEu) | ((raw >> 8) & 0x01u));
    d[3] = (uint8_t)(raw & 0xFFu);
}

OBC_INLINE uint16_t obc380_get_chargeable(const uint8_t *d)
{
    return (uint16_t)(((d[2] & 0x01u) << 8) | d[3]);
}

/* Output power: 9 bits x 0.1 kW in d[4] bits 3:0 (MSBs) and d[5] bits 7:3. */
OBC_INLINE void obc380_set_output_power(uint8_t *d, uint16_t raw)
{
    if (raw > 511u) raw = 511u;
    d[4] = (uint8_t)((d[4] & 0xF0u) | ((raw >> 5) & 0x0Fu));
    d[5] = (uint8_t)((d[5] & 0x07u) | ((raw & 0x1Fu) << 3));
}

OBC_INLINE uint16_t obc380_get_output_power(const uint8_t *d)
{
    return (uint16_t)(((d[4] & 0x0Fu) << 5) | (d[5] >> 3));
}

/* AC voltage: 9 bits in d[5] bits 2:0 (MSBs) and d[6] bits 7:2; V = raw * 0.5 + 70, raw 0 = no AC. */
OBC_INLINE void obc380_set_ac_raw(uint8_t *d, uint16_t raw)
{
    if (raw > 511u) raw = 511u;
    d[5] = (uint8_t)((d[5] & 0xF8u) | ((raw >> 6) & 0x07u));
    d[6] = (uint8_t)((d[6] & 0x03u) | ((raw & 0x3Fu) << 2));
}

OBC_INLINE uint16_t obc380_get_ac_raw(const uint8_t *d)
{
    return (uint16_t)(((d[5] & 0x07u) << 6) | (d[6] >> 2));
}

/* AC voltage in 0.1 V to the 0x380 raw value: 0.5 V steps above 70 V, 0 (no AC) at or below 70 V. */
OBC_INLINE uint16_t obc380_ac_raw_from_dv(uint16_t dv)
{
    if (dv <= 700u) return 0u;
    uint32_t raw = ((uint32_t)dv - 700u) / 5u;
    return (uint16_t)(raw > 511u ? 511u : raw);
}

OBC_INLINE void obc380_set_mode(uint8_t *d, uint8_t mode)
{
    d[2] = (uint8_t)((d[2] & (uint8_t)~OBC380_D2_MODE_MASK) | (mode & OBC380_D2_MODE_MASK));
}

OBC_INLINE void obc_set_bits(uint8_t *byte, uint8_t mask, bool on)
{
    *byte = on ? (uint8_t)(*byte | mask) : (uint8_t)(*byte & (uint8_t)~mask);
}

/* ── 0x5BF OBC charge state (TX, 100 ms) ───────────────────────────────────── */
#define OBC5BF_ST_START1          0x10u   /* first frame of every OBC start     */
#define OBC5BF_ST_START2          0x18u   /* 100 ms later                       */
#define OBC5BF_ST_IDLE            0x28u   /* power-button wake, no charge       */
#define OBC5BF_ST_STOP_ACK        0x30u   /* stop request acknowledged          */
#define OBC5BF_ST_ASLEEP          0x38u   /* after GoToSleep                    */
#define OBC5BF_ST_CHG_REQ         0x40u   /* normal-charge request received     */
#define OBC5BF_ST_RELAY_CLOSED    0x48u
#define OBC5BF_ST_PREP1           0x50u
#define OBC5BF_ST_PREP2           0x58u
#define OBC5BF_ST_CHARGING        0x60u
#define OBC5BF_ST_END             0x70u   /* end of charge after the stop request */
#define OBC5BF_ST_RELAY_OPEN      0x78u
#define OBC5BF_ST_OBC_STOP        0x80u   /* OBC-initiated stop (release switch) */
#define OBC5BF_D1_AC_PRESENT      0x65u   /* d[1] while AC is present; meaning unknown */
#define OBC5BF_DC_V_OFFSET        263u    /* d[3] = DC volts - 263 */
#define OBC5BF_CSUM_OFFSET        1u

OBC_INLINE void obc5bf_set_dc_volts(uint8_t *d, uint16_t volts)
{
    uint16_t v = (volts > OBC5BF_DC_V_OFFSET) ? (uint16_t)(volts - OBC5BF_DC_V_OFFSET) : 0u;
    d[3] = (uint8_t)(v > 255u ? 255u : v);
}

/* DC output current, whole amps, d[6] bits 7:3. */
OBC_INLINE void obc5bf_set_dc_amps(uint8_t *d, uint16_t amps)
{
    if (amps > 31u) amps = 31u;
    d[6] = (uint8_t)((d[6] & 0x07u) | (amps << 3));
}

/* ── Shared PRUN / CSUM for 0x380 and 0x5BF ────────────────────────────────── */
/* CSUM = (sum of nibbles of d[0..6] + high nibble of d[7] - offset) & 0xF. */
OBC_INLINE uint8_t obc_nibble_csum(const uint8_t *d, uint8_t offset)
{
    uint16_t s = 0;
    for (int i = 0; i < 7; i++) {
        s = (uint16_t)(s + (d[i] >> 4) + (d[i] & 0x0Fu));
    }
    s = (uint16_t)(s + (d[7] >> 4));
    return (uint8_t)((s - offset) & 0x0Fu);
}

/* Keep d[7] bits 7:6, write PRUN to bits 5:4, then the CSUM to bits 3:0. */
OBC_INLINE void obc_frame_seal(uint8_t *d, uint8_t prun, uint8_t offset)
{
    d[7] = (uint8_t)((d[7] & 0xC0u) | ((prun & 0x03u) << 4));
    d[7] = (uint8_t)(d[7] | obc_nibble_csum(d, offset));
}

/* ── 0x1F2 VCM charge command (RX, 10 ms) ──────────────────────────────────── */
#define VCM1F2_REQ_MASK           0x60u
#define VCM1F2_REQ_NONE           0x00u
#define VCM1F2_REQ_NORMAL         0x20u
#define VCM1F2_REQ_QUICK          0x40u
#define VCM1F2_REQ_STOP           0x60u

/* d[2] bits 6:5; this car also sets d[2] bits 3 and 7, so always mask. */
OBC_INLINE uint8_t vcm1f2_request(const uint8_t *d)
{
    return (uint8_t)(d[2] & VCM1F2_REQ_MASK);
}

/* Maximum charge power command, W: (d[1] - 100) * 100, 0 below 100. */
OBC_INLINE uint16_t vcm1f2_power_cmd_w(const uint8_t *d)
{
    return (uint16_t)((d[1] > 100u) ? (d[1] - 100u) * 100u : 0u);
}

/* d[5] = CONSULT "VCM MODE": 0-7 start-up, 10 no connector, 30 READY,
 * 109-129 entering charge mode, 130 charging, 131-144 shutdown after charge. */
OBC_INLINE uint8_t vcm1f2_mode(const uint8_t *d)
{
    return d[5];
}

/* ── 0x1D4 VCM (RX, 10 ms) ─────────────────────────────────────────────────── */
OBC_INLINE bool vcm1d4_hv_live(const uint8_t *d)      { return (d[4] & 0x04u) != 0u; }
OBC_INLINE bool vcm1d4_relay_plus(const uint8_t *d)   { return (d[5] & 0x40u) != 0u; }
OBC_INLINE bool vcm1d4_charge_session(const uint8_t *d) { return (d[6] & 0x80u) != 0u; }

/* ── 0x50B VCM wake / sleep (RX, 100 ms) ───────────────────────────────────── */
#define VCM50B_CMD_SLEEP          0x00u
#define VCM50B_CMD_WAKE           0x03u

OBC_INLINE uint8_t vcm50b_wake_cmd(const uint8_t *d) { return (uint8_t)((d[3] >> 6) & 0x03u); }
/* d[2] stays 0x00 for a whole charge wake; non-zero only with the power switch ON. */
OBC_INLINE bool vcm50b_power_on(const uint8_t *d)    { return d[2] != 0x00u; }

/* ── 0x1DA inverter (RX, 10 ms) ────────────────────────────────────────────── */
/* d[0] = 0xFF is a placeholder (decodes as ~510 V) sent before a measurement exists. */
OBC_INLINE bool inv1da_bus_valid(const uint8_t *d) { return d[0] != 0xFFu; }

/* HV bus voltage, 10 bits x 0.5 V in d[0] and d[1] bits 7:6; returned in 0.1 V. */
OBC_INLINE uint16_t inv1da_bus_dv(const uint8_t *d)
{
    uint16_t raw = (uint16_t)(((uint16_t)d[0] << 2) | (d[1] >> 6));
    return (uint16_t)(raw * 5u);
}

/* ── Lear charger (TWAI1) ──────────────────────────────────────────────────── */
OBC_INLINE uint16_t lear617_dc_dv(const uint8_t *d) { return (uint16_t)(((uint16_t)d[0] << 8) | d[1]); }
OBC_INLINE uint16_t lear617_dc_da(const uint8_t *d) { return (uint16_t)(((uint16_t)d[2] << 8) | d[3]); }

/* 0x27D: AC input voltage, big-endian d[0..1] / 32 V (0.5 V steps, low nibble always 0), returned in 0.1 V.
 * Not in the DBC; identified from 120 V and 240 V sessions (twai1-can-frames.instructions.md). */
OBC_INLINE uint16_t lear27d_ac_dv(const uint8_t *d)
{
    uint32_t raw = ((uint32_t)d[0] << 8) | d[1];
    return (uint16_t)((raw * 10u) / 32u);
}

/* 0x050 Lear_Control: AC current limit in 0.1 A, big-endian d[5..6]. */
OBC_INLINE void lear050_set_ac_limit(uint8_t *d, uint16_t deci_amps)
{
    d[5] = (uint8_t)(deci_amps >> 8);
    d[6] = (uint8_t)(deci_amps & 0xFFu);
}

OBC_INLINE void lear050_set_enable(uint8_t *d, bool on)
{
    d[7] = on ? 0x01u : 0x00u;
}
