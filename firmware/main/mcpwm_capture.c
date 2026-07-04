#include "mcpwm_capture.h"
#include "gpio_config.h"
#include "esp_log.h"
#include "driver/mcpwm_cap.h"
#include "esp_timer.h"

static const char *TAG = "mcpwm";

volatile uint32_t DRAM_ATTR g_pwm_duty_us    = 0;
volatile uint32_t DRAM_ATTR g_pwm_period_us  = 0;
volatile bool     DRAM_ATTR g_pwm_signal_valid = false;

static esp_timer_handle_t pwm_watchdog = NULL;

static void pwm_watchdog_cb(void *arg)
{
    g_pwm_duty_us     = 0;
    g_pwm_period_us   = 0;
    g_pwm_signal_valid = false;
}

static bool IRAM_ATTR on_cap(mcpwm_cap_channel_handle_t ch,
                              const mcpwm_capture_event_data_t *edata,
                              void *user_data)
{
    /* TODO: compute duty_us and period_us, update g_pwm_* globals,
     * restart pwm_watchdog. */
    return false;
}

void mcpwm_capture_init(void)
{
    /* TODO: create MCPWM capture timer and channel on GPIO_PWM_CAPTURE,
     * register on_cap ISR, create and start pwm_watchdog esp_timer. */
}
