#pragma once
#include <stdint.h>
#include "esp_adc/adc_cali.h"

/* Latest averaged ADC raw value — written from ADC ISR, read from state machine task */
extern volatile uint32_t DRAM_ATTR g_adc_raw_avg;

/* Calibration handle — used only in task context (not ISR-safe for concurrent callers) */
extern adc_cali_handle_t adc_cali_hdl;

/**
 * Initialise ADC continuous DMA, create calibration scheme, register on_conv_done ISR,
 * and start ADC conversion.
 */
void adc_reader_init(void);
