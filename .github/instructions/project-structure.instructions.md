---
applyTo: "**"
---
# Project Structure — OBCController

## Identity

| Property | Value |
|---|---|
| Project name | `OBCController` |
| IDF version | **v6.0.2** |
| Target chip | **ESP32-C6** (single-core RISC-V, 160 MHz) |
| Build system | ESP-IDF CMake (`idf.py build`) |

## REQUIRED: Directory Layout

All application code lives under `main/`. The structure is intentionally flat so that individual translation units can later be promoted to proper `components/` without rewriting logic.

```
OBCController/
└── firmware/
    ├── main/
    │   ├── CMakeLists.txt
    │   ├── main.c                  # App entry point: hardware init sequence, task creation
    │   ├── gpio_config.h           # REQUIRED: single authoritative GPIO pin assignment header
    │   ├── twai_handler.c          # TWAI0 + TWAI1 driver init, filters, ISR callbacks, TX helpers
    │   ├── twai_handler.h
    │   ├── mcpwm_capture.c         # MCPWM capture timer + channel init, on_cap ISR, watchdog
    │   ├── mcpwm_capture.h
    │   ├── adc_reader.c            # ADC continuous DMA init, on_conv_done ISR, calibration handle
    │   ├── adc_reader.h
    │   ├── state_machine.c         # Application state machine task body (bypass + VCM-authorised paths)
    │   ├── state_machine.h         # SM flags, g_sm_rx decoded CAN status, runtime settings
    │   ├── sm_config.h             # REQUIRED: all state machine tunables
    │   ├── obc_frames.h            # Header-only EV-CAN / Lear frame encoders, decoders and CSUM (no IDF deps)
    │   ├── rx_dispatcher.c         # RX queue consumer task and per-ID callback dispatch
    │   └── rx_dispatcher.h
    ├── CMakeLists.txt
    └── sdkconfig.defaults          # Kconfig overrides — IRAM flags, log level, tick rate
```

## REQUIRED: `gpio_config.h`

Every GPIO pin number in the entire project MUST be defined as a macro in this one header. No numeric literals (`GPIO_NUM_x`) may appear anywhere else. This enables board-level pin remapping in a single edit.

```c
#pragma once
#include "driver/gpio.h"

/* ── TWAI ──────────────────────────────────────────── */
#define GPIO_TWAI0_TX        GPIO_NUM_4
#define GPIO_TWAI0_RX        GPIO_NUM_5
#define GPIO_TWAI1_TX        GPIO_NUM_6
#define GPIO_TWAI1_RX        GPIO_NUM_7

/* ── MCPWM Capture ─────────────────────────────────── */
#define GPIO_PWM_CAPTURE     GPIO_NUM_0

/* ── ADC ────────────────────────────────────────────── */
#define GPIO_ADC_INPUT       GPIO_NUM_2   /* ADC1_CHANNEL_2 */

/* ── Digital Outputs ───────────────────────────────── */
#define GPIO_CP_ENABLE       GPIO_NUM_9   /* WARNING: strapping pin — see gpio-outputs-config */
#define GPIO_PLUG_SIG        GPIO_NUM_8   /* WARNING: strapping pin — see gpio-outputs-config */
#define GPIO_STATUS_LED      GPIO_NUM_15
#define GPIO_EVA_REQ         GPIO_NUM_19
#define GPIO_CONT_POS        GPIO_NUM_22
#define GPIO_CONT_NEG        GPIO_NUM_23
```

## REQUIRED: `sdkconfig.defaults`

These Kconfig values MUST be present. They enable IRAM placement for all time-critical ISR paths and set production defaults.

```
# TWAI driver — IRAM and cache-safe ISR placement
CONFIG_TWAI_ISR_IN_IRAM=y
CONFIG_TWAI_IO_FUNC_IN_IRAM=y
CONFIG_TWAI_ISR_CACHE_SAFE=y

# MCPWM driver — cache-safe ISR placement
CONFIG_MCPWM_ISR_CACHE_SAFE=y

# ADC continuous driver — IRAM-safe ISR
CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE=y

# Logging — warn level only in production
CONFIG_LOG_DEFAULT_LEVEL=2

# FreeRTOS tick rate
CONFIG_FREERTOS_HZ=1000
```

## RECOMMENDED: `main/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS
        "main.c"
        "twai_handler.c"
        "mcpwm_capture.c"
        "adc_reader.c"
        "state_machine.c"
        "rx_dispatcher.c"
    INCLUDE_DIRS "."
)
```
