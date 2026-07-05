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
//   Target IDs in this range: 0x1D4, 0x1DB, 0x1DC, 0x1F2
// Filter 2: covers 0x500–0x5FF
//   Target IDs in this range: 0x50B, 0x55B, 0x5BC
twai_mask_filter_config_t twai0_filter =
    twai_make_dual_filter(0x1D0, 0x7D0,   /* id1, mask1 */
                          0x500, 0x700,   /* id2, mask2 */
                          false);         /* 11-bit standard IDs */
// Apply after node creation, before enable:
ESP_ERROR_CHECK(twai_node_config_mask_filter(twai0_hdl, 0, &twai0_filter));
```

**Mask logic:** `1` = bit must match, `0` = bit is don't-care.

**REQUIRED: software rejection** — The hardware filter passes false positives.
Inside `on_rx_done`, after calling `twai_node_receive_from_isr()`, reject any frame
whose ID is not exactly one of: `{0x1D4, 0x1DB, 0x1DC, 0x1F2, 0x50B, 0x55B, 0x5BC}`.

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

### TX-Enable Gate — 0x50B VCM Wake/Sleep Command

Transmission of 0x380 and 0x5BF is gated by `DRAM_ATTR volatile bool g_twai0_tx_enabled`,
controlled entirely by the **RX dispatcher task** when it processes 0x50B frames.

#### State Transitions

| `d[3][7:6]` | Symbolic name | Action |
|---|---|---|
| `0b11` | RUN — SLEEP→RUN leading edge | Start 5-second one-shot `esp_timer`; do NOT set flag yet |
| `0b11` | RUN — already waiting or already enabled | No-op; do not restart the timer |
| `0b00` | SLEEP | Set `g_twai0_tx_enabled = false`; set `g_twai0_tx_pending = false`; call `esp_timer_stop()` |

**REQUIRED:** The `esp_timer` one-shot callback (fires 5 s after the first `0b11` frame)
sets `g_twai0_tx_enabled = true`. The timer is created once at init with
`esp_timer_create()` and reused across RUN/SLEEP cycles via `esp_timer_start_once()` /
`esp_timer_stop()`.

```c
// One-shot timer callback — runs in esp_timer task context
static void twai0_tx_enable_cb(void *arg)
{
    g_twai0_tx_enabled = true;
}

// Called from RX dispatcher when a 0x50B frame is decoded
static void handle_50b(const uint8_t *data)
{
    uint8_t cmd = (data[3] >> 6) & 0x03;
    if (cmd == 0x03) {                              // RUN
        if (!g_twai0_tx_pending && !g_twai0_tx_enabled) {
            g_twai0_tx_pending = true;
            esp_timer_start_once(twai0_tx_delay_timer, 5000000ULL); // 5 s
        }
    } else if (cmd == 0x00) {                       // SLEEP
        g_twai0_tx_enabled  = false;
        g_twai0_tx_pending  = false;
        esp_timer_stop(twai0_tx_delay_timer);
    }
}
```

`g_twai0_tx_pending` is a sentinel that prevents the 5-second timer being restarted on
every subsequent `0b11` frame received during the delay window.

**REQUIRED:** Both `g_twai0_tx_enabled` and `g_twai0_tx_pending` MUST be declared
`DRAM_ATTR volatile bool`. `g_twai0_tx_enabled` is read from the gptimer alarm ISR.

**REQUIRED:** `twai0_tx_delay_timer` MUST be created (via `esp_timer_create()`) inside
`twai_handler_init()`, before any call to `twai_node_enable()`.

**NOTE:** The `esp_timer` callback does NOT require `IRAM_ATTR` — it only writes a
single `volatile bool` and makes no cache-sensitive calls.

### Error Handling — REQUIRED

Register an `on_state_change` callback. On transition to `TWAI_ERROR_BUS_OFF`:
1. Immediately call `twai_node_recover(twai0_hdl)` to initiate the 128×11 recessive-bit
   recovery sequence.
2. Set a `DRAM_ATTR volatile` flag visible to the state machine task.
3. The state machine task MUST treat TWAI0 bus-off as a high-priority fault condition.

Register an `on_error` callback to log `twai_error_flags_t` fields at `ESP_LOG_WARN`.

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
// Filter 1: exact match for 0x274  (mask 0x7FF = all bits must match)
// Filter 2: covers 0x610–0x61F
//   Target IDs in this range: 0x616, 0x617, 0x618
twai_mask_filter_config_t twai1_filter =
    twai_make_dual_filter(0x274, 0x7FF,
                          0x610, 0x7F0,
                          false);
ESP_ERROR_CHECK(twai_node_config_mask_filter(twai1_hdl, 0, &twai1_filter));
```

**REQUIRED: software rejection** inside `on_rx_done` — accept only
`{0x274, 0x616, 0x617, 0x618}`, drop everything else.

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
| `g_twai0_tx_pending` (5-second delay sentinel) | `DRAM_ATTR volatile` |

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
