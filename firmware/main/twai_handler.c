#include "twai_handler.h"
#include "gpio_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/twai.h"

static const char *TAG = "twai";

twai_node_handle_t twai0_hdl = NULL;
twai_node_handle_t twai1_hdl = NULL;

volatile bool DRAM_ATTR g_twai0_bus_off = false;
volatile bool DRAM_ATTR g_twai1_bus_off = false;

QueueHandle_t DRAM_ATTR rx_queue = NULL;

void twai_handler_init(void)
{
    /* TODO: configure and install TWAI0 and TWAI1 nodes,
     * apply acceptance filters, and register on_rx_done / on_bus_off callbacks. */
}

void twai_handler_enable(void)
{
    /* TODO: call twai_node_enable() for twai0_hdl and twai1_hdl. */
}

void twai1_tx_frame_050(void)
{
    /* TODO: build and transmit CAN ID 0x050 on TWAI1. */
}
