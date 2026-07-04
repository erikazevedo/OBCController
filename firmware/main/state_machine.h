#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Handle exposed so app_main() can pass it to rx_dispatcher if needed */
extern TaskHandle_t state_machine_task_hdl;

/**
 * FreeRTOS task body for the application state machine.
 * Priority: configMAX_PRIORITIES - 4 (21). Stack: 4096 bytes.
 */
void state_machine_task(void *pvParams);
