#include "twai_handler.h"
#include "state_machine.h"
#include "gpio_config.h"
#include "obc_frames.h"
#include "sm_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_twai_types.h"
#include "driver/gptimer.h"

static const char *TAG = "twai";

twai_node_handle_t twai0_hdl = NULL;
twai_node_handle_t twai1_hdl = NULL;

volatile bool DRAM_ATTR g_twai0_bus_off    = false;
volatile bool DRAM_ATTR g_twai1_bus_off    = false;
volatile bool DRAM_ATTR g_twai0_tx_enabled = false;

QueueHandle_t DRAM_ATTR rx_queue = NULL;

/* ── TX Frame Data Buffers ────────────────────────────────────────────────
 * DRAM_ATTR: accessible from the gptimer alarm ISR when Flash cache is off.
 * Fully pre-initialised so the first transmitted frame has valid static fields.
 * Dynamic fields (relay bit, status, J1772 current, charge enable) are updated
 * by the state machine task between transmissions.
 * ────────────────────────────────────────────────────────────────────────── */
/* 0x380 OBC Status — d[0]=0x02 fixed, d[1]=0x04 fixed,
 * d[2]=0x10 (mode flag | power MSB=0), d[3]=0x21 (33 = 3300 W / 100)      */
#define FRAME_380_DEFAULT { 0x02, 0x04, 0x10, 0x21, 0x00, 0x00, 0x00, 0x00 }
/* 0x5BF OBC Charge State — d[4]=0x28 (IDLE) at power-up                    */
#define FRAME_5BF_DEFAULT { 0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00 }
DRAM_ATTR uint8_t frame_380_data[8] = FRAME_380_DEFAULT;
DRAM_ATTR uint8_t frame_5bf_data[8] = FRAME_5BF_DEFAULT;
/* 0x050 Lear_Control — d[1]=0xDC always; other dynamic fields set in SM    */
DRAM_ATTR uint8_t frame_050_data[8] = { 0x00, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
/* 0x6C1 display advert — d[0]/d[1]/d[7] refreshed by the gptimer ISR; d[2..6] stay 0 */
static DRAM_ATTR uint8_t frame_6c1_data[8] = { 0 };

static const uint8_t k_frame_380_default[8] = FRAME_380_DEFAULT;
static const uint8_t k_frame_5bf_default[8] = FRAME_5BF_DEFAULT;

DRAM_ATTR portMUX_TYPE g_twai0_frame_lock = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR portMUX_TYPE g_frame050_lock    = portMUX_INITIALIZER_UNLOCKED;

/* Private TX copies.  0x5BF queues behind 0x380 and is read by the driver after
 * the gptimer ISR returns, so only the ISR (every 100 ms) may write these. */
static DRAM_ATTR uint8_t s_tx_380[8];
static DRAM_ATTR uint8_t s_tx_5bf[8];
static uint8_t s_tx_050[8];

/* twai_frame_t structs — buffer pointers set in init functions below */
static DRAM_ATTR twai_frame_t frame_380;
static DRAM_ATTR twai_frame_t frame_5bf;
static DRAM_ATTR twai_frame_t frame_050;
static DRAM_ATTR twai_frame_t frame_6c1;

/* ── ISR-safe log tag ────────────────────────────────────────────────────── */
static DRAM_ATTR const char TWAI_ISR_TAG[] = "twai";

/* ── Bus-error log rate limit ────────────────────────────────────────────────
 * An unacknowledged frame is retried every ≈ 0.3 ms and each retry raises a bus error.
 * One blocking ROM print per error (≈ 2 ms at 115200 baud) keeps this level-3 ISR busy
 * back to back and starves every task, so each bus logs at most once per period with
 * the number of errors and the OR of their flags since the previous line. */
#define TWAI_ERR_LOG_PERIOD_MS  1000u

typedef struct {
    TickType_t last;
    uint32_t   count;
    uint32_t   flags;
    bool       logged;
} twai_err_log_t;

static DRAM_ATTR twai_err_log_t s_twai0_err_log;
static DRAM_ATTR twai_err_log_t s_twai1_err_log;

static inline bool IRAM_ATTR twai_err_log_due(twai_err_log_t *s, uint32_t flags)
{
    TickType_t now = xTaskGetTickCountFromISR();
    s->count++;
    s->flags |= flags;
    if (s->logged && (now - s->last) < pdMS_TO_TICKS(TWAI_ERR_LOG_PERIOD_MS)) {
        return false;
    }
    s->last   = now;
    s->logged = true;
    return true;
}

static inline void IRAM_ATTR twai_err_log_reset(twai_err_log_t *s)
{
    s->count = 0;
    s->flags = 0;
}

/* ── TWAI0 bus-off recovery task ─────────────────────────────────────────── */
static TaskHandle_t s_twai0_recovery_task_hdl = NULL;

static void twai0_recovery_task(void *pvParams)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        twai_node_recover(twai0_hdl);
    }
}

/* ── Software ID filters ─────────────────────────────────────────────────── */
static inline bool IRAM_ATTR twai0_id_accepted(uint32_t id)
{
    return (id == 0x1D4u || id == 0x1DAu || id == 0x1DBu || id == 0x1DCu || id == 0x1F2u ||
            id == 0x50Bu || id == 0x55Bu || id == 0x5BCu || id == 0x603u || id == 0x68Cu ||
            id == CAN_ID_DISPLAY_CMD);
}

static inline bool IRAM_ATTR twai1_id_accepted(uint32_t id)
{
    return SM_LOG_LEAR_RAW || TWAI1_ID_IS_CORE(id);
}

/* Raw-capture frames (non-core IDs) are best effort: they are dropped while the shared queue is this full,
 * so an unexpectedly chatty Lear ID can never crowd out the TWAI0 frames the state machine needs. */
#define TWAI1_RAW_QUEUE_LIMIT   8u

/* ── TWAI0 ISR callbacks ─────────────────────────────────────────────────── */
static bool IRAM_ATTR twai0_rx_done_cb(twai_node_handle_t handle,
                                        const twai_rx_done_event_data_t *edata,
                                        void *user_ctx)
{
    uint8_t buf[8] = {0};
    twai_frame_t frame = { .buffer = buf, .buffer_len = sizeof(buf) };
    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK) {
        return false;
    }
    if (!twai0_id_accepted(frame.header.id)) {
        return false;
    }
    can_rx_event_t evt = { .bus = handle, .id = frame.header.id,
                           .dlc = (uint8_t)frame.header.dlc };
    uint8_t n = (evt.dlc < 8u) ? evt.dlc : 8u;
    for (uint8_t i = 0; i < n; i++) { evt.data[i] = buf[i]; }
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(rx_queue, &evt, &woken);
    portYIELD_FROM_ISR(woken);
    return (woken == pdTRUE);
}

static bool IRAM_ATTR twai0_state_change_cb(twai_node_handle_t handle,
                                             const twai_state_change_event_data_t *edata,
                                             void *user_ctx)
{
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        g_twai0_bus_off = true;
        BaseType_t woken = pdFALSE;
        xTaskNotifyFromISR(s_twai0_recovery_task_hdl, 0, eNoAction, &woken);
        portYIELD_FROM_ISR(woken);
        return (woken == pdTRUE);
    }
    return false;
}

static bool IRAM_ATTR twai0_error_cb(twai_node_handle_t handle,
                                      const twai_error_event_data_t *edata,
                                      void *user_ctx)
{
    /* Skip log if arb_lost is the only flag — normal CAN bus event */
    if (edata->err_flags.val != 0x1u &&
        twai_err_log_due(&s_twai0_err_log, edata->err_flags.val)) {
        ESP_DRAM_LOGW(TWAI_ISR_TAG, "twai0 err 0x%x (%u since last report)",
                      (unsigned int)s_twai0_err_log.flags, (unsigned int)s_twai0_err_log.count);
        twai_err_log_reset(&s_twai0_err_log);
    }
    return false;
}

/* ── TWAI1 ISR callbacks ─────────────────────────────────────────────────── */
static bool IRAM_ATTR twai1_rx_done_cb(twai_node_handle_t handle,
                                        const twai_rx_done_event_data_t *edata,
                                        void *user_ctx)
{
    uint8_t buf[8] = {0};
    twai_frame_t frame = { .buffer = buf, .buffer_len = sizeof(buf) };
    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK) {
        return false;
    }
    if (!twai1_id_accepted(frame.header.id)) {
        return false;
    }
    if (!TWAI1_ID_IS_CORE(frame.header.id) && uxQueueMessagesWaitingFromISR(rx_queue) >= TWAI1_RAW_QUEUE_LIMIT) {
        return false;
    }
    can_rx_event_t evt = { .bus = handle, .id = frame.header.id,
                           .dlc = (uint8_t)frame.header.dlc };
    uint8_t n = (evt.dlc < 8u) ? evt.dlc : 8u;
    for (uint8_t i = 0; i < n; i++) { evt.data[i] = buf[i]; }
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(rx_queue, &evt, &woken);
    portYIELD_FROM_ISR(woken);
    return (woken == pdTRUE);
}

static bool IRAM_ATTR twai1_state_change_cb(twai_node_handle_t handle,
                                             const twai_state_change_event_data_t *edata,
                                             void *user_ctx)
{
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        g_twai1_bus_off = true;
    }
    return false;
}

static bool IRAM_ATTR twai1_error_cb(twai_node_handle_t handle,
                                      const twai_error_event_data_t *edata,
                                      void *user_ctx)
{
    if (twai_err_log_due(&s_twai1_err_log, edata->err_flags.val)) {
        ESP_DRAM_LOGW(TWAI_ISR_TAG, "twai1 err 0x%x (%u since last report)",
                      (unsigned int)s_twai1_err_log.flags, (unsigned int)s_twai1_err_log.count);
        twai_err_log_reset(&s_twai1_err_log);
    }
    return false;
}

/* ── gptimer alarm ISR — fires every 100 ms ─────────────────────────────── */
static bool IRAM_ATTR gptimer_alarm_cb(gptimer_handle_t timer,
                                        const gptimer_alarm_event_data_t *edata,
                                        void *user_data)
{
    if (!g_twai0_tx_enabled) {
        return false;
    }

    /* Snapshot the application buffers; the SM holds the same lock for multi-byte updates */
    portENTER_CRITICAL_ISR(&g_twai0_frame_lock);
    for (int i = 0; i < 8; i++) {
        s_tx_380[i] = frame_380_data[i];
        s_tx_5bf[i] = frame_5bf_data[i];
    }
    portEXIT_CRITICAL_ISR(&g_twai0_frame_lock);

    /* PRUN in d[7] bits [5:4] then CSUM in [3:0]; d[7] bits [7:6] come from the SM.
     * CSUM = (S − 5) for 0x380 and (S − 1) for 0x5BF, S including the d[7] high nibble. */
    static DRAM_ATTR uint8_t prun_380 = 0;
    prun_380 = (prun_380 + 1u) & 0x03u;
    obc_frame_seal(s_tx_380, prun_380, OBC380_CSUM_OFFSET);

    static DRAM_ATTR uint8_t prun_5bf = 0;
    prun_5bf = (prun_5bf + 1u) & 0x03u;
    obc_frame_seal(s_tx_5bf, prun_5bf, OBC5BF_CSUM_OFFSET);

    twai_node_transmit(twai0_hdl, &frame_380, 0);
    twai_node_transmit(twai0_hdl, &frame_5bf, 0);

    /* ── 0x6C1 display advert — every 5th tick (500 ms) ── */
    static DRAM_ATTR uint8_t advert_ticks = 0;
    if (++advert_ticks >= 5u) {
        advert_ticks = 0;
        frame_6c1_data[0] = (uint8_t)(g_sm_target_voltage_v >> 1);
        frame_6c1_data[1] = g_sm_target_soc_pct;
        frame_6c1_data[7] = display_frame_csum(frame_6c1_data);
        twai_node_transmit(twai0_hdl, &frame_6c1, 0);
    }
    return false;
}

void twai_handler_init(void)
{
    /* Recovery task — highest priority so it preempts immediately on bus-off notify */
    xTaskCreate(twai0_recovery_task, "twai0_rec", 1024, NULL,
                configMAX_PRIORITIES - 1, &s_twai0_recovery_task_hdl);

    /* ── TWAI0 ────────────────────────────────────────────────────────── */
    twai_onchip_node_config_t twai0_cfg = {
        .io_cfg = {
            .tx                = GPIO_TWAI0_TX,
            .rx                = GPIO_TWAI0_RX,
            .quanta_clk_out    = -1,
            .bus_off_indicator = -1,
        },
        .clk_src        = TWAI_CLK_SRC_DEFAULT,
        .bit_timing     = { .bitrate = 500000 },
        .tx_queue_depth = 8,
        .fail_retry_cnt = -1,
        .intr_priority  = 3,
        .flags          = { .no_receive_rtr = 1 },
    };
    ESP_ERROR_CHECK(twai_new_node_onchip(&twai0_cfg, &twai0_hdl));

    /* Second filter passes 0x400-0x7FF so 0x6C0 arrives; twai0_id_accepted() trims the rest */
    twai_mask_filter_config_t twai0_filter =
        twai_make_dual_filter(0x1D0, 0x7D0, 0x400, 0x400, false);
    ESP_ERROR_CHECK(twai_node_config_mask_filter(twai0_hdl, 0, &twai0_filter));

    twai_event_callbacks_t twai0_cbs = {
        .on_rx_done      = twai0_rx_done_cb,
        .on_state_change = twai0_state_change_cb,
        .on_error        = twai0_error_cb,
    };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(twai0_hdl, &twai0_cbs, NULL));

    /* ── TWAI1 ────────────────────────────────────────────────────────── */
    twai_onchip_node_config_t twai1_cfg = {
        .io_cfg = {
            .tx                = GPIO_TWAI1_TX,
            .rx                = GPIO_TWAI1_RX,
            .quanta_clk_out    = -1,
            .bus_off_indicator = -1,
        },
        .clk_src        = TWAI_CLK_SRC_DEFAULT,
        .bit_timing     = { .bitrate = 500000 },
        .tx_queue_depth = 8,
        .fail_retry_cnt = -1,
        .intr_priority  = 3,
        .flags          = { .no_receive_rtr = 1 },
    };
    ESP_ERROR_CHECK(twai_new_node_onchip(&twai1_cfg, &twai1_hdl));

    /* Raw capture accepts every ID (mask 0 = don't care); otherwise only 0x270–0x27F and 0x610–0x61F */
    twai_mask_filter_config_t twai1_filter = SM_LOG_LEAR_RAW
        ? twai_make_dual_filter(0x000, 0x000, 0x000, 0x000, false)
        : twai_make_dual_filter(0x270, 0x7F0, 0x610, 0x7F0, false);
    ESP_ERROR_CHECK(twai_node_config_mask_filter(twai1_hdl, 0, &twai1_filter));

    twai_event_callbacks_t twai1_cbs = {
        .on_rx_done      = twai1_rx_done_cb,
        .on_state_change = twai1_state_change_cb,
        .on_error        = twai1_error_cb,
    };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(twai1_hdl, &twai1_cbs, NULL));

    /* ── frame_050 buffer pointer (private copy, see twai1_tx_frame_050) ── */
    frame_050 = (twai_frame_t){
        .header     = { .id = 0x050u, .dlc = 8 },
        .buffer     = s_tx_050,
        .buffer_len = 8,
    };
}

void gptimer_init_twai0_tx(void)
{
    /* Initialise TWAI0 TX frame buffer pointers before the ISR can fire */
    frame_380 = (twai_frame_t){
        .header     = { .id = 0x380u, .dlc = 8 },
        .buffer     = s_tx_380,
        .buffer_len = 8,
    };
    frame_5bf = (twai_frame_t){
        .header     = { .id = 0x5BFu, .dlc = 8 },
        .buffer     = s_tx_5bf,
        .buffer_len = 8,
    };
    frame_6c1 = (twai_frame_t){
        .header     = { .id = CAN_ID_DISPLAY_ECHO, .dlc = 8 },
        .buffer     = frame_6c1_data,
        .buffer_len = 8,
    };

    gptimer_handle_t timer = NULL;

    gptimer_config_t timer_cfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,   /* 1 MHz → 1 µs per tick */
        .intr_priority = 3,         /* must match TWAI intr_priority — prevents TWAI ISR
                                     * (level 3) from preempting this ISR while it holds
                                     * the TWAI driver spinlock inside twai_node_transmit() */
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_cfg, &timer));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = gptimer_alarm_cb,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer, &cbs, NULL));

    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count                = 100000,   /* 100 ms */
        .reload_count               = 0,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(timer, &alarm_cfg));
    ESP_ERROR_CHECK(gptimer_enable(timer));
    ESP_ERROR_CHECK(gptimer_start(timer));
}

void twai_handler_enable(void)
{
    ESP_ERROR_CHECK(twai_node_enable(twai0_hdl));
    ESP_ERROR_CHECK(twai_node_enable(twai1_hdl));
}

void twai1_tx_frame_050(void)
{
    portENTER_CRITICAL(&g_frame050_lock);
    for (int i = 0; i < 8; i++) {
        s_tx_050[i] = frame_050_data[i];
    }
    portEXIT_CRITICAL(&g_frame050_lock);

    if (twai_node_transmit(twai1_hdl, &frame_050, pdMS_TO_TICKS(10)) != ESP_OK) {
        ESP_LOGW(TAG, "twai1 0x050 TX dropped");
    }
}

void twai0_frames_reset(void)
{
    portENTER_CRITICAL(&g_twai0_frame_lock);
    for (int i = 0; i < 8; i++) {
        frame_380_data[i] = k_frame_380_default[i];
        frame_5bf_data[i] = k_frame_5bf_default[i];
    }
    portEXIT_CRITICAL(&g_twai0_frame_lock);
}

/* The driver reads a queued frame's buffer when it starts the transmission, so patching
 * s_tx_050 reaches every 0x050 frame still waiting in the queue. */
void twai1_frame_050_set_enable(bool on)
{
    portENTER_CRITICAL(&g_frame050_lock);
    lear050_set_enable(frame_050_data, on);
    if (!on) {
        lear050_set_enable(s_tx_050, false);
    }
    portEXIT_CRITICAL(&g_frame050_lock);
}
