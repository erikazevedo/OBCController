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
    SM_STATE_CHARGE_DONE,
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
volatile bool  DRAM_ATTR g_sm_ev_on              = false;

volatile uint16_t DRAM_ATTR g_sm_target_voltage_v = SM_LEAR_TARGET_VOLTAGE_V;
volatile uint8_t  DRAM_ATTR g_sm_target_soc_pct   = (uint8_t)SM_SOC_CUTOFF_PCT;

/* Voltage currently encoded in frame_050_data[2..3] */
static uint16_t s_frame050_voltage_v = 0;
static portMUX_TYPE s_frame050_lock = portMUX_INITIALIZER_UNLOCKED;

/* Both bytes change inside one critical section so the higher-priority TWAI1 TX task never sees half an update */
static void sm_encode_frame_050_voltage(void)
{
    uint16_t volts    = g_sm_target_voltage_v;
    uint16_t volt_raw = (uint16_t)(volts * 10u);

    portENTER_CRITICAL(&s_frame050_lock);
    frame_050_data[2] = (uint8_t)(volt_raw >> 8);
    frame_050_data[3] = (uint8_t)(volt_raw & 0xFFu);
    portEXIT_CRITICAL(&s_frame050_lock);

    s_frame050_voltage_v = volts;
}

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
    uint16_t curr_raw = (uint16_t)(session_amps * 10.0f);

    frame_050_data[0] = 0x00;
    frame_050_data[1] = 0xDC;
    sm_encode_frame_050_voltage();
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

static void sm_enter_charge_done(void)
{
    frame_5bf_data[2] = 0x00;               /* clear J1772 current field */
    /* frame_5bf_data[4] == 0x28 already set by sm_enter_charge_disable() */
    gpio_set_level(GPIO_STATUS_LED, 1);     /* sustained on = charge complete */
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
    static int idle_plug_ticks          = 0;
    static bool idle_plug_status_adv    = false;

    /* Normal charge completion flag — set when SOC cutoff triggers CHARGE_DISABLE */
    static bool charge_done_normal      = false;

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
        float target_soc_pct = (float)g_sm_target_soc_pct;
        /* ── 4. State machine ───────────────────────────────────────────── */
        switch (state) {

        case SM_STATE_SLEEP:
            if (vcm_run) {
                sm_enter_idle_no_plug();
                ESP_LOGI(TAG, "SM: SLEEP -> IDLE_NO_PLUG (VCM_RUN)");
                state = SM_STATE_IDLE_NO_PLUG;
            }
            break;

        case SM_STATE_IDLE_NO_PLUG:
            if (vcm_sleep) {
                sm_enter_sleep();
                ESP_LOGI(TAG, "SM: IDLE_NO_PLUG -> SLEEP (VCM_SLEEP)");
                state = SM_STATE_SLEEP;
            } else if (adc_mv <= SM_ADC_NO_PLUG_MV) {
                /* Zone A or B — plug present */
                idle_plug_ticks     = 0;
                idle_plug_status_adv = false;
                i_max_A             = 0.0f;
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

            /* Evaluate exit conditions */
            if (vcm_sleep) {
                gpio_set_level(GPIO_STATUS_LED, 0);   /* IDLE_PLUG exit action */
                sm_enter_sleep();
                ESP_LOGI(TAG, "SM: IDLE_PLUG -> SLEEP (VCM_SLEEP)");
                state = SM_STATE_SLEEP;
            } else if (adc_mv > SM_ADC_NO_PLUG_MV) {
                /* Zone C — plug physically removed */
                sm_enter_idle_no_plug();   /* also clears LED */
                ESP_LOGI(TAG, "SM: IDLE_PLUG -> IDLE_NO_PLUG (plug removed)");
                state = SM_STATE_IDLE_NO_PLUG;
#if SM_SKIP_VCM_AUTH
            } else if (idle_plug_status_adv && adc_mv < SM_ADC_BUTTON_MV) {
                /* Auth skipped — advance automatically after PLUG_BEEP (100 ms), no VCM signal needed.
                 * Zone A only: button held (Zone B) keeps us here until released. */
                session_amps = i_max_A;
                gpio_set_level(GPIO_STATUS_LED, 0);   /* IDLE_PLUG exit action */
                sm_enter_evse_enable();
                ESP_LOGI(TAG, "SM: IDLE_PLUG -> EVSE_ENABLE (auth skipped, %.1f A)", session_amps);
                state = SM_STATE_EVSE_ENABLE;
#else
            } else if (vcm_charge) {
                /* VCM charge command — freeze session current while CP is still LOW */
                session_amps = i_max_A;
                gpio_set_level(GPIO_STATUS_LED, 0);   /* IDLE_PLUG exit action */
                sm_enter_vcm_auth();
                ESP_LOGI(TAG, "SM: IDLE_PLUG -> VCM_AUTH (charge cmd, %.1f A)", session_amps);
                state = SM_STATE_VCM_AUTH;
#endif
            }
            break;

        // No idea how tf vcm auth is supposed to work. in testing could never get it to trigger the beep
        // this state is skipped if SM_SKIP_VCM_AUTH is defined
        // of course this means the car has to be powered ON so the main contactor is closed
        // and the vcm essentially has no idea we are charging
        case SM_STATE_VCM_AUTH:
            /* High-priority abort checks first */
            if (vcm_sleep) {
                sm_enter_sleep();
                ESP_LOGI(TAG, "SM: VCM_AUTH -> SLEEP (VCM_SLEEP)");
                state = SM_STATE_SLEEP;
            } else if (adc_mv > SM_ADC_NO_PLUG_MV) {
                /* Zone C — plug removed */
                sm_enter_idle_no_plug();
                ESP_LOGI(TAG, "SM: VCM_AUTH -> IDLE_NO_PLUG (plug removed)");
                state = SM_STATE_IDLE_NO_PLUG;
            } else if (adc_mv >= SM_ADC_BUTTON_MV) {
                /* Zone B — button pressed, plug body still present */
                idle_plug_ticks     = 0;
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

        // Transient state where EVSE is enabled and we are waiting for OBC to boot
        case SM_STATE_EVSE_ENABLE:
            /* VCM sleep overrides everything — shut down and go dark immediately */

            // Car turned off
            if (vcm_sleep) {
                sm_enter_charge_disable();
                sm_enter_sleep();
                ESP_LOGI(TAG, "SM: EVSE_ENABLE -> SLEEP (VCM_SLEEP)");
                state = SM_STATE_SLEEP;

            // Button pressed/plug pulled or battery set a failsafe code
            // should we check for lbc failsafe in other states perhaps?
            // again here if the fault is persistent the state machine will repeatedly turn the EVSE on and off
            // which we probably do not want
            } else if (lbc_fail || adc_mv >= SM_ADC_BUTTON_MV) {
                sm_enter_charge_disable();
                ESP_LOGI(TAG, "SM: EVSE_ENABLE -> CHARGE_DISABLE (abort)");
                state = SM_STATE_CHARGE_DISABLE;

            // OBC has responded, proceed to charging
            } else if (t1_rxd) {
                /* OBC alive — any TWAI1 frame received */
                sm_enter_charging(session_amps);
                ESP_LOGI(TAG, "SM: EVSE_ENABLE -> CHARGING (%.1f A)", session_amps);
                state = SM_STATE_CHARGING;
            }
            break;

        case SM_STATE_CHARGING:
            /* VCM sleep overrides everything — shut down and go dark immediately */

            // Car turned off
            if (vcm_sleep) {
                sm_enter_charge_disable();
                sm_enter_sleep();
                ESP_LOGI(TAG, "SM: CHARGING -> SLEEP (VCM_SLEEP)");
                state = SM_STATE_SLEEP;

            // Reached target SOC, set flag to go to charge done state
            } else if (soc_pct >= target_soc_pct) {
                charge_done_normal = true;      /* mark as normal completion */
                sm_enter_charge_disable();
                ESP_LOGI(TAG, "SM: CHARGING -> CHARGE_DISABLE (SOC cutoff)");
                state = SM_STATE_CHARGE_DISABLE;
            
            // Plug pulled
            } else if (adc_mv >= SM_ADC_BUTTON_MV  /* Zone B or C — button/plug removal */
                    || lbc_fail
                    || chg_int) {
                sm_enter_charge_disable();
                ESP_LOGI(TAG, "SM: CHARGING -> CHARGE_DISABLE (abort)");
                state = SM_STATE_CHARGE_DISABLE;
            } else if (g_sm_target_voltage_v != s_frame050_voltage_v) {
                /* Display changed the target voltage mid-session */
                sm_encode_frame_050_voltage();
            }
            break;

        case SM_STATE_CHARGE_DISABLE:
            /* Shutdown sequence completed inside sm_enter_charge_disable().
             * Determine next steady state. VCM sleep takes priority; the 0x50B
             * message is periodic (100 ms) so the flag re-arms during the 500 ms
             * shutdown delay and is reliably set here when VCM wants to sleep. */

            // Car turned off
            if (vcm_sleep) {
                charge_done_normal = false;
                sm_enter_sleep();
                ESP_LOGI(TAG, "SM: CHARGE_DISABLE -> SLEEP (VCM_SLEEP)");
                state = SM_STATE_SLEEP;

            // charge completed to target SOC and plug still present
            } else if (charge_done_normal && adc_mv <= SM_ADC_NO_PLUG_MV) {
                /* Normal charge completion — plug still present */
                charge_done_normal = false;
                sm_enter_charge_done();
                ESP_LOGI(TAG, "SM: CHARGE_DISABLE -> CHARGE_DONE (SOC cutoff, plug present)");
                state = SM_STATE_CHARGE_DONE;

            // Aborted for some other reason but plug still present
            // Charge will immediately restart
            // should we add some kind of counter so it doesn't loop endlessly if there is a persistent fault?
            } else if (adc_mv <= SM_ADC_NO_PLUG_MV) {
                /* Aborted/faulted but plug still present */
                charge_done_normal = false;
                idle_plug_ticks     = 0;
                idle_plug_status_adv = false;
                i_max_A             = 0.0f;
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

        // Chill until plug is removed or vehicle turned off.
        // Added because when it was reaching the SOC cutoff, it was reverting to idle,
        // causing the charge to immediately restart and immedately hit the cutoff again,
        // repeating the cycle and spamming all the relays
        // 2026-07-17 because the car is still on in this state it actually is discharging
        // if left in this state for too long, the SOC will drop
        // observed a 5% drop over 30min to an hour
        // so let's add a trigger for it to go back to charging if the SOC drops below a certain threshold
        case SM_STATE_CHARGE_DONE:
            // Car turned off
            if (vcm_sleep) {
                gpio_set_level(GPIO_STATUS_LED, 0);
                sm_enter_sleep();
                ESP_LOGI(TAG, "SM: CHARGE_DONE -> SLEEP (VCM_SLEEP)");
                state = SM_STATE_SLEEP;

            // Plug removed
            } else if (adc_mv > SM_ADC_NO_PLUG_MV) {
                gpio_set_level(GPIO_STATUS_LED, 0);
                sm_enter_idle_no_plug();
                ESP_LOGI(TAG, "SM: CHARGE_DONE -> IDLE_NO_PLUG (plug removed)");
                state = SM_STATE_IDLE_NO_PLUG;

            // SOC dropped below the restart threshold, resume charging
            } else if (soc_pct <= (target_soc_pct - SM_SOC_RESTART_HYST)) {
                gpio_set_level(GPIO_STATUS_LED, 0);
                sm_enter_idle_plug();
                ESP_LOGI(TAG, "SM: CHARGE_DONE -> IDLE_PLUG (SOC dropped to %.1f%%)", soc_pct);
                state = SM_STATE_IDLE_PLUG;
            }

            // else if (!g_sm_ev_on) {
            //     gpio_set_level(GPIO_STATUS_LED, 0);
            //     sm_enter_idle_no_plug();
            //     ESP_LOGI(TAG, "SM: CHARGE_DONE -> IDLE_NO_PLUG (EV status 0x00)");
            //     state = SM_STATE_IDLE_NO_PLUG;
            // }
            break;
        }

        /* ── 5. Safety override: immediate contactor de-assert ──────────── */
        // Note the current hardware experiences both of these conditions during a normal charge
        // So keep this commented until ...someone... figures that out
        // if (g_twai0_bus_off || !pwm_valid) {
        //     gpio_set_level(GPIO_CONT_POS, 0);
        //     gpio_set_level(GPIO_CONT_NEG, 0);
        // }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));
    }
}
