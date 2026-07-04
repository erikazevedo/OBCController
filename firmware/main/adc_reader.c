#include "adc_reader.h"
#include "gpio_config.h"
#include "esp_log.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "adc";

volatile uint32_t DRAM_ATTR g_adc_raw_avg = 0;

adc_cali_handle_t adc_cali_hdl = NULL;

static adc_continuous_handle_t adc_cont_hdl = NULL;

static bool IRAM_ATTR on_conv_done(adc_continuous_handle_t handle,
                                    const adc_continuous_evt_data_t *edata,
                                    void *user_data)
{
    /* TODO: average 16 raw 12-bit samples from edata->conv_frame_buffer,
     * write result to g_adc_raw_avg. */
    return false;
}

void adc_reader_init(void)
{
    /* TODO: configure ADC continuous DMA on ADC1_CHANNEL_2 (GPIO_ADC_INPUT),
     * create calibration scheme, register on_conv_done ISR,
     * call adc_continuous_start(). */
}
