#include "state_machine.h"
#include "sm_config.h"
#include "esp_log.h"

static const char *TAG = "sm";
#include "gpio_config.h"
#include "twai_handler.h"
#include "mcpwm_capture.h"
#include "adc_reader.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Application States ─────────────────────────────────────────────────── */
typedef enum {
    SM_STATE_SLEEP = 0,
    SM_STATE_IDLE_NO_PLUG,
    SM_STATE_IDLE_PLUG,
    SM_STATE_VCM_AUTH,
    SM_STATE_EVSE_ENABLE,
    SM_STATE_CHARGING,
    SM_STATE_CHARGE_DISABLE,
} sm_state_t;

/* ── Task Handles ───────────────────────────────────────────────────────── */
TaskHandle_t state_machine_task_hdl = NULL;
extern TaskHandle_t twai1_tx_task_hdl;

/* ── SM Signal Flags ────────────────────────────────────────────────────── */
volatile bool  DRAM_ATTR g_sm_vcm_run            = false;
volatile bool  DRAM_ATTR g_sm_vcm_sleep          = false;
volatile bool  DRAM_ATTR g_sm_vcm_charge_cmd     = false;
volatile bool  DRAM_ATTR g_sm_lbc_failsafe       = false;
volatile bool  DRAM_ATTR g_sm_charge_interrupted = false;
volatile bool  DRAM_ATTR g_sm_twai1_rxd          = false;
volatile float DRAM_ATTR g_sm_soc_pct            = 0.0f;

/* ── State Entry Helpers ────────────────────────────────────────────────── */

static void sm_enter_sleep(void)
{
    vTaskSuspend(twai1_tx_task_hdl);
    gpio_set_level(GPIO_CP_ENABLE,  0);
    gpio_set_level(GPIO_CONT_POS,   0);
    gpio_set_level(GPIO_CONT_NEG,   0);
    gpio_set_level(GPIO_STATUS_LED, 0);
}

static void sm_enter_idle_no_plug(void)
{
    gpio_set_level(GPIO_CP_ENABLE,  0);
    gpio_set_level(GPIO_CONT_POS,   0);
    gpio_set_level(GPIO_CONT_NEG,   0);
    gpio_set_level(GPIO_STATUS_LED, 0);

    frame_5bf_data[4] = 0x28;   /* IDLE status */
    frame_5bf_data[2] = 0x00;   /* J1772 current = 0 */
    frame_380_data[4] &= (uint8_t)~(1u << 6);   /* AC relay bit = 0 */

    vTaskSuspend(twai1_tx_task_hdl);
}

static void sm_enter_idle_plug(void)
{
    gpio_set_level(GPIO_STATUS_LED, 1);

    /* Broadcast TIMER_WAIT2 (0x10) to trigger VCM wake sequence.
     * The state machine advances this to PLUG_BEEP (0x18) after 100 ms. */
    frame_5bf_data[4] = 0x10;
    frame_5bf_data[2] = 0x00;
}

static void sm_enter_vcm_auth(void)
{
    frame_5bf_data[4] = 0x30;   /* TIMER_WAIT */
}

static void sm_enter_evse_enable(void)
{
    frame_5bf_data[4] = 0x40;           /* CHARGE_INIT */
    frame_380_data[4] |= (1u << 6);     /* AC relay bit = 1 */

    /* Assert EVSE relay command — AC power begins flowing to OBC input */
    gpio_set_level(GPIO_CP_ENABLE, 1);

    /* Close HV charging contactors: NEG first (return path), POS after 500 ms */
    gpio_set_level(GPIO_CONT_NEG, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(GPIO_CONT_POS, 1);
}

static void sm_enter_charging(float session_amps)
{
    /* Encode frame_050 fully before resuming TWAI1 TX task */
    uint16_t volt_raw = (uint16_t)(SM_LEAR_TARGET_VOLTAGE_V * 10u);
    uint16_t curr_raw = (uint16_t)(session_amps * 10.0f);

    frame_050_data[0] = 0x00;
    frame_050_data[1] = 0xDC;
    frame_050_data[2] = (uint8_t)(volt_raw >> 8);
    frame_050_data[3] = (uint8_t)(volt_raw & 0xFFu);
    frame_050_data[4] = 0x00;
    frame_050_data[5] = (uint8_t)(curr_raw >> 8);
    frame_050_data[6] = (uint8_t)(curr_raw & 0xFFu);
    frame_050_data[7] = 0x01;   /* Lear_ChargeEnable = 1 */

    /* 0x5BF: CHARGING status + frozen J1772 current (amps × 5) */
    frame_5bf_data[4] = 0x60;
    frame_5bf_data[2] = (uint8_t)(session_amps * 5.0f);

    /* Resume TWAI1 TX — immediately preempts this task (higher priority) */
    vTaskResume(twai1_tx_task_hdl);

    gpio_set_level(GPIO_STATUS_LED, 1);
}

static void sm_enter_charge_disable(void)
{
    /* Step 1: clear ChargeEnable in the shared frame before suspending the task */
    frame_050_data[7] = 0x00;

    /* Step 2: suspend TWAI1 TX (best-effort last frame may carry ChargeEnable=0) */
    vTaskSuspend(twai1_tx_task_hdl);

    /* Step 3: de-assert EVSE relay — remove AC from OBC input */
    gpio_set_level(GPIO_CP_ENABLE, 0);

    /* Step 4: open HV contactors — POS first (positive break first), NEG after 500 ms */
    gpio_set_level(GPIO_CONT_POS, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(GPIO_CONT_NEG, 0);

    /* Step 5: clear 0x380 AC relay bit */
    frame_380_data[4] &= (uint8_t)~(1u << 6);

    /* Step 6: revert 0x5BF to IDLE */
    frame_5bf_data[4] = 0x28;

    /* Step 7: LED off */
    gpio_set_level(GPIO_STATUS_LED, 0);
}

/* ── J1772 Current Derivation ───────────────────────────────────────────── */
/*
 * Compute J1772 AC current limit from MCPWM pilot duty cycle.
 * MUST be called only while GPIO_CP_ENABLE is LOW (before EVSE_ENABLE entry).
 * Returns amperes, clamped to [0.0, 32.0] A.
 */
static float derive_j1772_current(uint32_t duty_us, uint32_t period_us)
{
    if (period_us == 0u) {
        return 0.0f;
    }
    float duty_pct = ((float)duty_us * 100.0f) / (float)period_us;
    float i_max;

    if (duty_pct >= 10.0f && duty_pct < 85.0f) {
        i_max = duty_pct * 0.6f;
    } else if (duty_pct >= 85.0f && duty_pct <= 96.0f) {
        i_max = (duty_pct - 64.0f) * 2.5f;
    } else {
        i_max = 0.0f;
    }

    if (i_max > 32.0f) i_max = 32.0f;
    if (i_max < 0.0f)  i_max = 0.0f;
    return i_max;
}

/* ── State Machine Task ─────────────────────────────────────────────────── */

void state_machine_task(void *pvParams)
{
    TickType_t last_wake = xTaskGetTickCount();

    static sm_state_t state = SM_STATE_SLEEP;

    /* IDLE_PLUG 0x10→0x18 timing: advance after 10 SM cycles (100 ms) */
    static int  idle_plug_ticks         = 0;
    static bool idle_plug_status_adv    = false;

    /* Sleep-path routing: set true when IDLE_PLUG entered via WAKE (from SLEEP).
     * Cleared when IDLE_PLUG is entered from IDLE_NO_PLUG or CHARGE_DISABLE. */
    static bool g_arrived_from_sleep    = false;

    /* EVA_REQ 1 s pulse countdown (100 × 10 ms cycles). Non-zero only on sleep path. */
    static int  eva_req_ticks           = 0;

    /* J1772 current derived in IDLE_PLUG; frozen at VCM charge command */
    float session_amps = 0.0f;
    float i_max_A      = 0.0f;

    for (;;) {
        /* ── 1. Read sensor inputs (DRAM_ATTR volatile — no blocking) ──── */
        uint32_t raw_adc   = g_adc_raw_avg;
        uint32_t duty_us   = g_pwm_duty_us;
        uint32_t period_us = g_pwm_period_us;
        bool     pwm_valid = g_pwm_signal_valid;

        /* ── 2. Calibrate ADC (only from task context — not ISR-safe) ───── */
        int adc_mv = 0;
        if (adc_cali_hdl) {
            adc_cali_raw_to_voltage(adc_cali_hdl, (int)raw_adc, &adc_mv);
        }

        /* ── 3. Latch and clear SM signal flags ─────────────────────────── */
        bool vcm_run    = g_sm_vcm_run;    g_sm_vcm_run    = false;
        bool vcm_sleep  = g_sm_vcm_sleep;  g_sm_vcm_sleep  = false;
        bool vcm_charge = g_sm_vcm_charge_cmd; g_sm_vcm_charge_cmd = false;
        bool lbc_fail   = g_sm_lbc_failsafe;   g_sm_lbc_failsafe   = false;
        bool chg_int    = g_sm_charge_interrupted; g_sm_charge_interrupted = false;
        bool t1_rxd     = g_sm_twai1_rxd;  g_sm_twai1_rxd  = false;
        float soc_pct   = g_sm_soc_pct;

        /* ── 4. State machine ───────────────────────────────────────────── */
        switch (state) {

        case SM_STATE_SLEEP:
            if (vcm_run) {
                sm_enter_idle_no_plug();
                ESP_LOGI(TAG, "SM: SLEEP -> IDLE_NO_PLUG (VCM_RUN)");
                state = SM_STATE_IDLE_NO_PLUG;
            } else if (adc_mv <= SM_ADC_NO_PLUG_MV && pwm_valid) {
                /* Autonomous wake: plug inserted with valid CP while VCM asleep.
                 * WAKE is zero-dwell — execute entry actions and advance to IDLE_PLUG
                 * in the same evaluation cycle. */
                g_twai0_tx_enabled   = true;          /* enable TX immediately; no 5 s gate */
                gpio_set_level(GPIO_EVA_REQ, 1);      /* begin 1 s EVA_REQ pulse */
                eva_req_ticks        = 100;            /* arm countdown (100 × 10 ms = 1 s) */
                g_arrived_from_sleep = true;           /* tag IDLE_PLUG for sleep-path routing */
                idle_plug_ticks      = 0;
                idle_plug_status_adv = false;
                i_max_A              = 0.0f;
                sm_enter_idle_plug();
                ESP_LOGI(TAG, "SM: SLEEP -> WAKE -> IDLE_PLUG (plug + CP)");
                state = SM_STATE_IDLE_PLUG;
            }
            break;

        case SM_STATE_IDLE_NO_PLUG:
            if (vcm_sleep) {
                sm_enter_sleep();
                ESP_LOGI(TAG, "SM: IDLE_NO_PLUG -> SLEEP (VCM_SLEEP)");
                state = SM_STATE_SLEEP;
            } else if (adc_mv <= SM_ADC_NO_PLUG_MV) {
                /* Zone A or B — plug present */
                g_arrived_from_sleep = false;   /* awake path: clear sleep flag */
                idle_plug_ticks      = 0;
                idle_plug_status_adv = false;
                i_max_A              = 0.0f;
                sm_enter_idle_plug();
                ESP_LOGI(TAG, "SM: IDLE_NO_PLUG -> IDLE_PLUG (plug detected)");
                state = SM_STATE_IDLE_PLUG;
            }
            break;

        case SM_STATE_IDLE_PLUG:
            /* Zone B (button held) — hold the advance timer at zero so charging
             * does not restart the instant the button is released. */
            if (adc_mv >= SM_ADC_BUTTON_MV) {
                idle_plug_ticks      = 0;
                idle_plug_status_adv = false;
                frame_5bf_data[4]    = 0x10;   /* revert to TIMER_WAIT2 */
            }

            /* Advance 0x5BF status 0x10 → 0x18 (PLUG_BEEP) after exactly 100 ms */
            if (!idle_plug_status_adv) {
                idle_plug_ticks++;
                if (idle_plug_ticks >= 10) {
                    frame_5bf_data[4]    = 0x18;   /* PLUG_BEEP */
                    idle_plug_status_adv = true;
                }
            }

            /* Derive J1772 current every cycle (CP_ENABLE is still LOW here) */
            if (pwm_valid) {
                i_max_A = derive_j1772_current(duty_us, period_us);
            } else {
                i_max_A = 0.0f;
            }
            frame_5bf_data[2] = (uint8_t)(i_max_A * 5.0f);   /* amps × 5 encoding */

            /* EVA_REQ countdown (sleep path only — no-op when eva_req_ticks == 0) */
            if (eva_req_ticks > 0) {
                eva_req_ticks--;
                if (eva_req_ticks == 0) {
                    gpio_set_level(GPIO_EVA_REQ, 0);   /* de-assert at exactly 1 s after WAKE */
                }
            }

            /* Evaluate exit conditions */
            if (vcm_sleep) {
                gpio_set_level(GPIO_EVA_REQ, 0);
                eva_req_ticks = 0;
                gpio_set_level(GPIO_STATUS_LED, 0);   /* IDLE_PLUG exit action */
                sm_enter_sleep();
                ESP_LOGI(TAG, "SM: IDLE_PLUG -> SLEEP (VCM_SLEEP)");
                state = SM_STATE_SLEEP;
            } else if (adc_mv > SM_ADC_NO_PLUG_MV) {
                /* Zone C — plug physically removed */
                gpio_set_level(GPIO_EVA_REQ, 0);
                eva_req_ticks = 0;
                if (g_arrived_from_sleep) {
                    gpio_set_level(GPIO_STATUS_LED, 0);
                    sm_enter_sleep();
                    ESP_LOGI(TAG, "SM: IDLE_PLUG -> SLEEP (plug removed, sleep path)");
                    state = SM_STATE_SLEEP;
                } else {
                    sm_enter_idle_no_plug();   /* also clears LED */
                    ESP_LOGI(TAG, "SM: IDLE_PLUG -> IDLE_NO_PLUG (plug removed)");
                    state = SM_STATE_IDLE_NO_PLUG;
                }
            } else if (!g_arrived_from_sleep && idle_plug_status_adv && adc_mv < SM_ADC_BUTTON_MV) {
                /* Awake path: auto-advance after PLUG_BEEP (100 ms), Zone A only */
                session_amps = i_max_A;
                gpio_set_level(GPIO_STATUS_LED, 0);   /* IDLE_PLUG exit action */
                sm_enter_evse_enable();
                ESP_LOGI(TAG, "SM: IDLE_PLUG -> EVSE_ENABLE (awake path, %.1f A)", session_amps);
                state = SM_STATE_EVSE_ENABLE;
            } else if (g_arrived_from_sleep && vcm_charge) {
                /* Sleep path: VCM charge command — freeze session current, route through VCM_AUTH */
                session_amps = i_max_A;
                gpio_set_level(GPIO_STATUS_LED, 0);   /* IDLE_PLUG exit action */
                sm_enter_vcm_auth();
                ESP_LOGI(TAG, "SM: IDLE_PLUG -> VCM_AUTH (sleep path, %.1f A)", session_amps);
                state = SM_STATE_VCM_AUTH;
            }
            break;

        case SM_STATE_VCM_AUTH:
            /* High-priority abort checks first */
            if (vcm_sleep) {
                gpio_set_level(GPIO_EVA_REQ, 0);
                eva_req_ticks = 0;
                sm_enter_sleep();
                ESP_LOGI(TAG, "SM: VCM_AUTH -> SLEEP (VCM_SLEEP)");
                state = SM_STATE_SLEEP;
            } else if (adc_mv > SM_ADC_NO_PLUG_MV) {
                /* Zone C — plug removed (VCM_AUTH only reachable via sleep path) */
                gpio_set_level(GPIO_EVA_REQ, 0);
                eva_req_ticks = 0;
                sm_enter_sleep();
                ESP_LOGI(TAG, "SM: VCM_AUTH -> SLEEP (plug removed)");
                state = SM_STATE_SLEEP;
            } else if (adc_mv >= SM_ADC_BUTTON_MV) {
                /* Zone B — button pressed, plug body still present;
                 * g_arrived_from_sleep preserved; eva_req_ticks countdown continues */
                idle_plug_ticks      = 0;
                idle_plug_status_adv = false;
                sm_enter_idle_plug();
                ESP_LOGI(TAG, "SM: VCM_AUTH -> IDLE_PLUG (button pressed)");
                state = SM_STATE_IDLE_PLUG;
            } else if (vcm_charge) {
                sm_enter_evse_enable();
                ESP_LOGI(TAG, "SM: VCM_AUTH -> EVSE_ENABLE (%.1f A)", session_amps);
                state = SM_STATE_EVSE_ENABLE;
            }
            break;

        case SM_STATE_EVSE_ENABLE:
            /* High-priority abort checks first */
            if (vcm_sleep || lbc_fail || adc_mv >= SM_ADC_BUTTON_MV) {
                sm_enter_charge_disable();
                ESP_LOGI(TAG, "SM: EVSE_ENABLE -> CHARGE_DISABLE (abort)");
                state = SM_STATE_CHARGE_DISABLE;
            } else if (t1_rxd) {
                /* OBC alive — any TWAI1 frame received */
                sm_enter_charging(session_amps);
                ESP_LOGI(TAG, "SM: EVSE_ENABLE -> CHARGING (%.1f A)", session_amps);
                state = SM_STATE_CHARGING;
            }
            break;

        case SM_STATE_CHARGING:
            if (adc_mv >= SM_ADC_BUTTON_MV  /* Zone B or C — button/plug removal */
                    || vcm_sleep
                    || lbc_fail
                    || chg_int
                    || soc_pct >= SM_SOC_CUTOFF_PCT) {
                sm_enter_charge_disable();
                ESP_LOGI(TAG, "SM: CHARGING -> CHARGE_DISABLE");
                state = SM_STATE_CHARGE_DISABLE;
            }
            break;

        case SM_STATE_CHARGE_DISABLE:
            /* Shutdown sequence completed inside sm_enter_charge_disable().
             * Determine next steady state from current plug reading. */
            g_arrived_from_sleep = false;   /* charge completed — VCM is now awake */
            if (adc_mv <= SM_ADC_NO_PLUG_MV) {
                /* Plug body still present after charge end */
                idle_plug_ticks     = 0;
                idle_plug_status_adv = false;
                i_max_A             = 0.0f;
                sm_enter_idle_plug();
                ESP_LOGI(TAG, "SM: CHARGE_DISABLE -> IDLE_PLUG");
                state = SM_STATE_IDLE_PLUG;
            } else {
                sm_enter_idle_no_plug();
                ESP_LOGI(TAG, "SM: CHARGE_DISABLE -> IDLE_NO_PLUG");
                state = SM_STATE_IDLE_NO_PLUG;
            }
            break;
        }

        /* ── 5. Safety override: immediate contactor de-assert ──────────── */
        // if (g_twai0_bus_off || !pwm_valid) {
        //     gpio_set_level(GPIO_CONT_POS, 0);
        //     gpio_set_level(GPIO_CONT_NEG, 0);
        // }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));
    }
}
