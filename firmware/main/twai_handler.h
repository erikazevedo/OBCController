#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"
#include "esp_twai.h"
#include "esp_twai_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Opaque TWAI node handles — defined in twai_handler.c */
extern twai_node_handle_t twai0_hdl;
extern twai_node_handle_t twai1_hdl;

/* Bus-off / error flags — written from ISR, read from state machine task */
extern volatile bool g_twai0_bus_off;
extern volatile bool g_twai1_bus_off;

/* TX-enable gate — read from gptimer alarm ISR; written from RX dispatcher */
extern volatile bool g_twai0_tx_enabled;

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
 * Application-side content of the TWAI0 and TWAI1 TX frames, written by the
 * state machine task.  The driver keeps a pointer to the buffer until a frame
 * is actually on the bus, so the transmitters never send these arrays directly:
 * the gptimer ISR (TWAI0) and the TWAI1 TX task copy them into private TX
 * buffers first.  Multi-byte updates MUST hold the matching lock so a copy
 * never sees half a field; single-byte stores need no lock.
 * DRAM_ATTR ensures accessibility when the Flash cache is disabled.
 * ---------------------------------------------------------------------------*/
extern uint8_t frame_380_data[8];   /* 0x380  OBC Status        — g_twai0_frame_lock */
extern uint8_t frame_5bf_data[8];   /* 0x5BF  OBC Charge State  — g_twai0_frame_lock */
extern uint8_t frame_050_data[8];   /* 0x050  Lear_Control      — g_frame050_lock    */

extern portMUX_TYPE g_twai0_frame_lock;
extern portMUX_TYPE g_frame050_lock;

/** Restore frame_380_data / frame_5bf_data to their power-up content (bypass defaults). */
void twai0_frames_reset(void);

/**
 * Set Lear_ChargeEnable in frame_050_data.  A disable also patches the private TX copy,
 * so 0x050 frames already queued in the driver go out disabled too.
 */
void twai1_frame_050_set_enable(bool on);

/* ── Custom Display Unit Frames (TWAI0) ───────────────────────────────────────────────
 * Not part of the stock ZE0 EV-CAN bus; 0x6C0/0x6C1 were not seen in any
 * captured log or DBC.  Both frames share one layout:
 *   d[0]    target voltage, raw * 2 V
 *   d[1]    target SOC, 1 % per bit
 *   d[2..6] reserved, 0
 *   d[7]    checksum = DISPLAY_CSUM_SEED ^ d[0] ^ ... ^ d[6]
 *   0x6C0   display -> OBCController: overwrite g_sm_target_voltage_v / g_sm_target_soc_pct
 *   0x6C1   OBCController -> display: current values, every 500 ms while TX is enabled
 * ---------------------------------------------------------------------------*/
#define CAN_ID_DISPLAY_CMD      0x6C0u
#define CAN_ID_DISPLAY_ECHO     0x6C1u
#define DISPLAY_CSUM_SEED       0xA5u

/* Seed makes an all-zero frame invalid. Inlined so it is safe inside the gptimer ISR. */
static inline __attribute__((always_inline)) uint8_t display_frame_csum(const uint8_t *d)
{
    uint8_t c = DISPLAY_CSUM_SEED;
    for (int i = 0; i < 7; i++) {
        c ^= d[i];
    }
    return c;
}

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
 * Copies frame_050_data under g_frame050_lock into a private TX buffer, then
 * waits up to 10 ms if the TX queue is full before dropping the frame.
 */
void twai1_tx_frame_050(void);
