#include "rx_dispatcher.h"
#include "twai_handler.h"
#include "state_machine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "rx_disp";

TaskHandle_t rx_dispatcher_task_hdl = NULL;

/* ── 0x50B — VCM Wake/Sleep command ──────────────────────────────────────── */
static void handle_50b(const uint8_t *data)
{
    uint8_t cmd = (data[3] >> 6) & 0x03u;
    if (cmd == 0x03u) {                                         /* RUN */
        g_sm_vcm_run = true;
        if (!g_twai0_tx_pending && !g_twai0_tx_enabled) {
            g_twai0_tx_pending = true;
            esp_timer_start_once(twai0_tx_delay_timer, 5000000ULL); /* 5 s */
        }
    } else if (cmd == 0x00u) {                                  /* SLEEP */
        g_sm_vcm_sleep     = true;
        g_twai0_tx_enabled = false;
        g_twai0_tx_pending = false;
        esp_timer_stop(twai0_tx_delay_timer);
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
