---
applyTo: "**/*.c"
---
# Task Architecture — OBCController

> **API reference:** ESP-IDF v6.0.2 FreeRTOS and driver ISR APIs.
> FreeRTOS docs: https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32c6/api-reference/system/freertos.html
> gptimer docs: https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32c6/api-reference/peripherals/gptimer.html

---

## System Overview

The ESP32-C6 is a **single-core** RISC-V processor at 160 MHz. All FreeRTOS tasks and
ISRs share this one core. The architecture minimises task involvement in time-critical
paths by doing as much as possible in hardware ISR callbacks.

---

## FreeRTOS Configuration

| Parameter | Value | Source |
|---|---|---|
| Tick rate | **1000 Hz** | `CONFIG_FREERTOS_HZ=1000` in `sdkconfig.defaults` |
| Tick period | 1 ms | — |
| `configMAX_PRIORITIES` | Default (25) | ESP-IDF default for ESP32-C6 |

---

## ISR Paths (No FreeRTOS Task Involvement)

These run at hardware interrupt priority and MUST complete in microseconds.

### gptimer Alarm — TWAI0 Periodic TX (100 ms)

**This is the highest-priority real-time path in the system.**

```
gptimer alarm (hardware)
  └─► alarm ISR callback (IRAM_ATTR)
        └─► if (!g_twai0_tx_enabled) return   ← gate check (DRAM_ATTR volatile bool)
        └─► copy frame_380_data / frame_5bf_data → private TX buffers (g_twai0_frame_lock)
        └─► obc_frame_seal(): PRUN + CSUM into the copies
        └─► twai_node_transmit(twai0_hdl, &frame_380, 0)
        └─► twai_node_transmit(twai0_hdl, &frame_5bf, 0)
```

- REQUIRED: Use `gptimer` (General Purpose Timer) hardware alarm, NOT `esp_timer`,
  NOT FreeRTOS `xTimerCreate()`.
- REQUIRED: The ISR MUST check `g_twai0_tx_enabled` (`DRAM_ATTR volatile bool`) before
  any call to `twai_node_transmit()`. If false, return immediately.
- REQUIRED: The alarm callback and both TX frame structs MUST be `IRAM_ATTR` / `DRAM_ATTR`.
  Helpers it calls (`obc_frames.h`) are `static inline __attribute__((always_inline))`.
- REQUIRED: `CONFIG_TWAI_IO_FUNC_IN_IRAM=y` so `twai_node_transmit()` is in IRAM.
- `timeout` parameter to `twai_node_transmit()` is ignored in ISR context (returns
  immediately if TX queue is full).
- RECOMMENDED: alarm period = 100 000 µs. Use `gptimer_set_alarm_action()` with
  `alarm_cfg.auto_reload_on_alarm = true`.
- `g_twai0_tx_enabled` is owned by the state machine. See
  `twai-config.instructions.md` §TX-Enable Gate.

```c
// gptimer configuration (REQUIRED fields)
gptimer_config_t timer_cfg = {
    .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
    .direction     = GPTIMER_COUNT_UP,
    .resolution_hz = 1000000,   // 1 MHz → 1 µs per tick
};
gptimer_alarm_config_t alarm_cfg = {
    .alarm_count            = 100000,  // 100 ms
    .reload_count           = 0,
    .flags.auto_reload_on_alarm = true,
};
```

### TWAI0 / TWAI1 `on_rx_done` — Frame Reception

```
TWAI hardware RX interrupt
  └─► on_rx_done ISR (IRAM_ATTR)
        └─► twai_node_receive_from_isr()
        └─► software ID filter (accept / reject)
        └─► xQueueSendFromISR(rx_queue, &frame, &higher_prio_woken)
        └─► portYIELD_FROM_ISR(higher_prio_woken)
```

- The ISR MUST call `twai_node_receive_from_isr()` to retrieve the frame data.
- Software filter: reject any ID not in the target set. Only matching frames are enqueued.
- `rx_queue` handle MUST be `DRAM_ATTR`.
- Frame data written into the queue MUST be a value copy (not a pointer to a local).
- Return `true` from the callback if `higher_prio_woken` is set (allows the dispatcher
  task to preempt immediately on ISR exit).

### MCPWM `on_cap` — PWM Edge Capture

```
MCPWM capture hardware
  └─► on_cap ISR (IRAM_ATTR)
        └─► compute duty_us and period_us
        └─► write to DRAM_ATTR volatile g_pwm_duty_us / g_pwm_period_us
        └─► esp_timer_restart(pwm_watchdog, 3000)
```

- Results written directly to `DRAM_ATTR volatile` variables — no queue.
- Fires at up to 2 kHz (two edges per 1 kHz cycle). ISR body MUST be minimal.

### ADC `on_conv_done` — Continuous DMA Frame

```
ADC DMA hardware
  └─► on_conv_done ISR (IRAM_ATTR)
        └─► average 16 raw 12-bit samples
        └─► write to DRAM_ATTR volatile g_adc_raw_avg
```

- No queue. No calibration in ISR (calibration function is not IRAM-resident).
- Fires at 250 Hz effective rate.

---

## FreeRTOS Tasks

### Task Priority Table

| Task | Priority | Stack (bytes) | Core affinity |
|---|---|---|---|
| RX Dispatcher Task | `configMAX_PRIORITIES − 2` = 23 | 3072 | No affinity (single core) |
| TWAI1 TX Task | `configMAX_PRIORITIES − 3` = 22 | 2048 | No affinity |
| State Machine Task | `configMAX_PRIORITIES − 4` = 21 | 4096 | No affinity |

**Do not use `configMAX_PRIORITIES − 1`** (reserved for the FreeRTOS timer daemon task
when `configUSE_TIMERS = 1`).

---

### RX Dispatcher Task (Priority 23)

**Purpose:** Dequeue CAN frames from the shared RX queue and dispatch to the appropriate
application callback based on CAN ID and source bus.

```c
static void rx_dispatcher_task(void *pvParams)
{
    can_rx_event_t event;
    for (;;) {
        if (xQueueReceive(rx_queue, &event, portMAX_DELAY) == pdTRUE) {
            dispatch_can_message(&event);   // routes by bus + ID to handler function
        }
    }
}
```

- **Queue:** Single shared `QueueHandle_t rx_queue` fed by both TWAI0 and TWAI1
  `on_rx_done` ISRs. Queue depth: **16 entries** minimum (handles burst of all 15
  distinct target IDs — 11 on TWAI0, 4 on TWAI1 — with headroom).
- **Queue element type:** A struct containing `{ twai_node_handle_t bus, uint32_t id,
  uint8_t data[8], uint8_t dlc }` — a full value copy, not a pointer.
- The dispatcher calls handler functions that are allowed to use FreeRTOS primitives
  (semaphores, event groups, direct task notifications) to signal the state machine task.

---

### TWAI1 TX Task (Priority 22)

**Purpose:** Send CAN ID `0x050` on TWAI1 every 100 ms while the task is running.
The state machine task suspends and resumes this task to gate TWAI1 operation.

```c
static void twai1_tx_task(void *pvParams)
{
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        twai1_tx_frame_050();   // copies frame_050_data under g_frame050_lock, then transmits
        if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100)) == pdFALSE) {
            last_wake = xTaskGetTickCount();   // resumed after a suspension: re-align
        }
    }
}
```

- REQUIRED: Use `xTaskDelayUntil()` (not `vTaskDelay()`) for drift-free 100 ms cadence.
- REQUIRED: The task handle MUST be stored globally so the state machine can call
  `vTaskSuspend(twai1_tx_task_hdl)` and `vTaskResume(twai1_tx_task_hdl)`.
- REQUIRED: Re-align `last_wake` when `xTaskDelayUntil()` returns `pdFALSE`. After a
  suspension the deadline is long past, and FreeRTOS would otherwise return immediately for
  every missed period — thousands of back-to-back 0x050 frames after a long suspension,
  filling the TX queue.
- REQUIRED: Clear ChargeEnable with `twai1_frame_050_set_enable(false)`. The driver reads a
  queued frame's buffer when it starts transmitting it, and the helper also patches the private
  copy, so frames still queued go out disabled.
- `timeout = pdMS_TO_TICKS(10)`: if the TX queue is full (8 entries), wait up to 10 ms
  before dropping the frame. Log a warning if the frame is dropped.

---

### State Machine Task (Priority 21)

**Purpose:** Own the entire application state machine. Read sensor inputs, evaluate
transitions, drive GPIO outputs, manage TWAI1 activation.

```c
static void state_machine_task(void *pvParams)
{
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        // 1. Read sensor inputs (all are volatile DRAM_ATTR — no blocking)
        uint32_t raw_adc  = g_adc_raw_avg;
        uint32_t duty_us  = g_pwm_duty_us;
        uint32_t period_us = g_pwm_period_us;
        bool pwm_valid    = g_pwm_signal_valid;

        // 2. Calibrate ADC (not ISR-safe — only called from task context)
        int voltage_mv = 0;
        if (adc_cali_hdl) adc_cali_raw_to_voltage(adc_cali_hdl, (int)raw_adc, &voltage_mv);

        // 3. Evaluate state transitions and drive outputs
        // ...

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));  // 10 ms state machine cycle
    }
}
```

- REQUIRED: All GPIO writes (`gpio_set_level()`) happen ONLY in this task.
- REQUIRED: `adc_cali_raw_to_voltage()` is called ONLY in this task (not ISR-safe for
  concurrent callers on the same handle).
- REQUIRED: TWAI1 TX task is controlled via `vTaskSuspend()` / `vTaskResume()` from
  this task based on application state transitions.
- REQUIRED: Multi-byte updates of `frame_380_data` / `frame_5bf_data` hold
  `g_twai0_frame_lock`; updates of `frame_050_data` hold `g_frame050_lock`. Single-byte
  stores need no lock. This task is the only writer of all three buffers.
- REQUIRED: Decoded CAN values come from `g_sm_rx` (written only by the RX dispatcher).
  Age each source with the task's own clock from its frame counter (`cnt_*`) and treat a
  stale source as absent; never block on the dispatcher.
- REQUIRED: Timed actions (EVA pulse, announcement steps, contactor steps) are tick-counted
  across cycles, never `vTaskDelay()`.
- REQUIRED: Contactor outputs (`CONT_POS`, `CONT_NEG`) MUST be de-asserted immediately
  on detection of TWAI0 bus-off or PWM signal absence.
- RECOMMENDED: 10 ms task cycle — fast enough to react to button press (ADC updates at
  250 Hz) and PWM absence (watchdog at 3 ms), well within human-perceptible latency.

---

## IRAM / DRAM Placement Summary

Every symbol in an ISR hot path MUST be in internal RAM:

| Symbol | Attribute | Reason |
|---|---|---|
| All ISR callback functions | `IRAM_ATTR` | Flash cache may be disabled |
| TWAI TX frame data | `DRAM_ATTR` | Accessed from gptimer alarm ISR |
| RX queue handle | `DRAM_ATTR` | Accessed from TWAI on_rx_done ISR |
| `g_pwm_duty_us`, `g_pwm_period_us`, `g_pwm_signal_valid` | `DRAM_ATTR volatile` | Written from MCPWM ISR, read from task |
| `g_adc_raw_avg` | `DRAM_ATTR volatile` | Written from ADC ISR, read from task |
| TWAI error/bus-off flags | `DRAM_ATTR volatile` | Written from TWAI ISR, read from state machine |
| `g_twai0_tx_enabled` | `DRAM_ATTR volatile` | Read from gptimer alarm ISR; written by the state machine task |
| `g_twai0_frame_lock`, private TX copies of 0x380 / 0x5BF | `DRAM_ATTR` | Taken / written inside the gptimer alarm ISR |

`volatile` is REQUIRED on all variables shared between ISR writers and task readers to
prevent compiler caching into registers.

---

## Initialization Order in `app_main()`

REQUIRED sequence to avoid ISR firing before all data structures are ready:

1. `gpio_outputs_init()` — safe state before anything else
2. `rx_queue = xQueueCreate(16, sizeof(can_rx_event_t))` — queue before ISR registration
3. `twai_handler_init()` — TWAI nodes created, filters applied, callbacks registered
4. `mcpwm_capture_init()` — capture timer + channel created, callbacks registered
5. `adc_reader_init()` — ADC handle created, calibration scheme created, `adc_continuous_start()`
6. `gptimer_init_twai0_tx()` — gptimer created, alarm set, enabled (starts 100 ms TX cycle)
7. Create tasks: RX Dispatcher → TWAI1 TX → State Machine (in order of decreasing priority)
8. `twai_node_enable()` for TWAI0 and TWAI1 — connect to bus LAST, after all tasks exist

---

## Production Logging

Set `CONFIG_LOG_DEFAULT_LEVEL=2` (`ESP_LOG_WARN`) in `sdkconfig.defaults`.

- `ESP_LOGE` / `ESP_LOGW`: REQUIRED for fault conditions (bus-off, TX drop, ADC overflow)
- `ESP_LOGI`: FORBIDDEN in production builds — use only inside `#if CONFIG_LOG_LEVEL >= 3` guards
- `ESP_LOGD` / `ESP_LOGV`: FORBIDDEN in production builds
