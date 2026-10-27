#include "rx_dispatcher.h"
#include "twai_handler.h"
#include "state_machine.h"
#include "sm_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "rx_disp";

TaskHandle_t rx_dispatcher_task_hdl = NULL;

/* ── 0x50B — VCM Wake/Sleep command ──────────────────────────────────────── */
// TODO: change the run/sleep stuff to just be based on the value of d[2]
// which seems to reflect the power state much better than d[3]
// if d[2] == 0x00u, vcm_run = false and vcm_sleep = true
// if d[2] != 0x00u, vcm_run = true and vcm_sleep = false
static void handle_50b(const uint8_t *data)
{
    // uint8_t cmd = (data[3] >> 6) & 0x03u;

    // if (cmd == 0x03u) {                                         /* RUN */
    //     g_sm_vcm_run       = true;
    //     g_sm_ev_on         = (data[2] != 0x00u);
    //     g_twai0_tx_enabled = g_sm_ev_on;
    // } else if (cmd == 0x00u) {                                  /* SLEEP */
    //     g_sm_vcm_sleep     = true;
    //     g_twai0_tx_enabled = false;
    // }

    // Give this a try
    // vcm_run and vcm_sleep are flags that get latched and cleared out in every state machine cycle
    // so just set the one you want to true
    if (data[2] == 0x00u) {
        // g_sm_vcm_run   = false; // gets latched
        g_sm_vcm_sleep = true;
        g_twai0_tx_enabled = false;
    } else {
        g_sm_vcm_run   = true;
        // g_sm_vcm_sleep = false; // gets latched
        g_twai0_tx_enabled = true;
    }
}

/* ── 0x1F2 — VCM Charge Command ──────────────────────────────────────────── */
static void handle_1f2(const uint8_t *data)
{
    if (data[2] == 0x20u) {       /* CHARGING_SLOW — only valid charge trigger */
        g_sm_vcm_charge_cmd = true;
    }
}

/* ── 0x1DB — LBC Voltage / Current / Failsafe ────────────────────────────── */
static void handle_1db(const uint8_t *data)
{
    if ((data[1] & 0x07u) != 0x00u) {    /* d[1][2:0] != 0b000 — failsafe active */
        g_sm_lbc_failsafe = true;
    }
}

/* ── 0x1D4 — VCM Torque Request / Charge Status ──────────────────────────── */
static void handle_1d4(const uint8_t *data)
{
    if (data[6] == 0x8Cu) {       /* STATUS_INTERRUPTED */
        g_sm_charge_interrupted = true;
    }
}

/* ── 0x55B — LBC State of Charge ─────────────────────────────────────────── */
static void handle_55b(const uint8_t *data)
{
    /* raw = 10-bit value; physical SOC (%) = raw / 10.0 */
    uint16_t raw = (uint16_t)(((uint16_t)data[0] << 2) | (data[1] >> 6));
    g_sm_soc_pct = (float)raw * 0.1f;
}

// Don't think we actually care about these
/* ── 0x603 / 0x68C — VCM Startup Beacons ────────────────────────────────── */
// static void handle_vcm_beacon(uint32_t id, const uint8_t *data)
// {
//     ESP_LOGI(TAG, "0x%03"PRIX32"  %02X %02X %02X %02X %02X %02X %02X %02X",
//              id, data[0], data[1], data[2], data[3],
//              data[4], data[5], data[6], data[7]);
//     /* Halt TX immediately — resumes on next 0x50B with ev_on=ON */
//     g_twai0_tx_enabled = false;
//     /* Signal SM to reset to SLEEP */
//     g_sm_vcm_sleep = true;
//     ESP_LOGW(TAG, "VCM beacon 0x%03"PRIX32" — TX halted, SM->SLEEP", id);
// }

/* ── 0x6C0 — Display Unit Settings ───────────────────────────────────────────────────── */
/* A frame that fails any check is dropped whole; previous settings stay in effect. */
static void handle_display_cmd(const can_rx_event_t *event)
{
    const uint8_t *d = event->data;

    if (event->dlc != 8u || d[7] != display_frame_csum(d)) {
        return;
    }
    for (int i = 2; i < 7; i++) {
        if (d[i] != 0u) {
            return;
        }
    }

    uint16_t volts = (uint16_t)((uint16_t)d[0] * 2u);
    uint8_t  soc   = d[1];
    if (volts < SM_TARGET_VOLTAGE_MIN_V || volts > SM_TARGET_VOLTAGE_MAX_V ||
        soc   < SM_TARGET_SOC_MIN_PCT   || soc   > SM_TARGET_SOC_MAX_PCT) {
        return;
    }

    g_sm_target_voltage_v = volts;
    g_sm_target_soc_pct   = soc;
}

/* ── TWAI1 RX frames — OBC alive signal ──────────────────────────────────── */
static void handle_twai1_frame(void)
{
    g_sm_twai1_rxd = true;
}

/* ── Dispatch ─────────────────────────────────────────────────────────────── */
void dispatch_can_message(const can_rx_event_t *event)
{
    if (event->bus == twai1_hdl) {
        /* Any TWAI1 frame signals OBC alive (EVSE_ENABLE → CHARGING trigger) */
        handle_twai1_frame();
        return;
    }

    /* TWAI0 frames */
    switch (event->id) {
        case 0x50Bu: handle_50b(event->data); break;
        case 0x1F2u: handle_1f2(event->data); break;
        case 0x1DBu: handle_1db(event->data); break;
        case 0x1D4u: handle_1d4(event->data); break;
        case 0x55Bu: handle_55b(event->data); break;
        // case 0x603u: /* fall-through */
        // case 0x68Cu: handle_vcm_beacon(event->id, event->data); break;
        case CAN_ID_DISPLAY_CMD: handle_display_cmd(event); break;
        default: break;
    }
}

void rx_dispatcher_task(void *pvParams)
{
    can_rx_event_t event;
    for (;;) {
        if (xQueueReceive(rx_queue, &event, portMAX_DELAY) == pdTRUE) {
            dispatch_can_message(&event);
        }
    }
}
