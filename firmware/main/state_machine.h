#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Handle exposed so app_main() can create the task */
extern TaskHandle_t state_machine_task_hdl;

/* ── SM Signal Flags ──────────────────────────────────────────────────────────
 * Set by rx_dispatcher handler callbacks; read and cleared by the state
 * machine task each 10 ms cycle.  All DRAM_ATTR for ISR-safe placement.
 * ---------------------------------------------------------------------------*/

/** Set by 0x50B handler on every frame with d[2] != 0x00 (power switch ON). */
extern volatile bool g_sm_vcm_run;

/** Set by 0x50B handler on every frame with d[2] == 0x00 (power switch OFF — includes a charge wake). */
extern volatile bool g_sm_vcm_sleep;

/** Set by 0x1DB handler when LBC failsafe is active (d[1][2:0] != 0b000). */
extern volatile bool g_sm_lbc_failsafe;

/** Set by 0x1D4 handler when charge interrupted (d[6] == 0x8C; never seen in captures). */
extern volatile bool g_sm_charge_interrupted;

/** Set by TWAI1 on_rx_done when any frame is received (OBC alive signal). */
extern volatile bool g_sm_twai1_rxd;

/** Decoded SOC percentage from 0x55B (0.0–100.0 %). 32-bit aligned — atomic on RISC-V. */
extern volatile float g_sm_soc_pct;

/** Set by 0x50B handler: true when EV system state d[2] == 0x04 (ON), false when 0x00 (OFF). */
extern volatile bool g_sm_ev_on;

/* ── Decoded EV-CAN / Lear status ─────────────────────────────────────────────
 * Written only by the rx_dispatcher task, read by the state machine task.
 * cnt_* increment once per accepted frame so the state machine can age each
 * source with its own clock; the other fields hold the last decoded value.
 * ---------------------------------------------------------------------------*/
typedef struct {
    /* 0x50B VCM wake / sleep (100 ms) */
    volatile uint32_t cnt_50b;
    volatile uint32_t wake_count;      /* +1 on WakeUp after GoToSleep or after > SM_VCM_SILENT_MS silence */
    volatile bool     awake;           /* d[3][7:6] == 0b11 */
    volatile bool     power_on;        /* d[2] != 0x00: power switch ON (stays 0 in a charge wake) */
    volatile uint8_t  d2_50b;
    volatile uint8_t  d3_50b;
    /* 0x603 / 0x68C: sent ~40 ms before a power-button wake, never before a charge wake */
    volatile uint32_t cnt_beacon;
    /* 0x1F2 VCM charge command (10 ms) */
    volatile uint32_t cnt_1f2;
    volatile uint8_t  req_1f2;         /* d[2] & 0x60: 0x20 normal charge, 0x60 stop request */
    volatile uint16_t power_cmd_w;     /* d[1]: (raw - 100) * 100 W */
    volatile uint8_t  d0_1f2;
    volatile uint8_t  d1_1f2;
    volatile uint8_t  d2_1f2;
    volatile uint8_t  mode_1f2;        /* d[5] CONSULT VCM MODE */
    /* 0x1D4 VCM (10 ms) */
    volatile uint32_t cnt_1d4;
    volatile bool     hv_live;         /* d[4] bit 2 */
    volatile bool     relay_plus;      /* d[5] bit 6 */
    volatile bool     charge_session;  /* d[6] bit 7 */
    volatile uint8_t  d6_1d4;
    /* 0x1DA inverter HV bus voltage (10 ms) */
    volatile uint32_t cnt_1da;
    volatile uint16_t hv_bus_dv;       /* 0.1 V */
    /* 0x55B LBC SOC, placeholder 0x3FF filtered */
    volatile bool     soc_valid;
    volatile uint16_t soc_x10;         /* 0.1 % */
    /* 0x1DB / 0x1DC LBC (10 ms) — signal trace only */
    volatile uint32_t cnt_1db;
    volatile int16_t  lbc_current_ha;  /* 0.5 A, + = discharging, - = charging */
    volatile uint8_t  lbc_failsafe;    /* 0x1DB d[1] bits 2:0 */
    volatile uint8_t  lbc_relay_cut;   /* 0x1DB d[1] bits 4:3 */
    volatile bool     lbc_full;        /* 0x1DB d[3] bit 4, LB_Full_CHARGE_flag */
    volatile bool     lbc_main_relay;  /* 0x1DB d[3] bit 5 */
    volatile uint32_t cnt_1dc;
    volatile uint8_t  lbc_chg_status;  /* 0x1DC d[3] bits 1:0, LB_Charge_Power_Status */
    volatile uint16_t lbc_max_chgr_raw;/* 0x1DC LB_MAX_POWER_FOR_CHARGER, 0.1 kW */
    /* Lear (TWAI1): any frame = Lear alive; 0x617 DC output; 0x616 control echo; 0x618 / 0x274 trace only */
    volatile uint32_t cnt_lear;
    volatile uint32_t cnt_617;
    volatile uint16_t lear_dc_dv;      /* 0.1 V */
    volatile uint16_t lear_dc_da;      /* 0.1 A */
    volatile uint8_t  lear_aux_dv;     /* 0x617 d[4], 12 V auxiliary supply, 0.1 V */
    volatile uint16_t lear_ac_raw;     /* 0x618 AC current, x 0.1 (scale unverified) */
    volatile uint8_t  lear_temp_raw;   /* 0x274 d[6], deg C + 40 */
    volatile uint32_t cnt_616;
    volatile bool     lear_echo_en;
    volatile uint16_t lear_echo_tv_dv; /* 0.1 V */
    volatile uint16_t lear_echo_da;    /* 0.1 A */
} sm_rx_status_t;

extern sm_rx_status_t g_sm_rx;

/* ── Runtime Settings ─────────────────────────────────────────────────────────
 * Initialised from sm_config.h defaults at startup; overwritten by the 0x6C0
 * handler (rx_dispatcher) and advertised on 0x6C1 from the gptimer ISR.
 * Integer types so the ISR needs no soft-float.
 * ---------------------------------------------------------------------------*/

/** Lear OBC DC target voltage (V). */
extern volatile uint16_t g_sm_target_voltage_v;

/** SOC (%) at which a charging session is terminated. */
extern volatile uint8_t g_sm_target_soc_pct;

/**
 * FreeRTOS task body for the application state machine.
 * Priority: configMAX_PRIORITIES - 4 (21). Stack: 4096 bytes.
 */
void state_machine_task(void *pvParams);
