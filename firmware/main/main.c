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

static const char *TAG = "main";

/* TWAI1 TX task handle — suspend/resume controlled by state_machine_task */
TaskHandle_t twai1_tx_task_hdl = NULL;

static void twai1_tx_task(void *pvParams)
{
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        twai1_tx_frame_050();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));
    }
}

static void gpio_outputs_init(void)
{
    /* TODO: configure all digital output pins defined in gpio_config.h */
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

    /* 6. TODO: gptimer_init_twai0_tx() — 100 ms periodic TWAI0 TX alarm */

    /* 7. Create tasks in order of decreasing priority */
    xTaskCreate(rx_dispatcher_task, "rx_disp",  3072, NULL,
                configMAX_PRIORITIES - 2, &rx_dispatcher_task_hdl);

    xTaskCreate(twai1_tx_task,      "twai1_tx", 2048, NULL,
                configMAX_PRIORITIES - 3, &twai1_tx_task_hdl);

    xTaskCreate(state_machine_task, "state_sm", 4096, NULL,
                configMAX_PRIORITIES - 4, &state_machine_task_hdl);

    /* 8. Connect TWAI nodes to bus last — after all tasks exist */
    twai_handler_enable();
}