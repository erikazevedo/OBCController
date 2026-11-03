---
applyTo: "**/twai*"
---
# TWAI Configuration — OBCController

> **API reference:** ESP-IDF v6.0.2 TWAI driver (completely redesigned in v6.0 — the v5.x
> `twai_general_config_t` / `twai_driver_install()` API does not exist).
> Docs: https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32c6/api-reference/peripherals/twai.html
> Headers: `esp_twai_onchip.h`, `esp_twai.h`, `hal/twai_types.h`

---

## Shared Parameters (Both Buses)

| Property | Value |
|---|---|
| Baud rate | **500 kbps** |
| Frame type | Classic CAN, standard 11-bit IDs only |
| Operating mode | Normal (TX + RX + ACK) |
| Clock source | `TWAI_CLK_SRC_DEFAULT` |
| `fail_retry_cnt` | **-1** (unlimited retries) — downstream hardware faults on missed frames |
| TX queue depth | **8** |
| `intr_priority` | **3** (highest available) |

## REQUIRED: Kconfig (sdkconfig.defaults)

```
CONFIG_TWAI_ISR_IN_IRAM=y
CONFIG_TWAI_IO_FUNC_IN_IRAM=y
CONFIG_TWAI_ISR_CACHE_SAFE=y
```

When `CONFIG_TWAI_ISR_CACHE_SAFE=y` is set the driver enforces that all registered
callback functions and their associated data reside in internal RAM. Every `on_rx_done`,
`on_tx_done`, `on_error`, and `on_state_change` callback MUST be annotated with
`IRAM_ATTR`. All data those callbacks read or write MUST be annotated with `DRAM_ATTR`.

---

## TWAI0 — Timing-Critical Bus

### Pin Assignment

```c
// From gpio_config.h — do not hardcode
.io_cfg.tx = GPIO_TWAI0_TX,   // GPIO4
.io_cfg.rx = GPIO_TWAI0_RX,   // GPIO5
.io_cfg.quanta_clk_out    = -1,
.io_cfg.bus_off_indicator = -1,
```

### Node Config Struct

```c
twai_onchip_node_config_t twai0_cfg = {
    .io_cfg = {
        .tx = GPIO_TWAI0_TX,
        .rx = GPIO_TWAI0_RX,
        .quanta_clk_out    = -1,
        .bus_off_indicator = -1,
    },
    .clk_src         = TWAI_CLK_SRC_DEFAULT,
    .bit_timing      = { /* 500 kbps — let driver compute or use explicit values */ },
    .tx_queue_depth  = 8,
    .fail_retry_cnt  = -1,
    .intr_priority   = 3,
    .flags = {
        .enable_self_test  = 0,
        .enable_loopback   = 0,
        .enable_listen_only= 0,
        .no_receive_rtr    = 1,   /* RECOMMENDED: suppress RTR frames */
    },
};
```

### Acceptance Filter — REQUIRED

TWAI0 uses **dual mask filter mode** to maximise hardware pre-filtering.

```c
// Filter 1: covers 0x1D0–0x1DF  AND  0x1F0–0x1FF
//   Target IDs in this range: 0x1D4, 0x1DA, 0x1DB, 0x1DC, 0x1F2
// Filter 2: covers 0x400–0x7FF (bit 10 must be set)
//   Target IDs in this range: 0x50B, 0x55B, 0x5BC, 0x603, 0x68C, 0x6C0 (display settings)
twai_mask_filter_config_t twai0_filter =
    twai_make_dual_filter(0x1D0, 0x7D0,   /* id1, mask1 */
                          0x400, 0x400,   /* id2, mask2 */
                          false);         /* 11-bit standard IDs */
// Apply after node creation, before enable:
ESP_ERROR_CHECK(twai_node_config_mask_filter(twai0_hdl, 0, &twai0_filter));
```

**Mask logic:** `1` = bit must match, `0` = bit is don't-care.

**REQUIRED: software rejection** — The hardware filter passes false positives.
Inside `on_rx_done`, after calling `twai_node_receive_from_isr()`, reject any frame
whose ID is not exactly one of: `{0x1D4, 0x1DA, 0x1DB, 0x1DC, 0x1F2, 0x50B, 0x55B, 0x5BC, 0x603, 0x68C, 0x6C0}`.

### TX Frames

TWAI0 transmits **two frames per 100 ms cycle** while the TX-enable gate is active:

| Frame | CAN ID |
|---|---|
| Frame A | `0x380` |
| Frame B | `0x5BF` |

**REQUIRED:** The 100 ms transmit trigger MUST use a `gptimer` hardware alarm ISR, NOT a
FreeRTOS timer or task delay. The alarm ISR MUST read `g_twai0_tx_enabled` first; if
false, it returns immediately without calling `twai_node_transmit()`. When the flag is
true, both frames are transmitted in sequence. `CONFIG_TWAI_IO_FUNC_IN_IRAM=y` ensures
`twai_node_transmit()` is in IRAM, eliminating Flash cache fetch latency at ISR call time.

The alarm ISR callback and all frame data buffers it accesses MUST be `IRAM_ATTR` /
`DRAM_ATTR` respectively.

**REQUIRED: private TX buffers.** The driver keeps a pointer to the frame buffer until the
frame is on the bus, and `0x5BF` queues behind `0x380`. The ISR therefore copies
`frame_380_data` / `frame_5bf_data` into private `DRAM_ATTR` TX buffers inside
`portENTER_CRITICAL_ISR(&g_twai0_frame_lock)`, then writes PRUN and CSUM into the copies
(`obc_frame_seal()` in `obc_frames.h`) and transmits them. Only the ISR writes the private
buffers. The TWAI1 task does the same for `0x050` under `g_frame050_lock`.

### TX-Enable Gate — `g_twai0_tx_enabled`

Transmission of 0x380, 0x5BF and 0x6C1 is gated by `DRAM_ATTR volatile bool g_twai0_tx_enabled`.
There is no start-up delay: the stock OBC answers 250 ms after the VCM wakes.

| Owner | Rule |
|---|---|
| State machine task, end of each cycle | VCM-authorised states: transmit while a VCM frame (`0x50B`/`0x1F2`/`0x1D4`) arrived within `SM_TX_VCM_FRESH_MS` and GoToSleep has lasted less than `SM_SLEEP_TAIL_MS` (the VCM is awake in a charge wake with d[2] = 0x00). SLEEP and POWER_ON: transmit while the power switch is ON (`0x50B` d[2] != 0x00, fresh within `SM_VCM_SILENT_MS`) |

> **Do not gate on `0x50B d[2]` alone:** a charge wake keeps d[2] = `0x00`
> for the whole session (only d[3][7:6] = `11` shows the VCM is awake), so the bridge would
> stay silent and the VCM would time out after 10.24 s.

> **Never transmit to an empty bus.** TWAI0 uses `fail_retry_cnt = -1`: an unacknowledged
> frame is retried until some node ACKs it, keeping a sleeping EV-CAN busy. A finite retry
> count is not a fix on the ESP32-C6 — any value other than -1 selects single-shot mode, which
> also drops frames that lose arbitration (routine for 0x380/0x5BF). Close the gate while
> other nodes are still transmitting.

### Error Handling — REQUIRED

Register an `on_state_change` callback. On transition to `TWAI_ERROR_BUS_OFF`:
1. Immediately call `twai_node_recover(twai0_hdl)` to initiate the 128×11 recessive-bit
   recovery sequence.
2. Set a `DRAM_ATTR volatile` flag visible to the state machine task.
3. The state machine task MUST treat TWAI0 bus-off as a high-priority fault condition.

Register an `on_error` callback to log `twai_error_flags_t` fields at `ESP_LOG_WARN`,
**rate-limited** to one line per bus per `TWAI_ERR_LOG_PERIOD_MS` (1 s) carrying the error
count and the OR of the flags since the previous line. An unacknowledged frame is retried
every ≈ 0.3 ms and each retry raises a bus error; one blocking `ESP_DRAM_LOGW` (≈ 2 ms at
115200 baud) per error keeps the level-3 ISR running back to back and starves every task.

---

## TWAI1 — State-Gated Bus

### Pin Assignment

```c
.io_cfg.tx = GPIO_TWAI1_TX,   // GPIO6
.io_cfg.rx = GPIO_TWAI1_RX,   // GPIO7
.io_cfg.quanta_clk_out    = -1,
.io_cfg.bus_off_indicator = -1,
```

### Node Config Struct

Identical to TWAI0 config except pin assignments. Same `tx_queue_depth`, `fail_retry_cnt`,
`intr_priority`, and `no_receive_rtr` settings apply.

### Acceptance Filter — REQUIRED

```c
// Filter 1: covers 0x270–0x27F
//   Target IDs in this range: 0x274, 0x27D (AC input voltage)
// Filter 2: covers 0x610–0x61F
//   Target IDs in this range: 0x616, 0x617, 0x618
twai_mask_filter_config_t twai1_filter =
    twai_make_dual_filter(0x270, 0x7F0,
                          0x610, 0x7F0,
                          false);
ESP_ERROR_CHECK(twai_node_config_mask_filter(twai1_hdl, 0, &twai1_filter));
```

**REQUIRED: software rejection** inside `on_rx_done` — accept only
`{0x274, 0x27D, 0x616, 0x617, 0x618}` (`TWAI1_ID_IS_CORE()`), drop everything else.

**Raw capture (`SM_LOG_LEAR_RAW 1`, default).** To find undocumented Lear signals the hardware filter is
opened (`twai_make_dual_filter(0, 0, 0, 0, false)`, mask 0 = don't care) and the software rejection accepts
every ID. Frames outside the five core IDs (`TWAI1_ID_IS_CORE()`) are best effort: the ISR drops them while
`rx_queue` holds `TWAI1_RAW_QUEUE_LIMIT` events or more, and the RX dispatcher only logs them — they never
count as "Lear alive". With `SM_LOG_LEAR_RAW 0` the filter and rejection above apply unchanged.

### TX Frame

TWAI1 transmits **one frame per 100 ms cycle** while active:

| Frame | CAN ID |
|---|---|
| Frame A | `0x050` |

**REQUIRED:** TX is driven by a dedicated high-priority FreeRTOS task (see
`task-architecture.instructions.md`). The task MUST be suspendable: the state machine
calls `vTaskSuspend()` / `vTaskResume()` on it to gate TWAI1 operation to the active
application state only.

### Error Handling — REQUIRED

Register `on_state_change`. Bus-off recovery MUST only be attempted while TWAI1 is in
the active application state (i.e., while its TX task is running). If bus-off occurs
while suspended, store the fault flag and call `twai_node_recover()` only when the state
machine re-activates TWAI1.

---

## IRAM / DRAM Placement Checklist

Every symbol touched from a TWAI ISR callback context MUST comply:

| Symbol type | Required attribute |
|---|---|
| Callback function definition | `IRAM_ATTR` |
| TX frame data buffers | `DRAM_ATTR` |
| RX result buffers | `DRAM_ATTR` |
| Error/state flag variables | `DRAM_ATTR volatile` |
| Queue handle (for `xQueueSendFromISR`) | `DRAM_ATTR` |
| `g_twai0_tx_enabled` (TWAI0 TX gate) | `DRAM_ATTR volatile` |
| `g_twai0_frame_lock` / private TX copies of 0x380 and 0x5BF | `DRAM_ATTR` |

Failure to apply these when `CONFIG_TWAI_ISR_CACHE_SAFE=y` causes the driver to return
`ESP_ERR_INVALID_ARG` at callback registration time.

---

## Hardware Limitation Flags

- **No RX queue in v6 API.** All received frames arrive exclusively via `on_rx_done` ISR
  callback. There is no `twai_receive()` polling function equivalent.
- **Single hardware filter slot per controller.** Dual-filter mode splits this into two
  16-bit filters; there is no third filter. All remaining filtering is software-only.
- **`twai_node_config_mask_filter()` requires the node to be disabled.** Configure
  filters between `twai_new_onchip_node()` and `twai_node_enable()`, not at runtime.
- **`fail_retry_cnt = -1` can hold the bus** during persistent error conditions. The
  `on_error` / `on_state_change` callbacks are the only mechanism to detect this.
