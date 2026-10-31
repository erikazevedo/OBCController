#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "gpio_config.h"
#include "twai_handler.h"
#include "mcpwm_capture.h"
#include "adc_reader.h"
#include "state_machine.h"
#include "rx_dispatcher.h"

/* TWAI1 TX task handle — suspend/resume controlled by state_machine_task */
TaskHandle_t twai1_tx_task_hdl = NULL;

static void twai1_tx_task(void *pvParams)
{
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        twai1_tx_frame_050();
        /* After a vTaskResume the deadline is long past: re-align instead of sending every missed period back-to-back */
        if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100)) == pdFALSE) {
            last_wake = xTaskGetTickCount();
        }
    }
}

static void gpio_outputs_init(void)
{
    const uint64_t output_mask =
        (1ULL << GPIO_CP_ENABLE)  |
        (1ULL << GPIO_PLUG_SIG)   |
        (1ULL << GPIO_STATUS_LED) |
        (1ULL << GPIO_EVA_REQ)    |
        (1ULL << GPIO_CONT_POS)   |
        (1ULL << GPIO_CONT_NEG);

    gpio_config_t cfg = {
        .pin_bit_mask = output_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    gpio_set_level(GPIO_CP_ENABLE,  0);
    gpio_set_level(GPIO_PLUG_SIG,   0);
    gpio_set_level(GPIO_STATUS_LED, 0);
    gpio_set_level(GPIO_EVA_REQ,    0);
    gpio_set_level(GPIO_CONT_POS,   0);
    gpio_set_level(GPIO_CONT_NEG,   0);
}

void app_main(void)
{
    /* 1. Safe GPIO output state before any peripheral starts */
    gpio_outputs_init();

    /* 2. RX queue — must exist before TWAI callbacks are registered */
    rx_queue = xQueueCreate(16, sizeof(can_rx_event_t));
    ESP_ERROR_CHECK(rx_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    /* 3. TWAI nodes, filters, and callbacks */
    twai_handler_init();

    /* 4. MCPWM capture timer, channel, and PWM watchdog */
    mcpwm_capture_init();

    /* 5. ADC continuous DMA, calibration, and start */
    adc_reader_init();

    /* 6. gptimer 100 ms periodic alarm — TWAI0 TX (gate-checked inside ISR) */
    gptimer_init_twai0_tx();

    /* 7. Create tasks in order of decreasing priority */
    xTaskCreate(rx_dispatcher_task, "rx_disp",  3072, NULL,
                configMAX_PRIORITIES - 2, &rx_dispatcher_task_hdl);

    xTaskCreate(twai1_tx_task,      "twai1_tx", 2048, NULL,
                configMAX_PRIORITIES - 3, &twai1_tx_task_hdl);
    vTaskSuspend(twai1_tx_task_hdl);   /* SLEEP state at boot — TWAI1 TX must be silent */

    xTaskCreate(state_machine_task, "state_sm", 4096, NULL,
                configMAX_PRIORITIES - 4, &state_machine_task_hdl);

    /* 8. Connect TWAI nodes to bus last — after all tasks exist */
    twai_handler_enable();
}