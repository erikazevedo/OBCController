#include "adc_reader.h"
#include "gpio_config.h"
#include "esp_log.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "adc";

volatile uint32_t DRAM_ATTR g_adc_raw_avg = 0;

adc_cali_handle_t adc_cali_hdl = NULL;

static adc_continuous_handle_t adc_cont_hdl = NULL;

/* Channel index validated at init via adc_continuous_io_to_channel().
 * DRAM_ATTR: read inside the IRAM ISR. */
static DRAM_ATTR adc_channel_t s_adc_channel = ADC_CHANNEL_2;

static bool IRAM_ATTR on_conv_done(adc_continuous_handle_t handle,
                                    const adc_continuous_evt_data_t *edata,
                                    void *user_data)
{
    uint32_t sum = 0;
    uint32_t count = 0;
    const uint8_t *p   = edata->conv_frame_buffer;
    const uint8_t *end = p + edata->size;

    while (p < end) {
        const adc_digi_output_data_t *sample = (const adc_digi_output_data_t *)p;
        if (sample->type2.channel == s_adc_channel) {
            sum += sample->type2.data;
            count++;
        }
        p += SOC_ADC_DIGI_DATA_BYTES_PER_CONV;
    }

    if (count > 0) {
        g_adc_raw_avg = sum / count;
    }
    return false;
}

void adc_reader_init(void)
{
    /* Validate GPIO → ADC unit/channel mapping at runtime */
    adc_unit_t    unit;
    adc_channel_t channel;
    ESP_ERROR_CHECK(adc_continuous_io_to_channel(GPIO_ADC_INPUT, &unit, &channel));
    s_adc_channel = channel;

    /* ── Create continuous DMA handle ────────────────────────────────── */
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = 256,   /* 4 × 64-byte frames */
        .conv_frame_size    = 64,    /* 16 samples × 4 bytes → 250 Hz effective */
        .flags.flush_pool   = 1,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_cont_hdl));

    /* ── Configure channel pattern ───────────────────────────────────── */
    adc_digi_pattern_config_t pattern = {
        .atten     = ADC_ATTEN_DB_12,
        .channel   = channel,
        .unit      = unit,
        .bit_width = ADC_BITWIDTH_12,
    };
    adc_continuous_config_t cont_cfg = {
        .pattern_num    = 1,
        .adc_pattern    = &pattern,
        .sample_freq_hz = 4000,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        /* .format is deprecated in IDF v6.0.2 — omitted */
    };
    ESP_ERROR_CHECK(adc_continuous_config(adc_cont_hdl, &cont_cfg));

    /* ── Curve-fitting calibration (only scheme supported on ESP32-C6) ────── */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = unit,
        .chan     = channel,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t cali_err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali_hdl);
    if (cali_err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "ADC cali not supported (eFuse not burned) — using raw counts");
        adc_cali_hdl = NULL;
    } else {
        ESP_ERROR_CHECK(cali_err);
    }

    /* ── Register callback and start ─────────────────────────────────── */
    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = on_conv_done,
        .on_pool_ovf  = NULL,
    };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(adc_cont_hdl, &cbs, NULL));
    ESP_ERROR_CHECK(adc_continuous_start(adc_cont_hdl));
}
