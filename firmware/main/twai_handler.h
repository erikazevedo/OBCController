#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/twai.h"

/* Opaque TWAI node handles — defined in twai_handler.c */
extern twai_node_handle_t twai0_hdl;
extern twai_node_handle_t twai1_hdl;

/* Bus-off / error flags — written from ISR, read from state machine task */
extern volatile bool DRAM_ATTR g_twai0_bus_off;
extern volatile bool DRAM_ATTR g_twai1_bus_off;

/* RX event passed through the shared queue */
typedef struct {
    twai_node_handle_t bus;
    uint32_t           id;
    uint8_t            data[8];
    uint8_t            dlc;
} can_rx_event_t;

/* Shared RX queue — created in app_main() before twai_handler_init() */
extern QueueHandle_t DRAM_ATTR rx_queue;

/**
 * Initialise TWAI0 and TWAI1 nodes, apply acceptance filters, and register
 * all event callbacks.  Must be called after rx_queue is created.
 */
void twai_handler_init(void);

/**
 * Enable both TWAI nodes (connect to bus).  Must be called after all tasks
 * have been created, as the final step in app_main().
 */
void twai_handler_enable(void);

/**
 * Transmit a frame on TWAI1 (called from TWAI1 TX task).
 */
void twai1_tx_frame_050(void);
