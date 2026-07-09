# CHARGE_DONE State — Implementation Plan

## Overview

Add a new steady-state `SM_STATE_CHARGE_DONE` to the OBCController state machine. This state is entered after a **normal charge completion** (SOC reaching `SM_SOC_CUTOFF_PCT`), replacing the current behaviour of routing directly back to `IDLE_PLUG` or `IDLE_NO_PLUG`. The EV remains in a "charge complete / standby" condition until the user removes the plug or the EV system powers down.

---

## Problem with Current Flow

When `soc_pct >= SM_SOC_CUTOFF_PCT` fires in `CHARGING`, the state machine enters `CHARGE_DISABLE` and immediately exits to `IDLE_PLUG` (plug still present) or `IDLE_NO_PLUG` (plug removed). This is indistinguishable from an aborted or faulted session and could cause the VCM to attempt to re-start the charge. A dedicated `CHARGE_DONE` state provides a stable resting point that signals completion and prevents re-arming the charge sequence.

---

## New State: `SM_STATE_CHARGE_DONE`

### Description

A steady-state entered after a successful charge session (SOC cutoff reached). Controller remains in a minimal-power, plug-present configuration. TWAI0 TX continues (same content as IDLE_PLUG). TWAI1 TX stays suspended. No HV power active. The EV may show a "charge complete" indication via the 0x5BF status byte.

### Entry Point

`CHARGE_DISABLE` exit logic, **when the shutdown was triggered by `soc_pct >= SM_SOC_CUTOFF_PCT`** and plug is still present (`adc_mv <= SM_ADC_NO_PLUG_MV`).

### Entry Actions

1. `frame_5bf.d[4] = 0x28` (IDLE) — already set by `sm_enter_charge_disable()` step 6; no additional write needed.
2. `frame_5bf.d[2] = 0x00` — clear the J1772 current field (session ended).
3. `GPIO_STATUS_LED = HIGH` — sustained LED-on to indicate charge complete (distinct from the blinking or off states used elsewhere).
4. TWAI1 TX task remains suspended (already done in `CHARGE_DISABLE`).

### Exit Conditions (evaluated each 10 ms SM cycle, in priority order)

| Priority | Guard | Trigger | Next State |
|---|---|---|---|
| 1 | — | `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C — plug physically removed) | `IDLE_NO_PLUG` |
| 2 | — | `g_sm_ev_on == false` (0x50B `d[2] == 0x00` — EV system powers down) | `IDLE_NO_PLUG` |

> **Note on `g_sm_ev_on`**: The `handle_50b()` callback in `rx_dispatcher.c` already writes `g_sm_ev_on = (data[2] != 0x00u)` on every 0x50B frame. The state machine reads `g_sm_ev_on` from DRAM each cycle. When the EV system status byte drops to `0x00`, `g_sm_ev_on` becomes `false` — this is the exit trigger for CHARGE_DONE.

> **VCM sleep (`g_sm_vcm_sleep`) relationship**: A `0x50B` with `d[3][7:6] == 0b00` sets both `g_sm_vcm_sleep = true` and (because `cmd != 0x03`) implicitly `g_sm_ev_on` will have been last set on the preceding RUN frame. To avoid ambiguity, test `g_sm_vcm_sleep` as an additional high-priority exit: `vcm_sleep` → `SLEEP` (same as all other states). The `g_sm_ev_on == false` exit path handles the soft EV-off case where the VCM does not issue a SLEEP command.

### Revised Exit Table (including VCM sleep)

| Priority | Guard | Trigger | Next State |
|---|---|---|---|
| 1 | — | `vcm_sleep` (0x50B SLEEP) | `SLEEP` |
| 2 | — | `adc_mv > SM_ADC_NO_PLUG_MV` (Zone C — plug removed) | `IDLE_NO_PLUG` |
| 3 | — | `g_sm_ev_on == false` (0x50B `d[2] == 0x00`) | `IDLE_NO_PLUG` |

### Exit Actions

- For → `IDLE_NO_PLUG`: `GPIO_STATUS_LED = LOW` (cleared before `sm_enter_idle_no_plug()`).
- For → `SLEEP`: `GPIO_STATUS_LED = LOW` (cleared by `sm_enter_sleep()`).

### TWAI0 TX Obligations (same as IDLE_PLUG)

- `0x380` at 100 ms: AC relay bit = 0; power = `SM_MAX_CHARGE_POWER_W / 100`.
- `0x5BF` at 100 ms: status byte = `0x28` (IDLE); J1772 current field = `0x00`.

### TWAI1 Actions

None. TX task remains suspended.

---

## Required Code Changes

### 1. `state_machine.c` — `sm_state_t` enum

Add the new state value:

```c
typedef enum {
    SM_STATE_SLEEP = 0,
    SM_STATE_IDLE_NO_PLUG,
    SM_STATE_IDLE_PLUG,
    SM_STATE_VCM_AUTH,
    SM_STATE_EVSE_ENABLE,
    SM_STATE_CHARGING,
    SM_STATE_CHARGE_DISABLE,
    SM_STATE_CHARGE_DONE,      /* NEW */
} sm_state_t;
```

### 2. `state_machine.c` — charge done reason tracking

Add a `static bool` flag inside `state_machine_task()` to record *why* `CHARGE_DISABLE` was entered:

```c
static bool charge_done_normal = false;   /* true when SOC cutoff triggered CHARGE_DISABLE */
```

### 3. `state_machine.c` — `CHARGING` case

When `soc_pct >= SM_SOC_CUTOFF_PCT` triggers the transition, set the flag before entering `CHARGE_DISABLE`:

```c
} else if (soc_pct >= SM_SOC_CUTOFF_PCT) {
    charge_done_normal = true;      /* mark as normal completion */
    sm_enter_charge_disable();
    ESP_LOGI(TAG, "SM: CHARGING -> CHARGE_DISABLE (SOC cutoff)");
    state = SM_STATE_CHARGE_DISABLE;
```

All other `CHARGE_DISABLE` entries from `CHARGING` leave `charge_done_normal = false`.

### 4. `state_machine.c` — `CHARGE_DISABLE` exit logic

Update the exit branches to route to `CHARGE_DONE` when `charge_done_normal` is set and the plug is still present:

```c
case SM_STATE_CHARGE_DISABLE:
    if (vcm_sleep) {
        charge_done_normal = false;
        sm_enter_sleep();
        ESP_LOGI(TAG, "SM: CHARGE_DISABLE -> SLEEP (VCM_SLEEP)");
        state = SM_STATE_SLEEP;
    } else if (charge_done_normal && adc_mv <= SM_ADC_NO_PLUG_MV) {
        /* Normal charge completion — plug still present */
        charge_done_normal = false;
        sm_enter_charge_done();
        ESP_LOGI(TAG, "SM: CHARGE_DISABLE -> CHARGE_DONE (SOC cutoff, plug present)");
        state = SM_STATE_CHARGE_DONE;
    } else if (adc_mv <= SM_ADC_NO_PLUG_MV) {
        /* Aborted/faulted but plug still present */
        charge_done_normal = false;
        idle_plug_ticks      = 0;
        idle_plug_status_adv = false;
        i_max_A              = 0.0f;
        sm_enter_idle_plug();
        ESP_LOGI(TAG, "SM: CHARGE_DISABLE -> IDLE_PLUG");
        state = SM_STATE_IDLE_PLUG;
    } else {
        charge_done_normal = false;
        sm_enter_idle_no_plug();
        ESP_LOGI(TAG, "SM: CHARGE_DISABLE -> IDLE_NO_PLUG");
        state = SM_STATE_IDLE_NO_PLUG;
    }
    break;
```

### 5. `state_machine.c` — new `sm_enter_charge_done()` helper

```c
static void sm_enter_charge_done(void)
{
    frame_5bf_data[2] = 0x00;               /* clear J1772 current field */
    /* frame_5bf_data[4] == 0x28 already set by sm_enter_charge_disable() */
    gpio_set_level(GPIO_STATUS_LED, 1);     /* sustained on = charge complete */
}
```

### 6. `state_machine.c` — new `SM_STATE_CHARGE_DONE` case

```c
case SM_STATE_CHARGE_DONE:
    if (vcm_sleep) {
        gpio_set_level(GPIO_STATUS_LED, 0);
        sm_enter_sleep();
        ESP_LOGI(TAG, "SM: CHARGE_DONE -> SLEEP (VCM_SLEEP)");
        state = SM_STATE_SLEEP;
    } else if (adc_mv > SM_ADC_NO_PLUG_MV) {
        gpio_set_level(GPIO_STATUS_LED, 0);
        sm_enter_idle_no_plug();
        ESP_LOGI(TAG, "SM: CHARGE_DONE -> IDLE_NO_PLUG (plug removed)");
        state = SM_STATE_IDLE_NO_PLUG;
    } else if (!g_sm_ev_on) {
        gpio_set_level(GPIO_STATUS_LED, 0);
        sm_enter_idle_no_plug();
        ESP_LOGI(TAG, "SM: CHARGE_DONE -> IDLE_NO_PLUG (EV status 0x00)");
        state = SM_STATE_IDLE_NO_PLUG;
    }
    break;
```

### 7. `state_machine.h` — no new public signals required

`g_sm_ev_on` is already declared as `extern volatile bool`. No new additions needed.

### 8. `state-machine.instructions.md` — document the new state and transitions

Add a `### CHARGE_DONE` section and update the transitions table to include:

| From | To | Guard | Trigger | Priority |
|---|---|---|---|---|
| CHARGE_DISABLE | CHARGE_DONE | `charge_done_normal && adc_mv <= SM_ADC_NO_PLUG_MV` | Shutdown complete, SOC cutoff path, plug present | Normal |
| CHARGE_DONE | SLEEP | — | `vcm_sleep` | High |
| CHARGE_DONE | IDLE_NO_PLUG | — | `adc_mv > SM_ADC_NO_PLUG_MV` (plug removed) | Normal |
| CHARGE_DONE | IDLE_NO_PLUG | — | `g_sm_ev_on == false` (EV status byte = 0x00) | Normal |

---

## State Diagram — Updated Flow (SOC Cutoff Path)

```
CHARGING ─(soc_pct >= SM_SOC_CUTOFF_PCT)─► CHARGE_DISABLE
                                                │
                              ┌─────────────────┤
                              │ charge_done_normal && plug present
                              ▼
                          CHARGE_DONE ◄── LED on (charge complete)
                              │
              ┌───────────────┼───────────────────────┐
              ▼               ▼                       ▼
          SLEEP         IDLE_NO_PLUG            IDLE_NO_PLUG
      (vcm_sleep)      (plug removed)       (g_sm_ev_on == false)
```

For all other `CHARGE_DISABLE` entry reasons (ADC abort, LBC failsafe, charge interrupted, VCM sleep), the existing `IDLE_PLUG` / `IDLE_NO_PLUG` / `SLEEP` exits are unchanged.

---

## `sm_config.h` Changes

No new macros required. `CHARGE_DONE` has no tunable parameters.

---

## Files to Modify

| File | Change |
|---|---|
| `firmware/main/state_machine.c` | Add enum value, `charge_done_normal` flag, `sm_enter_charge_done()`, new `case`, updated `CHARGE_DISABLE` exit, updated `CHARGING` SOC-cutoff arm |
| `firmware/main/state_machine.h` | No changes required |
| `firmware/main/sm_config.h` | No changes required |
| `.github/instructions/state-machine.instructions.md` | Add `CHARGE_DONE` state section and update transitions table |

---

## Open Questions

1. **LED behaviour in CHARGE_DONE**: The plan uses sustained LED-on to indicate charge complete. Confirm this is the desired behaviour, or specify an alternative (e.g. slow blink, off).
2. **`frame_5bf.d[4]` in CHARGE_DONE**: Plan reuses `0x28` (IDLE). Confirm no dedicated "charge complete" status byte exists in the ZE0 EV-CAN spec for this field.
3. **Re-charge prevention**: If `g_sm_ev_on` becomes `false` and then `true` again while the plug is still inserted (EV soft-cycles), the SM will transition `IDLE_NO_PLUG → IDLE_PLUG` and could re-arm a new session. If this is undesirable, a "session exhausted" latch cleared only on plug removal should be added to `sm_config.h`.
