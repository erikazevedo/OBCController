#include "rx_dispatcher.h"
#include "twai_handler.h"
#include "state_machine.h"
#include "sm_config.h"
#include "obc_frames.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "rx_disp";

TaskHandle_t rx_dispatcher_task_hdl = NULL;

/* ── 0x50B — VCM Wake/Sleep command ──────────────────────────────────────── */
/* d[3][7:6] is WakeUp (0b11) / GoToSleep (0b00).  d[2] is non-zero only while the
 * power switch is ON: a charge wake keeps d[2] = 0x00 for the whole session.
 * The run/sleep events and, in the bypass build, the TX gate follow d[2]. */
static void handle_50b(const uint8_t *data)
{
    static TickType_t s_last_rx = 0;
    TickType_t now   = xTaskGetTickCount();
    bool       awake = vcm50b_wake_cmd(data) == VCM50B_CMD_WAKE;

    if (awake && (!g_sm_rx.awake || (now - s_last_rx) > pdMS_TO_TICKS(SM_VCM_SILENT_MS))) {
        g_sm_rx.wake_count++;
    }
    s_last_rx        = now;
    g_sm_rx.awake    = awake;
    g_sm_rx.power_on = vcm50b_power_on(data);
    g_sm_rx.d2_50b   = data[2];
    g_sm_rx.d3_50b   = data[3];
    g_sm_rx.cnt_50b++;

    // vcm_run and vcm_sleep are flags that get latched and cleared out in every state machine cycle
    // so just set the one you want to true
    if (data[2] == 0x00u) {
        g_sm_vcm_sleep = true;
    } else {
        g_sm_vcm_run   = true;
    }
    /* Auth build: the state machine owns the gate (it must transmit during a charge wake) */
    if (SM_SKIP_VCM_AUTH) {
        g_twai0_tx_enabled = (data[2] != 0x00u);
    }
}

/* ── 0x1F2 — VCM Charge Command ──────────────────────────────────────────── */
static void handle_1f2(const uint8_t *data)
{
    g_sm_rx.req_1f2     = vcm1f2_request(data);
    g_sm_rx.power_cmd_w = vcm1f2_power_cmd_w(data);
    g_sm_rx.d0_1f2      = data[0];
    g_sm_rx.d1_1f2      = data[1];
    g_sm_rx.d2_1f2      = data[2];
    g_sm_rx.mode_1f2    = vcm1f2_mode(data);
    g_sm_rx.cnt_1f2++;
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
    g_sm_rx.hv_live        = vcm1d4_hv_live(data);
    g_sm_rx.relay_plus     = vcm1d4_relay_plus(data);
    g_sm_rx.charge_session = vcm1d4_charge_session(data);
    g_sm_rx.d6_1d4         = data[6];
    g_sm_rx.cnt_1d4++;
}

/* ── 0x1DA — Inverter HV bus voltage ─────────────────────────────────────── */
static void handle_1da(const uint8_t *data)
{
    if (inv1da_bus_valid(data)) {
        g_sm_rx.hv_bus_dv = inv1da_bus_dv(data);
        g_sm_rx.cnt_1da++;
    }
}

/* ── 0x55B — LBC State of Charge ─────────────────────────────────────────── */
static void handle_55b(const uint8_t *data)
{
    /* raw = 10-bit value; physical SOC (%) = raw / 10.0 */
    uint16_t raw = (uint16_t)(((uint16_t)data[0] << 2) | (data[1] >> 6));
    g_sm_soc_pct = (float)raw * 0.1f;
    if (raw < 0x3FFu) {           /* 0x3FF = not yet measured */
        g_sm_rx.soc_x10   = raw;
        g_sm_rx.soc_valid = true;
    }
}

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
/* The Lear broadcasts only while it has AC input, so any frame means AC present. */
static void handle_twai1_frame(const can_rx_event_t *event)
{
    g_sm_twai1_rxd = true;
    g_sm_rx.cnt_lear++;
    if (event->id == 0x617u && event->dlc >= 4u) {
        g_sm_rx.lear_dc_dv = lear617_dc_dv(event->data);
        g_sm_rx.lear_dc_da = lear617_dc_da(event->data);
        g_sm_rx.cnt_617++;
    }
}

/* ── Dispatch ─────────────────────────────────────────────────────────────── */
void dispatch_can_message(const can_rx_event_t *event)
{
    if (event->bus == twai1_hdl) {
        /* Any TWAI1 frame signals OBC alive (EVSE_ENABLE → CHARGING trigger) */
        handle_twai1_frame(event);
        return;
    }

    /* TWAI0 frames */
    switch (event->id) {
        case 0x50Bu: handle_50b(event->data); break;
        case 0x1F2u: handle_1f2(event->data); break;
        case 0x1DAu: handle_1da(event->data); break;
        case 0x1DBu: handle_1db(event->data); break;
        case 0x1D4u: handle_1d4(event->data); break;
        case 0x55Bu: handle_55b(event->data); break;
        case 0x603u: /* fall-through */
        case 0x68Cu: g_sm_rx.cnt_beacon++; break;
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
