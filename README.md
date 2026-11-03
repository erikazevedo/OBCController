# OBCController

ESP32-C6 firmware that lets a **Lear on-board charger** charge a **2011–2012 Nissan LEAF (ZE0)** in place of the stock OBC.

The controller sits on two CAN buses:

- **EV-CAN:** it imitates the stock OBC, so the VCM authorises and runs a normal AC charge.
- **Lear CAN:** it drives the Lear within the limits the VCM, the EVSE pilot and the measured AC voltage allow.

> **Warning:** this firmware switches the HV charge contactors and controls a charger connected to the traction battery. Mistakes can damage the car or cause injury. Use it only if you are qualified to work on HV systems.
>
> The code is provided **as is, without warranty of any kind**, express or implied, including fitness for a particular purpose. **Use it at your own risk.** The authors accept no liability for damage to vehicles, equipment or property, or for injury, resulting from its use.

## How it works

The VCM authorises a charge only with the power switch OFF. In outline:

1. **Plug-in:** the controller detects the plug and reads the J1772 pilot. The plug is detected on PP through the ADC.
2. **Wake:** it wakes the VCM with the EV activation pulse, or answers a VCM self-wake (charge timer or charge switch).
3. **Announce:** it appears on EV-CAN as the OBC (`0x380`, `0x5BF`), announces the EVSE and asserts J1772 State C.
4. **Charge request:** it waits for the VCM's normal-charge request on `0x1F2`.
5. **Contactors:** once `0x1D4` reports the HV bus live and the Lear measures AC (`0x27D`), it closes the charge contactors. It then steps through the stock OBC start-up states.
6. **Charge:** it enables the Lear (`0x050`). The AC current limit is the lowest of the pilot current, 3.3 kW and the VCM's power command, with watts converted to amps at the measured AC voltage.
7. **Stop:** on a VCM stop request, connector button, unplug, SOC target or fault, it runs the stock stop sequence.

With the power switch ON it sends only idle OBC frames and never charges.

The full sequence, with state diagrams and frame contents, is in [charge-start-sequence.md](charge-start-sequence.md).

## Hardware

| Item | Value |
|---|---|
| MCU | ESP32-C6 (single-core RISC-V, 160 MHz) |
| TWAI0 | EV-CAN, 500 kbit/s |
| TWAI1 | Lear charger CAN, 500 kbit/s |

| GPIO | Name | Function |
|---|---|---|
| 4 / 5 | `GPIO_TWAI0_TX` / `RX` | EV-CAN transceiver |
| 6 / 7 | `GPIO_TWAI1_TX` / `RX` | Lear CAN transceiver |
| 0 | `GPIO_PWM_CAPTURE` | J1772 CP pilot, MCPWM capture (duty → current limit) |
| 2 | `GPIO_ADC_INPUT` | J1772 PP proximity, ADC1 channel 2 (no plug / plugged in / button pressed) |
| 9 | `GPIO_CP_ENABLE` | J1772 State C resistor |
| 8 | `GPIO_PLUG_SIG` | OBC pin 21, combination-meter plug indicator |
| 19 | `GPIO_EVA_REQ` | EV activation pulse, OBC pin 18 to the VCM |
| 22 | `GPIO_CONT_POS` | HV charge contactor + |
| 23 | `GPIO_CONT_NEG` | HV charge contactor − |
| 15 | `GPIO_STATUS_LED` | Status LED |

**GPIO notes:**
- All pin numbers are defined only in [gpio_config.h](firmware/main/gpio_config.h).
- GPIO 8 and 9 are strapping pins.

## Repository layout

```
OBCController/
├── charge-start-sequence.md     Charge-start state diagrams and CAN frame contents
├── .github/
│   ├── instructions/            Authoritative specs (CAN frames, state machine, peripherals)
│   ├── agents/                  Copilot agents for coding, QA and spec work
│   └── workflows/               CI firmware build
└── firmware/                    ESP-IDF project
    ├── sdkconfig.defaults
    └── main/
        ├── main.c               Init sequence and task creation
        ├── gpio_config.h        All GPIO assignments
        ├── sm_config.h          State machine tunables
        ├── state_machine.c/.h   Charge state machine
        ├── rx_dispatcher.c/.h   CAN RX decoding
        ├── twai_handler.c/.h    TWAI0/TWAI1 drivers, filters, TX frames
        ├── obc_frames.h         Frame encoders/decoders and checksums (no IDF dependencies)
        ├── mcpwm_capture.c/.h   Pilot PWM capture
        └── adc_reader.c/.h      Proximity ADC
```

## Building

The firmware targets **ESP-IDF v6.0**.

| How | Command |
|---|---|
| VS Code | Run the task **Firmware: Build (Docker ESP-IDF)** |
| Docker | `docker run --rm -v "${PWD}/firmware:/project" -w /project espressif/idf idf.py build` |
| Local ESP-IDF | `cd firmware` then `idf.py build` |

CI ([firmware-build.yml](.github/workflows/firmware-build.yml)) builds every push and pull request to `development` that changes `firmware/`.

## Flashing

On Windows the Docker container has no access to the serial port. Flash from a local ESP-IDF install:

```
cd firmware
idf.py -p <PORT> flash monitor
```

## Runtime structure

| Context | Priority | Period | Role |
|---|---|---|---|
| TWAI RX ISR | ISR | per frame | Queues accepted frames |
| gptimer ISR | ISR | 100 ms | Sends `0x380`, `0x5BF` (EV-CAN) and `0x6C1` (display) when the state machine allows EV-CAN transmit |
| `rx_disp` | max − 2 | queue | Decodes RX frames into `g_sm_rx` |
| `twai1_tx` | max − 3 | 100 ms | Sends `0x050` to the Lear; suspended while the Lear must be silent |
| `state_sm` | max − 4 | 10 ms | Charge state machine; owns every output |

## Configuration

**Compile-time:** tunables are in [sm_config.h](firmware/main/sm_config.h), including:
- charge power limit, SOC cutoff and Lear target voltage;
- every timeout in the sequence;
- logging switches: `SM_LOG_VCM_TRACE` for VCM signal changes, `SM_LOG_LEAR_RAW` to capture all Lear IDs.

**Runtime:** the target voltage (300–400 V) and target SOC (50–100 %) can be changed over EV-CAN with the custom frame `0x6C0`. The current values are reported on `0x6C1`. See [twai0-can-frames.instructions.md](.github/instructions/twai0-can-frames.instructions.md), section 11.

## Specifications

The instruction files in [.github/instructions](.github/instructions) are the reference for the code:

| File | Covers |
|---|---|
| [twai0-can-frames](.github/instructions/twai0-can-frames.instructions.md) | EV-CAN frames: OBC TX encoding, VCM/LBC RX decoding, checksums |
| [twai1-can-frames](.github/instructions/twai1-can-frames.instructions.md) | Lear charger frames, including the AC input voltage on `0x27D` |
| [state-machine](.github/instructions/state-machine.instructions.md) | States, transitions and timings |
| [task-architecture](.github/instructions/task-architecture.instructions.md) | Tasks, ISRs and shared data |
| [twai-config](.github/instructions/twai-config.instructions.md), [mcpwm-capture-config](.github/instructions/mcpwm-capture-config.instructions.md), [adc-config](.github/instructions/adc-config.instructions.md), [gpio-outputs-config](.github/instructions/gpio-outputs-config.instructions.md) | Peripheral setup |
| [project-structure](.github/instructions/project-structure.instructions.md) | Layout and required files |

**Companion repository:** **LeafOBCBridge** holds the captures, the DBC files (`EV-can_ZE0.dbc`, `Lear Charger.dbc`), the OBC–VCM sequence analysis and the hardware design.

## Known limitations

- **Pilot ceiling:** the pilot current is capped at 32 A.
- **Unproven AC voltage scale:** the AC input voltage scale on `0x27D` is based on two voltage points (120 V and 240 V).
- **Safety override disabled:** the override that opens the contactors on TWAI0 bus-off or pilot loss is commented out in `state_machine_task()`. The current hardware hits both conditions during a normal charge.
