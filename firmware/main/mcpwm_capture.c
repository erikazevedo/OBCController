#include "mcpwm_capture.h"
#include "gpio_config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/mcpwm_cap.h"
#include "esp_timer.h"

static const char *TAG = "mcpwm";

volatile uint32_t DRAM_ATTR g_pwm_duty_us    = 0;
volatile uint32_t DRAM_ATTR g_pwm_period_us  = 0;
volatile bool     DRAM_ATTR g_pwm_signal_valid = false;

static esp_timer_handle_t pwm_watchdog = NULL;

/* Last rising-edge timestamp (µs from esp_timer_get_time, 32-bit truncation).
 * Written atomically from the IRAM ISR; read by pwm_watchdog_cb in task context. */
static DRAM_ATTR volatile uint32_t s_last_rise_us = 0;

static void pwm_watchdog_cb(void *arg)
{
    /* esp_timer_restart() is not ISR-safe in IDF v6.0.2 — use periodic timer
     * with a staleness check instead of one-shot restart from ISR. */
    if ((uint32_t)esp_timer_get_time() - s_last_rise_us > 3000U) {
        g_pwm_duty_us      = 0;
        g_pwm_period_us    = 0;
        g_pwm_signal_valid = false;
    }
}

static bool IRAM_ATTR on_cap(mcpwm_cap_channel_handle_t ch,
                              const mcpwm_capture_event_data_t *edata,
                              void *user_data)
{
    static uint32_t s_rising_tick = 0;

    if (edata->cap_edge == MCPWM_CAP_EDGE_POS) {
        if (s_rising_tick != 0) {
            g_pwm_period_us = edata->cap_value - s_rising_tick;
        }
        s_rising_tick  = edata->cap_value;
        s_last_rise_us = (uint32_t)esp_timer_get_time();
    } else {
        if (s_rising_tick != 0) {
            g_pwm_duty_us      = edata->cap_value - s_rising_tick;
            g_pwm_signal_valid = true;
        }
    }
    return false;
}

void mcpwm_capture_init(void)
{
    /* ── Capture timer: group 0, 1 MHz (1 µs/tick) ───────────────────────── */
    mcpwm_cap_timer_handle_t cap_timer = NULL;
    mcpwm_capture_timer_config_t cap_timer_cfg = {
        .group_id      = 0,
        .clk_src       = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
    };
    ESP_ERROR_CHECK(mcpwm_new_capture_timer(&cap_timer_cfg, &cap_timer));

    /* ── Capture channel 0: both edges, no prescale ────────────────────── */
    mcpwm_cap_channel_handle_t cap_chan = NULL;
    mcpwm_capture_channel_config_t cap_chan_cfg = {
        .gpio_num      = GPIO_PWM_CAPTURE,
        .prescale      = 1,
        .intr_priority = 0,
        .flags = {
            .pos_edge          = 1,
            .neg_edge          = 1,
            .invert_cap_signal = 0,
        },
    };
    ESP_ERROR_CHECK(mcpwm_new_capture_channel(cap_timer, &cap_chan_cfg, &cap_chan));

    /* External pull resistor on PCB — configure GPIO as floating */
    ESP_ERROR_CHECK(gpio_set_pull_mode(GPIO_PWM_CAPTURE, GPIO_FLOATING));

    /* ── Register callback, enable, start ──────────────────────────────── */
    mcpwm_capture_event_callbacks_t cbs = {
        .on_cap = on_cap,
    };
    ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(cap_chan, &cbs, NULL));
    ESP_ERROR_CHECK(mcpwm_capture_channel_enable(cap_chan));
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(cap_timer));
    ESP_ERROR_CHECK(mcpwm_capture_timer_start(cap_timer));

    /* ── Signal-absence watchdog: 3 ms periodic, staleness-checked in callback ── */
    esp_timer_create_args_t wd_args = {
        .callback = pwm_watchdog_cb,
        .name     = "pwm_wd",
    };
    ESP_ERROR_CHECK(esp_timer_create(&wd_args, &pwm_watchdog));
    ESP_ERROR_CHECK(esp_timer_start_periodic(pwm_watchdog, 3000));
}
