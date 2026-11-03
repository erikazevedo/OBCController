#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Handle exposed so app_main() can create the task */
extern TaskHandle_t state_machine_task_hdl;

/* ── SM Signal Flag ───────────────────────────────────────────────────────────
 * Set by an rx_dispatcher handler; read and cleared by the state machine task
 * each 10 ms cycle.  DRAM_ATTR for ISR-safe placement.
 * ---------------------------------------------------------------------------*/

/** Set by 0x1DB handler when LBC failsafe is active (d[1][2:0] != 0b000). */
extern volatile bool g_sm_lbc_failsafe;

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
    /* Lear (TWAI1): any core frame (TWAI1_ID_IS_CORE) = Lear alive (it broadcasts whenever powered);
     * 0x617 DC output; 0x27D AC input voltage */
    volatile uint32_t cnt_lear;
    volatile uint32_t cnt_617;
    volatile uint16_t lear_dc_dv;      /* 0.1 V */
    volatile uint16_t lear_dc_da;      /* 0.1 A */
    volatile uint32_t cnt_27d;
    volatile uint16_t lear_ac_dv;      /* 0.1 V, written before cnt_27d is incremented */
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
