#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "twai_handler.h"

/* Handle exposed so app_main() stores it */
extern TaskHandle_t rx_dispatcher_task_hdl;

/**
 * FreeRTOS task body for the CAN RX dispatcher.
 * Priority: configMAX_PRIORITIES - 2 (23). Stack: 3072 bytes.
 * Blocks on rx_queue and dispatches frames to per-ID handler callbacks.
 */
void rx_dispatcher_task(void *pvParams);

/**
 * Route a received CAN frame to the appropriate handler based on bus and CAN ID.
 * Called exclusively from rx_dispatcher_task().
 */
void dispatch_can_message(const can_rx_event_t *event);
