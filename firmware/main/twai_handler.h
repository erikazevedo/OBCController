#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"
#include "esp_twai.h"
#include "esp_twai_types.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Opaque TWAI node handles — defined in twai_handler.c */
extern twai_node_handle_t twai0_hdl;
extern twai_node_handle_t twai1_hdl;

/* Bus-off / error flags — written from ISR, read from state machine task */
extern volatile bool g_twai0_bus_off;
extern volatile bool g_twai1_bus_off;

/* TX-enable gate — read from gptimer alarm ISR; written from esp_timer callback and RX dispatcher */
extern volatile bool g_twai0_tx_enabled;

/* Sentinel preventing the 5-second delay timer from restarting on duplicate RUN frames */
extern volatile bool g_twai0_tx_pending;

/* One-shot 5-second esp_timer — created once in twai_handler_init(), reused across RUN/SLEEP cycles */
extern esp_timer_handle_t twai0_tx_delay_timer;

/* RX event passed through the shared queue */
typedef struct {
    twai_node_handle_t bus;
    uint32_t           id;
    uint8_t            data[8];
    uint8_t            dlc;
} can_rx_event_t;

/* Shared RX queue — created in app_main() before twai_handler_init() */
extern QueueHandle_t rx_queue;

/* ── TX Frame Data Buffers ────────────────────────────────────────────────────
 * Backing byte arrays for the TWAI0 and TWAI1 TX frames.
 * Written by the state machine task between transmissions.
 * Read by the gptimer alarm ISR (TWAI0) and TWAI1 TX task via twai_node_transmit().
 * DRAM_ATTR ensures accessibility when the Flash cache is disabled.
 * ---------------------------------------------------------------------------*/
extern uint8_t frame_380_data[8];   /* 0x380  OBC Status        */
extern uint8_t frame_5bf_data[8];   /* 0x5BF  OBC Charge State  */
extern uint8_t frame_050_data[8];   /* 0x050  Lear_Control      */

/**
 * Initialise TWAI0 and TWAI1 nodes, apply acceptance filters, register
 * all event callbacks, and create twai0_tx_delay_timer.
 * Must be called after rx_queue is created.
 */
void twai_handler_init(void);

/**
 * Enable both TWAI nodes (connect to bus).  Must be called after all tasks
 * have been created, as the final step in app_main().
 */
void twai_handler_enable(void);

/**
 * Create and start the gptimer hardware alarm that fires every 100 ms to
 * transmit TWAI0 frames 0x380 and 0x5BF.  The ISR checks g_twai0_tx_enabled
 * before every transmission.  Must be called after twai_handler_init().
 */
void gptimer_init_twai0_tx(void);

/**
 * Transmit the current frame_050_data on TWAI1.  Called from the TWAI1 TX task.
 * Waits up to 10 ms if the TX queue is full before dropping the frame.
 */
void twai1_tx_frame_050(void);
