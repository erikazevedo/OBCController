#pragma once
#include <stdbool.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Handle exposed so app_main() can create the task */
extern TaskHandle_t state_machine_task_hdl;

/* ── SM Signal Flags ──────────────────────────────────────────────────────────
 * Set by rx_dispatcher handler callbacks; read and cleared by the state
 * machine task each 10 ms cycle.  All DRAM_ATTR for ISR-safe placement.
 * ---------------------------------------------------------------------------*/

/** Set by 0x50B handler when VCM sends RUN (d[3][7:6] == 0b11). */
extern volatile bool g_sm_vcm_run;

/** Set by 0x50B handler when VCM sends SLEEP (d[3][7:6] == 0b00). */
extern volatile bool g_sm_vcm_sleep;

/** Set by 0x1F2 handler when VCM sends CHARGING_SLOW (d[2] == 0x20). */
extern volatile bool g_sm_vcm_charge_cmd;

/** Set by 0x1DB handler when LBC failsafe is active (d[1][2:0] != 0b000). */
extern volatile bool g_sm_lbc_failsafe;

/** Set by 0x1D4 handler when charge interrupted (d[6] == 0x8C). */
extern volatile bool g_sm_charge_interrupted;

/** Set by TWAI1 on_rx_done when any frame is received (OBC alive signal). */
extern volatile bool g_sm_twai1_rxd;

/** Decoded SOC percentage from 0x55B (0.0–100.0 %). 32-bit aligned — atomic on RISC-V. */
extern volatile float g_sm_soc_pct;

/**
 * FreeRTOS task body for the application state machine.
 * Priority: configMAX_PRIORITIES - 4 (21). Stack: 4096 bytes.
 */
void state_machine_task(void *pvParams);
