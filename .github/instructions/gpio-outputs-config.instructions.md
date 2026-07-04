---
applyTo: "**/gpio_config*"
---
# GPIO Outputs Configuration — OBCController

> **API reference:** ESP-IDF v6.0.2 GPIO driver.
> Docs: https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32c6/api-reference/peripherals/gpio.html
> Header: `driver/gpio.h`

---

## Output Pin Summary

All six outputs are **active-high, standard push-pull, OFF (logic 0) at boot**.
No output requires open-drain mode. No output is toggled from ISR context.

| Macro | GPIO | Function | Circuit | Active State |
|---|---|---|---|---|
| `GPIO_CP_ENABLE` | GPIO9 | CP_ENABLE | MOSFET switching resistor to GND | HIGH = resistor connected |
| `GPIO_PLUG_SIG` | GPIO8 | PLUG_SIG | MOSFET switching | HIGH = active |
| `GPIO_STATUS_LED` | GPIO15 | Status LED | LED anode drive | HIGH = LED on |
| `GPIO_EVA_REQ` | GPIO19 | EVA_REQ | MOSFET → PNP → +12 V pulse | HIGH = pulse active |
| `GPIO_CONT_POS` | GPIO22 | CONT_POS | Low-side contactor MOSFET | HIGH = contactor closed |
| `GPIO_CONT_NEG` | GPIO23 | CONT_NEG | Low-side contactor MOSFET | HIGH = contactor closed |

---

## REQUIRED: Strapping Pin Warning — GPIO8 and GPIO9

**GPIO8 and GPIO9 are ESP32-C6 strapping pins.** Their logic levels are sampled by the
chip at every reset/power-on. Incorrect levels at reset will cause boot failure or
unintended JTAG mode entry.

| GPIO | Strapping function | Safe level at reset | Risk if driven LOW |
|---|---|---|---|
| GPIO9 | Boot mode select | **HIGH** (internal pull-up default) | Enters ROM serial download mode |
| GPIO8 | JTAG signal source | **HIGH** (internal pull-up default) | JTAG routed from MTDO/GPIO3 |

**REQUIRED PCB constraint:** Place a **10 kΩ pull-up resistor to 3.3 V** on the GPIO8
and GPIO9 nets **between the MCU pad and the MOSFET gate**. This ensures the strapping
level remains HIGH during the reset window before firmware actively drives the pins,
regardless of MOSFET gate capacitance or layout parasitics.

Do NOT rely solely on the ESP32-C6 internal pull-ups for strapping protection on these
nets because the MOSFET gate and PCB trace capacitance can pull the node low during the
reset edge before the internal pull-up charges the net.

---

## REQUIRED: Initialization

Configure all output pins in a single call using a bitmask. GPIO must be driven LOW
before the direction is set to output to guarantee the initial state is OFF before any
MOSFET gate charges.

```c
#include "driver/gpio.h"
#include "gpio_config.h"

void gpio_outputs_init(void)
{
    // Define the output bitmask from gpio_config.h macros
    const uint64_t output_mask =
        (1ULL << GPIO_CP_ENABLE)  |
        (1ULL << GPIO_PLUG_SIG)   |
        (1ULL << GPIO_STATUS_LED) |
        (1ULL << GPIO_EVA_REQ)    |
        (1ULL << GPIO_CONT_POS)   |
        (1ULL << GPIO_CONT_NEG);

    gpio_config_t cfg = {
        .pin_bit_mask = output_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    // Explicitly drive all outputs LOW after direction is set
    gpio_set_level(GPIO_CP_ENABLE,  0);
    gpio_set_level(GPIO_PLUG_SIG,   0);
    gpio_set_level(GPIO_STATUS_LED, 0);
    gpio_set_level(GPIO_EVA_REQ,    0);
    gpio_set_level(GPIO_CONT_POS,   0);
    gpio_set_level(GPIO_CONT_NEG,   0);
}
```

Call `gpio_outputs_init()` as the **first peripheral init call** in `app_main()`, before
any TWAI, ADC, or MCPWM initialization, to ensure outputs are in a safe state from the
earliest possible point in firmware execution.

---

## REQUIRED: Application-Layer Control

All output writes are performed only by the **state machine task** (see
`task-architecture.instructions.md`). No other task or ISR writes to output GPIO.

Use the macros from `gpio_config.h` exclusively — never hardcode GPIO numbers at
call sites:

```c
// Correct
gpio_set_level(GPIO_CONT_POS, 1);

// FORBIDDEN — do not hardcode
gpio_set_level(GPIO_NUM_22, 1);
```

---

## EVA_REQ — Pulse Output Timing Note

`GPIO_EVA_REQ` drives a MOSFET that gates a PNP transistor producing a +12 V pulse.
The pulse width and timing are determined by the state machine task logic.
**Do not leave EVA_REQ asserted indefinitely** — the downstream PNP drive circuit is
not intended for continuous conduction. The state machine is responsible for enforcing
a maximum assertion duration appropriate to the hardware design.

---

## CONT_POS / CONT_NEG — Contactor Safety Note

`GPIO_CONT_POS` and `GPIO_CONT_NEG` control low-side MOSFET switches for a positive and
negative contactor respectively. The state machine MUST enforce a defined **closure
sequence and opening sequence** to protect against pre-charge faults or arcing:

- RECOMMENDED closure sequence: energise pre-charge path before asserting CONT_POS and
  CONT_NEG simultaneously, if pre-charge is part of the system design.
- REQUIRED: both contactors MUST be de-asserted (driven LOW) immediately on any
  TWAI0 bus-off event or loss of PWM signal.

These safety interlocks are the responsibility of the state machine task, not the GPIO
driver layer.

---

## Hardware Limitation Flags

- **GPIO8/GPIO9 are strapping pins.** PCB pull-up resistors are a hardware requirement,
  not optional. Firmware alone cannot guarantee safe boot if pull-ups are absent.
- **GPIO0–GPIO7 are LP IO pads** on ESP32-C6. GPIO8 and GPIO9 are within this range;
  they are full-function digital IO pads suitable for MOSFET gate drive.
- **No ISR toggling.** `gpio_set_level()` is not declared `IRAM_ATTR` in the standard
  driver. If any future requirement arises to toggle an output from an ISR, use
  `gpio_ll_set_level()` directly (HAL layer, IRAM-safe) or the `IRAM_ATTR`-wrapped
  GPIO functions and update this instruction file accordingly.
