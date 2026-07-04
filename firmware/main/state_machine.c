#include "state_machine.h"
#include "gpio_config.h"
#include "twai_handler.h"
#include "mcpwm_capture.h"
#include "adc_reader.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sm";

TaskHandle_t state_machine_task_hdl = NULL;

/* TWAI1 TX task handle — controlled (suspend/resume) from this task */
extern TaskHandle_t twai1_tx_task_hdl;

void state_machine_task(void *pvParams)
{
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        /* 1. Read sensor inputs — all are DRAM_ATTR volatile, no blocking */
        uint32_t raw_adc   = g_adc_raw_avg;
        uint32_t duty_us   = g_pwm_duty_us;
        uint32_t period_us = g_pwm_period_us;
        bool pwm_valid     = g_pwm_signal_valid;

        /* 2. Calibrate ADC (not ISR-safe — only called from task context) */
        int voltage_mv = 0;
        if (adc_cali_hdl) {
            adc_cali_raw_to_voltage(adc_cali_hdl, (int)raw_adc, &voltage_mv);
        }

        /* 3. Evaluate state transitions and drive GPIO outputs */
        /* TODO: implement application state machine logic */

        /* 4. De-assert contactors on bus-off or PWM absence */
        if (g_twai0_bus_off || !pwm_valid) {
            gpio_set_level(GPIO_CONT_POS, 0);
            gpio_set_level(GPIO_CONT_NEG, 0);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));
    }
}
