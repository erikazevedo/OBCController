---
applyTo: "**/adc*"
---
# ADC Configuration — OBCController

> **API reference:** ESP-IDF v6.0.2 ADC Continuous (DMA) driver and ADC Calibration.
> Docs: https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32c6/api-reference/peripherals/adc_continuous.html
> https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32c6/api-reference/peripherals/adc_calibration.html
> Headers: `esp_adc/adc_continuous.h`, `esp_adc/adc_cali.h`, `esp_adc/adc_cali_scheme.h`

---

## Hardware Capability Summary (ESP32-C6)

| Property | Value |
|---|---|
| ADC units | 1 (`ADC_UNIT_1` only — no ADC2) |
| ADC-capable GPIOs | GPIO0–GPIO6 only (`ADC1_CHANNEL_0` through `ADC1_CHANNEL_6`) |
| Bit width in continuous mode | **12-bit only** (`SOC_ADC_DIGI_MAX_BITWIDTH = SOC_ADC_DIGI_MIN_BITWIDTH = 12`) |
| Sample frequency range | 611 Hz – 83 333 Hz (`SOC_ADC_SAMPLE_FREQ_THRES_LOW/HIGH`) |
| Bytes per conversion result | 4 (`SOC_ADC_DIGI_DATA_BYTES_PER_CONV = 4`) |
| Calibration scheme | **Curve Fitting only** (`ADC_CALI_SCHEME_VER_CURVE_FITTING`) |
| Raw output range | 0–4095 |

**Hardware limitation:** Line fitting calibration (`ADC_CALI_SCHEME_VER_LINE_FITTING`)
is NOT supported on ESP32-C6. Only `adc_cali_create_scheme_curve_fitting()` must be used.

---

## Channel Assignment

| Property | Value |
|---|---|
| GPIO | `GPIO_ADC_INPUT` (`GPIO_NUM_2`) — from `gpio_config.h` |
| ADC unit | `ADC_UNIT_1` |
| Channel | `ADC_CHANNEL_2` (`ADC1_CHANNEL_2`) |
| Attenuation | `ADC_ATTEN_DB_12` (input range 0–3.1 V, full 3.3 V rail) |

Use `adc_continuous_io_to_channel(GPIO_ADC_INPUT, &unit, &channel)` at init time to
validate the mapping at runtime rather than hardcoding unit/channel values.

**Hardware limitation:** `ADC_ATTEN_DB_12` introduces nonlinearity near the top of the
input range (approaching Vcc). If the signal being measured is expected to sit near 3.3 V
frequently, account for this in the threshold comparison or apply curve-fitting
calibration (which partially corrects nonlinearity).

---

## REQUIRED: Handle and Continuous Config

### Step 1 — Create Handle

```c
adc_continuous_handle_cfg_t handle_cfg = {
    .max_store_buf_size = 256,   // 4 frames × 64 bytes — absorbs task scheduling jitter
    .conv_frame_size    = 64,    // 16 samples × 4 bytes → fires on_conv_done at 250 Hz
    .flags.flush_pool   = 1,     // discard stale data when pool is full
};
adc_continuous_handle_t adc_hdl = NULL;
ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_hdl));
```

### Step 2 — Configure Channel Pattern

```c
adc_digi_pattern_config_t pattern = {
    .atten     = ADC_ATTEN_DB_12,
    .channel   = ADC_CHANNEL_2,
    .unit      = ADC_UNIT_1,
    .bit_width = ADC_BITWIDTH_DEFAULT,  // resolves to 12-bit on ESP32-C6
};

adc_continuous_config_t cont_cfg = {
    .pattern_num    = 1,
    .adc_pattern    = &pattern,
    .sample_freq_hz = 4000,                     // 4 kHz raw → 250 Hz effective after averaging
    .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
    .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
};
ESP_ERROR_CHECK(adc_continuous_config(adc_hdl, &cont_cfg));
```

**Effective update rate:** `sample_freq_hz / (conv_frame_size / SOC_ADC_DIGI_DATA_BYTES_PER_CONV)`
= 4000 / (64 / 4) = 4000 / 16 = **250 Hz** (one averaged reading every 4 ms).
This is sufficient to detect a resistor-divider button press within one FreeRTOS tick.

---

## REQUIRED: `on_conv_done` ISR Callback

The callback runs in **ISR context**. It MUST:
1. Average the 16 raw 12-bit samples in the frame.
2. Write the averaged raw count to a `DRAM_ATTR volatile` variable.
3. Return immediately — no blocking, no queue posting.

`adc_cali_raw_to_voltage()` MUST NOT be called from this ISR because the calibration
function is not guaranteed to reside in IRAM. Raw-to-mV conversion is performed in the
state machine task.

```c
// Shared output — DRAM_ATTR because accessed from IRAM ISR
static DRAM_ATTR volatile uint32_t g_adc_raw_avg = 0;

static IRAM_ATTR bool adc_conv_done_cb(adc_continuous_handle_t handle,
                                        const adc_continuous_evt_data_t *edata,
                                        void *user_data)
{
    uint32_t sum = 0;
    uint32_t count = 0;
    uint8_t *p = edata->conv_frame_buffer;
    uint8_t *end = p + edata->size;

    while (p < end) {
        adc_digi_output_data_t *sample = (adc_digi_output_data_t *)p;
        if (sample->type2.channel == ADC_CHANNEL_2) {
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
```

---

## REQUIRED: Calibration Setup

Create the calibration handle once at init. Store it for use by the state machine task.

```c
adc_cali_handle_t adc_cali_hdl = NULL;

adc_cali_curve_fitting_config_t cali_cfg = {
    .unit_id  = ADC_UNIT_1,
    .chan     = ADC_CHANNEL_2,
    .atten   = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
};
esp_err_t cali_err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali_hdl);
if (cali_err == ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(TAG, "ADC calibration not supported on this chip eFuse — using raw counts");
    adc_cali_hdl = NULL;
}
```

`adc_cali_raw_to_voltage()` is NOT thread-safe for the same handle across concurrent
callers. Since only the state machine task calls it, no mutex is needed. Do NOT call it
from any ISR.

---

## REQUIRED: State Machine Task Usage

```c
// In state machine task body:
int voltage_mv = 0;
if (adc_cali_hdl != NULL) {
    adc_cali_raw_to_voltage(adc_cali_hdl, (int)g_adc_raw_avg, &voltage_mv);
} else {
    voltage_mv = (int)g_adc_raw_avg;  // uncalibrated fallback
}

// Threshold comparison (example — thresholds defined as named constants):
bool plugged = (voltage_mv > ADC_PLUG_PRESENT_THRESHOLD_MV);
```

Define `ADC_PLUG_PRESENT_THRESHOLD_MV` as a named constant in `adc_reader.h`.
Do NOT use a magic number at the comparison site.

---

## REQUIRED: Startup Sequence

```c
// 1. Register callbacks (before starting)
adc_continuous_evt_cbs_t cbs = {
    .on_conv_done = adc_conv_done_cb,
    .on_pool_ovf  = NULL,   // pool overflow silently drops old data (flush_pool=1)
};
ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(adc_hdl, &cbs, NULL));

// 2. Start
ESP_ERROR_CHECK(adc_continuous_start(adc_hdl));
```

`adc_continuous_start()` acquires an `ESP_PM_APB_FREQ_MAX` lock internally if
`CONFIG_PM_ENABLE` is set. If power management is enabled in this project,
`adc_continuous_stop()` MUST be called symmetrically.

---

## REQUIRED: Kconfig

```
CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE=y
```

When set, the driver validates at callback registration that `on_conv_done` resides in
IRAM. The `IRAM_ATTR` annotation MUST be present on the callback function definition.
`conv_frame_buffer` is driver-owned DRAM; the ISR reads from it directly — valid.

---

## RECOMMENDED: `on_pool_ovf` Callback

Because `flags.flush_pool = 1` is set, old data is automatically discarded when the pool
fills. An `on_pool_ovf` callback is therefore optional; if registered it should only
increment a `DRAM_ATTR` overflow counter for diagnostic logging in the state machine task.
