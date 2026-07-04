#include "rx_dispatcher.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "rx_disp";

TaskHandle_t rx_dispatcher_task_hdl = NULL;

void dispatch_can_message(const can_rx_event_t *event)
{
    /* TODO: route event->id (and event->bus) to the appropriate handler function.
     * Handlers may use FreeRTOS primitives to signal the state machine task. */
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
