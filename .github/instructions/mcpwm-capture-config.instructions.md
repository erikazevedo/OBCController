---
applyTo: "**/mcpwm*"
---
# MCPWM Capture Configuration — OBCController

> **API reference:** ESP-IDF v6.0.2 MCPWM Capture driver.
> Docs: https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32c6/api-reference/peripherals/mcpwm.html
> Headers: `driver/mcpwm_cap.h`, `driver/mcpwm_types.h`

---

## Hardware Capability Summary (ESP32-C6)

| Property | Value |
|---|---|
| MCPWM groups | 1 (`MCPWM_LL_GROUP_NUM = 1`) |
| Capture timers per group | 1 |
| Capture channels per timer | 3 (channels 0, 1, 2) |
| Max capture timer clock | 160 MHz (from `SOC_MOD_CLK_PLL_F160M`, group prescale = 1) |
| Capture timer prescale | None — timer has no independent prescale (`MCPWM_LL_MAX_CAPTURE_TIMER_PRESCALE = 1`) |
| Per-channel input prescale | 1–256 (divides input signal edge frequency, NOT the timer clock) |
| Min hardware tick | 6.25 ns at 160 MHz group clock |

The capture submodule operates **independently of MCPWM operators and generators**.
No PWM output configuration is required.

---

## Signal Parameters

| Property | Value |
|---|---|
| Input GPIO | `GPIO_PWM_CAPTURE` (`GPIO_NUM_0`) — from `gpio_config.h` |
| Signal frequency | ~1 kHz (1 ms period) |
| Measurement targets | **Period** (time between rising edges) and **duty cycle** (high time / period) |
| Required resolution | ±5 µs — timer at 1 MHz (1 µs/tick) satisfies this with large margin |
| Signal absence | Detected via watchdog: no `on_cap` event within 3 ms → signal absent |

---

## REQUIRED: Timer Configuration

```c
mcpwm_capture_timer_config_t cap_timer_cfg = {
    .group_id  = 0,
    .clk_src   = MCPWM_CAPTURE_CLK_SRC_DEFAULT,  // 160 MHz PLL
    .resolution_hz = 1000000,                     // 1 MHz → 1 µs per tick
};
mcpwm_cap_timer_handle_t cap_timer = NULL;
ESP_ERROR_CHECK(mcpwm_new_capture_timer(&cap_timer_cfg, &cap_timer));
```

The group prescale required to achieve 1 MHz from 160 MHz is 160. The driver computes
this automatically from `resolution_hz`. Do NOT set `resolution_hz` higher than 1 MHz
for this application; the ±5 µs tolerance does not justify the additional tick wraparound
frequency.

---

## REQUIRED: Channel Configuration

Use **channel 0**. Configure both edges on the single channel to compute both period and
duty cycle without occupying a second channel.

```c
mcpwm_capture_channel_config_t cap_chan_cfg = {
    .gpio_num       = GPIO_PWM_CAPTURE,
    .prescale       = 1,      // capture every edge (no edge skipping needed at 1 kHz)
    .intr_priority  = 0,      // driver picks appropriate low priority
    .flags = {
        .pos_edge          = 1,   // capture rising edge
        .neg_edge          = 1,   // capture falling edge
        .invert_cap_signal = 0,
    },
};
mcpwm_cap_channel_handle_t cap_chan = NULL;
ESP_ERROR_CHECK(mcpwm_new_capture_channel(cap_timer, &cap_chan_cfg, &cap_chan));
```

GPIO pull configuration (internal pull-up or pull-down) MUST be set separately via
`gpio_set_pull_mode()` after channel creation — the v6.0.2 struct does NOT include
`flags.pull_up` / `flags.pull_down` despite what some doc text implies. Confirm the
external signal's idle state and configure accordingly.

---

## REQUIRED: `on_cap` ISR Callback

The callback fires once per captured edge. Both period and duty cycle are computed
inline. Results are written to `DRAM_ATTR` shared variables for the state machine task
to read.

```c
// Shared output variables — DRAM_ATTR because accessed from IRAM ISR
static DRAM_ATTR volatile uint32_t g_pwm_period_us   = 0;
static DRAM_ATTR volatile uint32_t g_pwm_duty_us     = 0;
static DRAM_ATTR volatile bool     g_pwm_signal_valid = false;

static IRAM_ATTR bool cap_on_edge(mcpwm_cap_channel_handle_t chan,
                                   const mcpwm_capture_event_data_t *edata,
                                   void *user_ctx)
{
    static uint32_t rising_tick = 0;
    static uint32_t prev_rising_tick = 0;

    if (edata->cap_edge == MCPWM_CAP_EDGE_POS) {
        // Rising edge: record timestamp, compute period from last rising edge
        if (prev_rising_tick != 0) {
            g_pwm_period_us = edata->cap_value - prev_rising_tick;
        }
        prev_rising_tick = rising_tick;
        rising_tick = edata->cap_value;
        // Reset the absence watchdog (notify via direct-to-task notification or flag)
        // See watchdog section below
    } else {
        // Falling edge: compute high time (duty) since last rising edge
        g_pwm_duty_us = edata->cap_value - rising_tick;
        g_pwm_signal_valid = true;
    }

    return false; // return true only if a higher-priority task was unblocked
}
```

**`cap_value` units:** ticks at the timer resolution (1 µs/tick at 1 MHz).
Call `mcpwm_capture_timer_get_resolution()` at init time to confirm the actual resolution
and store it for conversion if `resolution_hz` ever changes.

---

## REQUIRED: Signal Absence Watchdog

Use `esp_timer` to detect loss of PWM signal. Reset the timer on every rising-edge
capture event. If it fires, the signal has been absent for ≥ 3 ms (3 × expected period).

```c
static esp_timer_handle_t s_pwm_watchdog = NULL;

static void pwm_watchdog_cb(void *arg)
{
    // Called from esp_timer task context — NOT ISR
    g_pwm_signal_valid = false;
    g_pwm_period_us    = 0;
    g_pwm_duty_us      = 0;
}

// At init:
esp_timer_create_args_t wd_args = {
    .callback = pwm_watchdog_cb,
    .name     = "pwm_wd",
};
ESP_ERROR_CHECK(esp_timer_create(&wd_args, &s_pwm_watchdog));
ESP_ERROR_CHECK(esp_timer_start_once(s_pwm_watchdog, 3000)); // 3 ms in µs

// In on_cap rising-edge branch (ISR context):
// Use esp_timer_restart() which is ISR-safe in IDF v6
esp_timer_restart(s_pwm_watchdog, 3000);
```

**HARDWARE LIMITATION:** `esp_timer_restart()` is ISR-safe in IDF v5.1+ / v6.x when
called on a one-shot timer. Verify this is the case in v6.0.2 before use; if not,
use a `DRAM_ATTR` timestamp variable and check staleness in the state machine task.

---

## REQUIRED: Callback Registration and Startup Sequence

```c
mcpwm_capture_event_callbacks_t cbs = {
    .on_cap = cap_on_edge,
};
ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(cap_chan, &cbs, NULL));
ESP_ERROR_CHECK(mcpwm_capture_channel_enable(cap_chan));
ESP_ERROR_CHECK(mcpwm_capture_timer_enable(cap_timer));
ESP_ERROR_CHECK(mcpwm_capture_timer_start(cap_timer));
```

---

## REQUIRED: Kconfig

```
CONFIG_MCPWM_ISR_CACHE_SAFE=y
```

When this is set, the driver validates at registration time that `on_cap` resides in
IRAM (`esp_ptr_in_iram()` check). The `IRAM_ATTR` annotation on the callback function
definition is MANDATORY. User data passed via `user_ctx` must also reside in DRAM
(`DRAM_ATTR` or stack is NOT acceptable).

---

## State Machine Task: Consuming PWM Data

The state machine task reads `g_pwm_period_us`, `g_pwm_duty_us`, and `g_pwm_signal_valid`
directly. Because the ESP32-C6 is a 32-bit single-core processor, aligned 32-bit reads
and writes are atomic; no mutex is required for these three variables. The `volatile`
annotation is REQUIRED to prevent compiler caching.

Duty cycle to setpoint mapping and signal validity checks are performed in the state
machine task, NOT in the ISR.

---

## Hardware Limitation Flags

- **GPIO0 is an ADC-capable pin (ADC1_CHANNEL_0).** It is exclusively assigned to MCPWM
  capture and MUST NOT be used as an ADC input simultaneously.
- **GPIO0 is an LP IO pad** (low-power pad, GPIO0–GPIO7). It is fully usable as MCPWM
  capture input through the GPIO matrix.
- **The capture timer shares the MCPWM group clock.** Setting `group_id = 0` for the
  capture timer uses the same clock domain as any MCPWM operators in group 0. Since no
  operators are used in this project, there is no conflict.
- **Per-channel prescale divides input edge frequency, not timer clock.** Setting
  `prescale = 4` at 1 kHz would only capture every 4th edge (250 Hz effective). Keep
  `prescale = 1` for this application.
