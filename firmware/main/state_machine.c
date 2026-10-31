#include "state_machine.h"
#include "sm_config.h"
#include "esp_log.h"

static const char *TAG = "sm";
#include <string.h>
#include "gpio_config.h"
#include "twai_handler.h"
#include "mcpwm_capture.h"
#include "adc_reader.h"
#include "obc_frames.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Application States ─────────────────────────────────────────────────── */
typedef enum {
    SM_STATE_SLEEP = 0,
    SM_STATE_IDLE_NO_PLUG,
    SM_STATE_IDLE_PLUG,
    SM_STATE_EVSE_ENABLE,
    SM_STATE_CHARGING,
    SM_STATE_CHARGE_DISABLE,
    SM_STATE_CHARGE_DONE,
    /* VCM-authorised path: SM_SKIP_VCM_AUTH == 0 and power switch OFF */
    SM_STATE_VCM_WAKE,        /* activation pulse, waiting for 0x50B WakeUp */
    SM_STATE_VCM_AUTH,        /* on EV-CAN as the OBC, EVSE announced, waiting for the 0x1F2 request */
    SM_STATE_VCM_STANDBY,     /* stop request acknowledged: timer pending, refused, or after a charge */
    SM_STATE_AUTH_START,      /* normal charge requested: HV live, contactors, 0x48..0x58, power command */
    SM_STATE_AUTH_CHARGING,   /* Lear enabled within the VCM power command */
    SM_STATE_AUTH_STOP,       /* ramp down, release State C, open the contactors */
    SM_STATE_VCM_SHUTDOWN,    /* after GoToSleep: 0x5BF ASLEEP briefly, then silent */
} sm_state_t;

/* ── Task Handles ───────────────────────────────────────────────────────── */
TaskHandle_t state_machine_task_hdl = NULL;
extern TaskHandle_t twai1_tx_task_hdl;

/* ── SM Signal Flags ────────────────────────────────────────────────────── */
volatile bool  DRAM_ATTR g_sm_vcm_run            = false;
volatile bool  DRAM_ATTR g_sm_vcm_sleep          = false;
volatile bool  DRAM_ATTR g_sm_lbc_failsafe       = false;
volatile bool  DRAM_ATTR g_sm_charge_interrupted = false;
volatile bool  DRAM_ATTR g_sm_twai1_rxd          = false;
volatile float DRAM_ATTR g_sm_soc_pct            = 0.0f;
volatile bool  DRAM_ATTR g_sm_ev_on              = false;

sm_rx_status_t g_sm_rx = { 0 };

volatile uint16_t DRAM_ATTR g_sm_target_voltage_v = SM_LEAR_TARGET_VOLTAGE_V;
volatile uint8_t  DRAM_ATTR g_sm_target_soc_pct   = (uint8_t)SM_SOC_CUTOFF_PCT;

/* Voltage currently encoded in frame_050_data[2..3] */
static uint16_t s_frame050_voltage_v = 0;

/* Both bytes change inside one critical section so the higher-priority TWAI1 TX task never sees half an update */
static void sm_encode_frame_050_voltage(void)
{
    uint16_t volts    = g_sm_target_voltage_v;
    uint16_t volt_raw = (uint16_t)(volts * 10u);

    portENTER_CRITICAL(&g_frame050_lock);
    frame_050_data[2] = (uint8_t)(volt_raw >> 8);
    frame_050_data[3] = (uint8_t)(volt_raw & 0xFFu);
    portEXIT_CRITICAL(&g_frame050_lock);

    s_frame050_voltage_v = volts;
}

/* ── Bypass-path context (file scope so the auth path can hand over to IDLE_PLUG) ── */

/* IDLE_PLUG 0x10→0x18 timing: advance after 10 SM cycles (100 ms) */
static int   idle_plug_ticks       = 0;
static bool  idle_plug_status_adv  = false;

/* Normal charge completion flag — set when SOC cutoff triggers CHARGE_DISABLE */
static bool  charge_done_normal    = false;

/* J1772 current derived in IDLE_PLUG; frozen at VCM charge command */
static float session_amps          = 0.0f;
static float i_max_A               = 0.0f;

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
    /* Step 1: clear ChargeEnable in the shared frame (and in any 0x050 still queued) before suspending the task */
    twai1_frame_050_set_enable(false);

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

/* ════════════════════════════════════════════════════════════════════════════
 * VCM-authorised path (SM_SKIP_VCM_AUTH == 0, power switch OFF)
 *
 * The bridge wakes the VCM with the EV activation pulse and then behaves like
 * the stock OBC in the ZE0 AC-charge capture (LeafOBCBridge/docs/
 * OBC-VCM-State-Sequence.md §3/§7): appear on EV-CAN, announce the EVSE, wait
 * for the 0x1F2 normal-charge request, close the charge contactors once the
 * VCM's main relays have made the HV bus live, charge within the VCM power
 * command, and run the stock stop / acknowledge / sleep sequence.
 * ════════════════════════════════════════════════════════════════════════════ */

#define MS_MAX           0x7FFFFFFFu
#define FRAMES_LOCK()    portENTER_CRITICAL(&g_twai0_frame_lock)
#define FRAMES_UNLOCK()  portEXIT_CRITICAL(&g_twai0_frame_lock)

typedef enum { PLUG_NONE = 0, PLUG_IN, PLUG_BUTTON } plug_zone_t;

typedef enum {
    STOP_RELEASE,   /* connector button or unplug: release switch follows the button, 0x5BF 0x80 */
    STOP_SOC,       /* SOC target: a short emulated release-switch press, 0x5BF 0x80 */
    STOP_FAULT,     /* bridge-side fault: 0x5BF 0x80 */
    STOP_VCM,       /* the VCM ended the session: 0x5BF 0x70 */
} stop_kind_t;

/* Age of a CAN source, measured with the SM's own clock from a frame counter */
typedef struct {
    uint32_t cnt;
    uint32_t age_ms;
} rx_age_t;

/* One cycle's view of the inputs the auth path uses */
typedef struct {
    plug_zone_t zone;
    float       pilot_a;        /* valid only while State C is released */
    bool        bus_alive;      /* 0x50B within SM_VCM_SILENT_MS */
    bool        awake;          /* ... and WakeUp */
    bool        gotosleep;      /* ... and GoToSleep */
    uint32_t    gotosleep_ms;   /* GoToSleep held for (0 while awake) */
    bool        vcm_fresh;      /* 0x50B, 0x1F2 or 0x1D4 within SM_TX_VCM_FRESH_MS */
    bool        power_on;       /* ... and power switch ON */
    bool        wake_edge;      /* VCM woke since the last cycle */
    bool        beacon_recent;  /* 0x603/0x68C within SM_BEACON_WINDOW_MS */
    bool        f1f2_fresh;
    uint8_t     req;            /* 0x1F2 request, NONE when stale */
    uint16_t    power_cmd_w;    /* 0 when stale */
    bool        d4_fresh;
    bool        hv_live;
    bool        relay_plus;
    bool        session;
    bool        bus_v_fresh;
    uint16_t    bus_dv;
    bool        lear_alive;     /* AC at the Lear */
    bool        dc_fresh;
    uint16_t    dc_dv;
    uint16_t    dc_da;
    bool        lbc_fail;
    bool        soc_valid;
    float       soc_pct;
    float       target_soc;
} sm_in_t;

/* Per-session auth context; cleared when the bridge (re)appears on EV-CAN */
static struct {
    uint32_t    st_ms;          /* time in the current state */
    uint32_t    sub_ms;         /* time in the current sub-step */
    uint8_t     sub;
    bool        appeared;       /* 0x5BF 0x10 → 0x18 done */
    uint32_t    ann_ms;         /* plug in with a valid pilot, before State C */
    bool        ready;          /* 0x380 EVSE ready announced */
    bool        s2_closed;      /* State C asserted, 0x380 S2 set */
    uint32_t    s2_ms;
    uint32_t    s2_open_ms;
    bool        pilot_set;      /* 0x5BF pilot reported: announcement complete */
    float       amps;           /* pilot latched just before State C */
    bool        lear_stale;     /* Lear already broadcasting when State C was asserted (left over from the last session) */
    uint32_t    ac_wait_ms;     /* State C asserted, AC not yet confirmed, for */
    bool        ac_reported;
    bool        ac_type_set;
    uint32_t    ac_ms;
    bool        contactors;     /* a charge contactor is closed (CONT_NEG closes first) */
    bool        pos_closed;     /* CONT_POS closed: the Lear is connected to the HV bus */
    bool        cmd_seen;       /* VCM power command > 0 seen */
    bool        session_seen;   /* 0x1D4 charge-session bit seen set */
    bool        acked;          /* stop request acknowledged */
    bool        saw_stop;       /* stop request seen in standby: its withdrawal is a VCM restart */
    bool        release_emul;   /* emulated release-switch press (SOC target stop) */
    bool        soc_logged;
    bool        fast;           /* VCM gone: shorten the stop sequence */
    uint32_t    cnt617_at_stop;
    uint16_t    lear_limit_da;
} s_auth;

static rx_age_t s_age_50b    = { 0, MS_MAX };
static rx_age_t s_age_beacon = { 0, MS_MAX };
static rx_age_t s_age_1f2    = { 0, MS_MAX };
static rx_age_t s_age_1d4    = { 0, MS_MAX };
static rx_age_t s_age_1da    = { 0, MS_MAX };
static rx_age_t s_age_lear   = { 0, MS_MAX };
static rx_age_t s_age_617    = { 0, MS_MAX };
static uint32_t s_wake_count_seen = 0;

/* Plug-session latches, cleared when the plug is removed */
static uint8_t  s_wake_attempts  = 0;      /* bridge-initiated wakes since the last charge */
static uint8_t  s_fault_stops    = 0;
static bool     s_session_done   = false;  /* charge finished or stopped by the user: no more bridge-initiated wakes */
static bool     s_soc_stopped    = false;  /* stopped at the SOC target: restart needs SM_SOC_RESTART_HYST */
static bool     s_gave_up_logged = false;

static uint32_t s_quiet_ms     = 0;        /* EV-CAN silent, and no failed wake attempt, for */
static uint32_t s_pilot_ok_ms  = 0;        /* plug in zone A with a valid pilot for */
static uint32_t s_eva_left_ms  = 0;
static bool     s_plug_sig     = false;
static bool     s_btn_wake     = false;    /* SLEEP: VCM woke after 0x603/0x68C, waiting for power ON */
static uint32_t s_btn_wake_ms  = 0;

static inline void sat_add(uint32_t *t, uint32_t dt)
{
    *t = (*t >= MS_MAX - dt) ? MS_MAX : *t + dt;
}

static void rx_age_tick(rx_age_t *a, uint32_t cnt, uint32_t dt)
{
    if (cnt != a->cnt) {
        a->cnt    = cnt;
        a->age_ms = 0;
    } else {
        sat_add(&a->age_ms, dt);
    }
}

static plug_zone_t plug_zone(int adc_mv)
{
    if (adc_mv > SM_ADC_NO_PLUG_MV) return PLUG_NONE;
    if (adc_mv >= SM_ADC_BUTTON_MV) return PLUG_BUTTON;
    return PLUG_IN;
}

static const char *sm_state_name(sm_state_t s)
{
    switch (s) {
    case SM_STATE_SLEEP:          return "SLEEP";
    case SM_STATE_IDLE_NO_PLUG:   return "IDLE_NO_PLUG";
    case SM_STATE_IDLE_PLUG:      return "IDLE_PLUG";
    case SM_STATE_EVSE_ENABLE:    return "EVSE_ENABLE";
    case SM_STATE_CHARGING:       return "CHARGING";
    case SM_STATE_CHARGE_DISABLE: return "CHARGE_DISABLE";
    case SM_STATE_CHARGE_DONE:    return "CHARGE_DONE";
    case SM_STATE_VCM_WAKE:       return "VCM_WAKE";
    case SM_STATE_VCM_AUTH:       return "VCM_AUTH";
    case SM_STATE_VCM_STANDBY:    return "VCM_STANDBY";
    case SM_STATE_AUTH_START:     return "AUTH_START";
    case SM_STATE_AUTH_CHARGING:  return "AUTH_CHARGING";
    case SM_STATE_AUTH_STOP:      return "AUTH_STOP";
    case SM_STATE_VCM_SHUTDOWN:   return "VCM_SHUTDOWN";
    }
    return "?";
}

/* States in which the bridge is on EV-CAN as the OBC under VCM control */
static bool sm_state_is_auth_tx(sm_state_t s)
{
    return s == SM_STATE_VCM_AUTH || s == SM_STATE_VCM_STANDBY || s == SM_STATE_AUTH_START ||
           s == SM_STATE_AUTH_CHARGING || s == SM_STATE_AUTH_STOP || s == SM_STATE_VCM_SHUTDOWN;
}

static sm_state_t sm_go(sm_state_t from, sm_state_t to, const char *why)
{
    ESP_LOGI(TAG, "SM: %s -> %s (%s)", sm_state_name(from), sm_state_name(to), why);
    return to;
}

static void sm_auth_sub(uint8_t sub)
{
    s_auth.sub    = sub;
    s_auth.sub_ms = 0;
}

static void sm_read_inputs(sm_in_t *in, uint32_t dt, int adc_mv, float pilot_a,
                           bool lbc_fail, float target_soc)
{
    rx_age_tick(&s_age_50b,    g_sm_rx.cnt_50b,    dt);
    rx_age_tick(&s_age_beacon, g_sm_rx.cnt_beacon, dt);
    rx_age_tick(&s_age_1f2,    g_sm_rx.cnt_1f2,    dt);
    rx_age_tick(&s_age_1d4,    g_sm_rx.cnt_1d4,    dt);
    rx_age_tick(&s_age_1da,    g_sm_rx.cnt_1da,    dt);
    rx_age_tick(&s_age_lear,   g_sm_rx.cnt_lear,   dt);
    rx_age_tick(&s_age_617,    g_sm_rx.cnt_617,    dt);

    uint32_t wc = g_sm_rx.wake_count;
    in->wake_edge     = (wc != s_wake_count_seen);
    s_wake_count_seen = wc;

    in->zone          = plug_zone(adc_mv);
    in->pilot_a       = pilot_a;
    in->bus_alive     = s_age_50b.age_ms < SM_VCM_SILENT_MS;
    in->awake         = in->bus_alive && g_sm_rx.awake;
    in->gotosleep     = in->bus_alive && !g_sm_rx.awake;
    in->power_on      = in->bus_alive && g_sm_rx.power_on;
    in->beacon_recent = s_age_beacon.age_ms < SM_BEACON_WINDOW_MS;

    static bool     s_was_gotosleep = false;
    static uint32_t s_gotosleep_ms  = 0;
    if (!in->gotosleep) {
        s_gotosleep_ms = 0;
    } else if (s_was_gotosleep) {
        sat_add(&s_gotosleep_ms, dt);
    }
    s_was_gotosleep  = in->gotosleep;
    in->gotosleep_ms = s_gotosleep_ms;

    uint32_t vcm_age = s_age_50b.age_ms;
    if (s_age_1f2.age_ms < vcm_age) vcm_age = s_age_1f2.age_ms;
    if (s_age_1d4.age_ms < vcm_age) vcm_age = s_age_1d4.age_ms;
    in->vcm_fresh = vcm_age <= SM_TX_VCM_FRESH_MS;

    in->f1f2_fresh    = s_age_1f2.age_ms < SM_VCM_SILENT_MS;
    in->req           = in->f1f2_fresh ? g_sm_rx.req_1f2 : VCM1F2_REQ_NONE;
    in->power_cmd_w   = in->f1f2_fresh ? g_sm_rx.power_cmd_w : 0u;

    in->d4_fresh      = s_age_1d4.age_ms < SM_VCM_SILENT_MS;
    in->hv_live       = in->d4_fresh && g_sm_rx.hv_live;
    in->relay_plus    = in->d4_fresh && g_sm_rx.relay_plus;
    in->session       = in->d4_fresh && g_sm_rx.charge_session;

    in->bus_v_fresh   = s_age_1da.age_ms < SM_VCM_SILENT_MS;
    in->bus_dv        = g_sm_rx.hv_bus_dv;

    in->lear_alive    = s_age_lear.age_ms < SM_LEAR_ALIVE_MS;
    in->dc_fresh      = s_age_617.age_ms < SM_LEAR_ALIVE_MS;
    in->dc_dv         = g_sm_rx.lear_dc_dv;
    in->dc_da         = g_sm_rx.lear_dc_da;

    in->lbc_fail      = lbc_fail;
    in->soc_valid     = g_sm_rx.soc_valid;
    in->soc_pct       = (float)g_sm_rx.soc_x10 * 0.1f;
    in->target_soc    = target_soc;
}

/* ── EV system activation pulse (GPIO_EVA_REQ → OBC pin 18 → VCM pin 96) ── */
static void sm_eva_start(void)
{
    gpio_set_level(GPIO_EVA_REQ, 1);
    s_eva_left_ms = SM_EVA_PULSE_MS;
}

static void sm_eva_stop(void)
{
    gpio_set_level(GPIO_EVA_REQ, 0);
    s_eva_left_ms = 0;
}

/* Runs every cycle in every state, so the line is never left asserted */
static void sm_eva_tick(uint32_t dt)
{
    if (s_eva_left_ms == 0u) {
        return;
    }
    if (dt >= s_eva_left_ms) {
        sm_eva_stop();
    } else {
        s_eva_left_ms -= dt;
    }
}

/* ── Signal trace (SM_LOG_VCM_TRACE) ─────────────────────────────────────── */
static const char *vcm_mode_name(uint8_t m)
{
    if (m <= 9u)                return "start-up";
    if (m == 10u)               return "awake, no charge";
    if (m <= 29u)               return "to READY";
    if (m == 30u)               return "READY";
    if (m <= 68u)               return "shutdown";
    if (m >= 109u && m <= 129u) return "entering charge";
    if (m == 130u)              return "charging";
    if (m >= 131u && m <= 149u) return "charge shutdown";
    if (m >= 211u && m <= 229u) return "entering 12V/remote charge";
    if (m == 230u)              return "12V/remote charge";
    if (m >= 231u && m <= 246u) return "12V/remote shutdown";
    return "?";
}

static const char *vcm_req_name(uint8_t req)
{
    switch (req) {
    case VCM1F2_REQ_NORMAL: return "NORMAL";
    case VCM1F2_REQ_QUICK:  return "QUICK";
    case VCM1F2_REQ_STOP:   return "STOP";
    default:                return "none";
    }
}

/* LBC flags that can make the VCM refuse or end a charge (0x1DB, 0x1DC), with the SOC */
static void sm_trace_lbc(void)
{
    unsigned chgr = g_sm_rx.lbc_max_chgr_raw;
    ESP_LOGI(TAG, "LBC full charge=%d main relay=%d failsafe=%u relay cut=%u | 0x1DC charge power status=%u, "
                  "for-charger raw x0.1 = %u.%u | SOC %u.%u%%%s",
             (int)g_sm_rx.lbc_full, (int)g_sm_rx.lbc_main_relay, (unsigned)g_sm_rx.lbc_failsafe,
             (unsigned)g_sm_rx.lbc_relay_cut, (unsigned)g_sm_rx.lbc_chg_status, chgr / 10u, chgr % 10u,
             (unsigned)(g_sm_rx.soc_x10 / 10u), (unsigned)(g_sm_rx.soc_x10 % 10u),
             g_sm_rx.soc_valid ? "" : " (not valid)");
}

static void sm_trace(const sm_in_t *in, uint32_t dt)
{
    static int      s_bus     = -1;
    static uint16_t s_50b     = 0xFFFFu;
    static uint32_t s_1f2     = 0xFFFFFFFFu;
    static uint16_t s_cmd     = 0xFFFFu;
    static uint32_t s_cmd_ms  = MS_MAX;
    static uint16_t s_1d4     = 0xFFFFu;
    static uint32_t s_1d4_ms  = MS_MAX;
    static uint16_t s_bus_dv  = 0xFFFFu;
    static uint32_t s_bus_ms  = MS_MAX;
    static int      s_lear    = -1;
    static uint32_t s_lear_ms = MS_MAX;
    static uint32_t s_beacon  = 0;
    static uint32_t s_beacon_ms = MS_MAX;
    static uint32_t s_echo    = 0xFFFFFFFFu;
    static uint16_t s_lbc     = 0xFFFFu;

    sat_add(&s_cmd_ms, dt);
    sat_add(&s_bus_ms, dt);
    sat_add(&s_lear_ms, dt);
    sat_add(&s_beacon_ms, dt);
    sat_add(&s_1d4_ms, dt);

    if ((int)in->bus_alive != s_bus) {
        ESP_LOGI(TAG, "VCM EV-CAN %s", in->bus_alive ? "active" : "silent");
        s_bus = in->bus_alive;
    }
    if (g_sm_rx.cnt_beacon != s_beacon) {
        if (s_beacon_ms >= 5000u) {
            ESP_LOGI(TAG, "VCM 0x603/0x68C seen (power-switch wake marker)");
        }
        s_beacon    = g_sm_rx.cnt_beacon;
        s_beacon_ms = 0;
    }
    if (in->bus_alive) {
        uint16_t v = (uint16_t)((g_sm_rx.d2_50b << 8) | g_sm_rx.d3_50b);
        if (v != s_50b) {
            ESP_LOGI(TAG, "VCM 0x50B d2=%02X d3=%02X (%s, power switch %s)",
                     (unsigned)g_sm_rx.d2_50b, (unsigned)g_sm_rx.d3_50b,
                     g_sm_rx.awake ? "WakeUp" : "GoToSleep", g_sm_rx.power_on ? "ON" : "OFF");
            s_50b = v;
        }
    }
    if (in->f1f2_fresh) {
        uint8_t  d0 = g_sm_rx.d0_1f2, d2 = g_sm_rx.d2_1f2, mode = g_sm_rx.mode_1f2;
        uint32_t v  = ((uint32_t)d0 << 16) | ((uint32_t)d2 << 8) | mode;
        if (v != s_1f2) {
            ESP_LOGI(TAG, "VCM 0x1F2 d0=%02X d2=%02X req=%s mode=%u (%s)", (unsigned)d0, (unsigned)d2,
                     vcm_req_name((uint8_t)(d2 & VCM1F2_REQ_MASK)), (unsigned)mode, vcm_mode_name(mode));
            s_1f2 = v;
        }
        uint16_t cmd = in->power_cmd_w;
        if (cmd != s_cmd && (s_cmd_ms >= 250u || cmd == 0u || s_cmd == 0u || s_cmd == 0xFFFFu)) {
            ESP_LOGI(TAG, "VCM 0x1F2 max charge power %u W (d1=%02X)", (unsigned)cmd, (unsigned)g_sm_rx.d1_1f2);
            s_cmd    = cmd;
            s_cmd_ms = 0;
        }
    }
    if (in->d4_fresh) {
        /* The three decoded bits log on change; d[6] alone (it varies while driving) at most once a second */
        uint16_t bits = (uint16_t)((in->hv_live << 2) | (in->relay_plus << 1) | in->session);
        uint16_t v    = (uint16_t)((g_sm_rx.d6_1d4 << 8) | bits);
        if (v != s_1d4 && ((v & 0xFFu) != (s_1d4 & 0xFFu) || s_1d4_ms >= 1000u)) {
            ESP_LOGI(TAG, "VCM 0x1D4 HV live=%d relay+=%d charge session=%d d6=%02X",
                     in->hv_live, in->relay_plus, in->session, (unsigned)g_sm_rx.d6_1d4);
            s_1d4    = v;
            s_1d4_ms = 0;
        }
    }
    if (in->bus_v_fresh && s_bus_ms >= 250u &&
        (s_bus_dv == 0xFFFFu || in->bus_dv > s_bus_dv + 200u || in->bus_dv + 200u < s_bus_dv)) {
        ESP_LOGI(TAG, "HV bus %u V (0x1DA)", (unsigned)(in->bus_dv / 10u));
        s_bus_dv = in->bus_dv;
        s_bus_ms = 0;
    }
    if ((int)in->lear_alive != s_lear) {
        ESP_LOGI(TAG, "Lear %s", in->lear_alive ? "broadcasting" : "silent");
        s_lear = in->lear_alive;
    }
    if (in->lear_alive && in->dc_fresh && s_lear_ms >= 10000u) {
        ESP_LOGI(TAG, "Lear DC %u.%u V %u.%u A", (unsigned)(in->dc_dv / 10u), (unsigned)(in->dc_dv % 10u),
                 (unsigned)(in->dc_da / 10u), (unsigned)(in->dc_da % 10u));
        s_lear_ms = 0;
    }
    if (g_sm_rx.cnt_616 != 0u) {
        uint32_t e = ((uint32_t)g_sm_rx.lear_echo_en << 31) | ((uint32_t)g_sm_rx.lear_echo_tv_dv << 16) |
                     g_sm_rx.lear_echo_da;
        if (e != s_echo) {
            ESP_LOGI(TAG, "Lear 0x616 echo: enable=%d target %u.%u V AC limit %u.%u A", (int)g_sm_rx.lear_echo_en,
                     (unsigned)(g_sm_rx.lear_echo_tv_dv / 10u), (unsigned)(g_sm_rx.lear_echo_tv_dv % 10u),
                     (unsigned)(g_sm_rx.lear_echo_da / 10u), (unsigned)(g_sm_rx.lear_echo_da % 10u));
            s_echo = e;
        }
    }
    if (g_sm_rx.cnt_1db != 0u) {
        uint16_t lbc = (uint16_t)(g_sm_rx.lbc_full | (g_sm_rx.lbc_main_relay << 1) | (g_sm_rx.lbc_failsafe << 2) |
                                  (g_sm_rx.lbc_relay_cut << 5) | (g_sm_rx.lbc_chg_status << 7));
        if (lbc != s_lbc) {
            sm_trace_lbc();
            s_lbc = lbc;
        }
    }
}

/* Charge-start window: LBC / Lear / VCM every SM_TRACE_STEP_MS after the charge contactors close */
static uint32_t s_win_ms   = MS_MAX;
static uint32_t s_win_next = 0;

static void sm_trace_window_start(void)
{
    if (SM_LOG_VCM_TRACE) {
        s_win_ms   = 0;
        s_win_next = 0;
        sm_trace_lbc();
    }
}

static void sm_trace_window(const sm_in_t *in, uint32_t dt)
{
    if (s_win_ms >= SM_TRACE_WINDOW_MS) {
        return;
    }
    if (s_win_ms >= s_win_next) {
        int      lbc_da = g_sm_rx.lbc_current_ha * 5;   /* 0.1 A, + = discharging */
        unsigned lbc_ab = (unsigned)(lbc_da < 0 ? -lbc_da : lbc_da);
        uint16_t out    = obc380_get_output_power(frame_380_data);
        unsigned pil_da = (unsigned)(in->pilot_a * 10.0f);
        ESP_LOGI(TAG, "chg +%u ms: LBC %s%u.%u A | Lear %u.%u V %u.%u A (%u ms old) 12V %u.%u AC %u.%u %dC | "
                      "pilot %u.%u A | reported %u.%u kW %u A | VCM d0=%02X mode=%u cmd %u W",
                 (unsigned)s_win_ms, lbc_da < 0 ? "-" : "+", lbc_ab / 10u, lbc_ab % 10u,
                 (unsigned)(in->dc_dv / 10u), (unsigned)(in->dc_dv % 10u),
                 (unsigned)(in->dc_da / 10u), (unsigned)(in->dc_da % 10u),
                 (unsigned)(s_age_617.age_ms > 9999u ? 9999u : s_age_617.age_ms),
                 (unsigned)(g_sm_rx.lear_aux_dv / 10u), (unsigned)(g_sm_rx.lear_aux_dv % 10u),
                 (unsigned)(g_sm_rx.lear_ac_raw / 10u), (unsigned)(g_sm_rx.lear_ac_raw % 10u),
                 (int)g_sm_rx.lear_temp_raw - 40,
                 pil_da / 10u, pil_da % 10u,
                 (unsigned)(out / 10u), (unsigned)(out % 10u), (unsigned)(frame_5bf_data[6] >> 3),
                 (unsigned)g_sm_rx.d0_1f2, (unsigned)g_sm_rx.mode_1f2, (unsigned)in->power_cmd_w);
        s_win_next += SM_TRACE_STEP_MS;
    }
    sat_add(&s_win_ms, dt);
}

/* ── Frame helpers for the auth path ─────────────────────────────────────── */

/* Chargeable power advertised on 0x380: OBC maximum, limited by the pilot once latched */
static uint16_t sm_auth_chargeable_raw(void)
{
    uint32_t w = SM_MAX_CHARGE_POWER_W;
    if (s_auth.amps > 0.0f) {
        uint32_t pilot_w = (uint32_t)(s_auth.amps * (float)SM_EVSE_AC_NOMINAL_V);
        if (pilot_w < w) {
            w = pilot_w;
        }
    }
    return (uint16_t)(w / 100u);
}

/* Lear AC current limit (0.1 A): pilot, OBC maximum, and the VCM power command once seen */
static uint16_t sm_auth_lear_limit_da(const sm_in_t *in)
{
    float amps = s_auth.amps;
    float cap  = (float)SM_MAX_CHARGE_POWER_W / (float)SM_EVSE_AC_NOMINAL_V;
    if (amps > cap) {
        amps = cap;
    }
    if (SM_FOLLOW_VCM_POWER_CMD && s_auth.cmd_seen) {
        float cmd_a = (float)in->power_cmd_w / (float)SM_EVSE_AC_NOMINAL_V;
        if (amps > cmd_a) {
            amps = cmd_a;
        }
    }
    return (uint16_t)(amps * 10.0f);
}

static void sm_auth_set_lear_limit(uint16_t da)
{
    if (da == s_auth.lear_limit_da) {
        return;
    }
    portENTER_CRITICAL(&g_frame050_lock);
    lear050_set_ac_limit(frame_050_data, da);
    portEXIT_CRITICAL(&g_frame050_lock);
    s_auth.lear_limit_da = da;
}

static void sm_auth_lear_enable(bool on)
{
    if (!on) {
        twai1_frame_050_set_enable(false);
        return;
    }
    portENTER_CRITICAL(&g_frame050_lock);
    lear050_set_enable(frame_050_data, true);
    portEXIT_CRITICAL(&g_frame050_lock);
}

/* AC is present: command the Lear off (limit 0) until the VCM asks for power, so it is
 * known to be disabled before any charge contactor closes */
static void sm_auth_lear_hold_off(void)
{
    portENTER_CRITICAL(&g_frame050_lock);
    frame_050_data[0] = 0x00u;
    frame_050_data[1] = 0xDCu;
    frame_050_data[4] = 0x00u;
    lear050_set_ac_limit(frame_050_data, 0);
    portEXIT_CRITICAL(&g_frame050_lock);
    sm_auth_lear_enable(false);
    sm_encode_frame_050_voltage();
    s_auth.lear_limit_da = 0;
    vTaskResume(twai1_tx_task_hdl);
}

/* 0x380 d[2]: EVSE ready while announced and the button is not pressed; release switch bit */
static void sm_auth_sync_switch_bits(const sm_in_t *in)
{
    uint8_t d2 = frame_380_data[2];
    obc_set_bits(&d2, OBC380_D2_EVSE_READY, s_auth.ready && in->zone == PLUG_IN);
    obc_set_bits(&d2, OBC380_D2_RELEASE_SW, in->zone == PLUG_BUTTON || s_auth.release_emul);
    frame_380_data[2] = d2;
}

/* AC is at the Lear once it broadcasts after State C.  A Lear already broadcasting at State C is
 * left over from the last session and drops off TWAI1 while AC returns, so 0x050 sent then is never
 * acknowledged: only its frames from SM_LEAR_AC_SETTLE_MS after State C count. */
static bool sm_auth_lear_has_ac(const sm_in_t *in)
{
    if (!in->lear_alive) {
        return false;
    }
    if (!s_auth.lear_stale) {
        return true;
    }
    return s_auth.ac_wait_ms >= SM_LEAR_AC_SETTLE_MS &&
           s_age_lear.age_ms < s_auth.ac_wait_ms - SM_LEAR_AC_SETTLE_MS;
}

/* AC confirmed this State C (Lear held off) and the Lear still broadcasting */
static bool sm_auth_ac_present(const sm_in_t *in)
{
    return s_auth.ac_reported && in->lear_alive;
}

/* AC voltage / type are reported once AC is confirmed at the Lear after State C; cleared 400 ms after release */
static void sm_auth_ac_monitor(const sm_in_t *in, uint32_t dt)
{
    if (s_auth.s2_closed) {
        if (!s_auth.ac_reported) {
            sat_add(&s_auth.ac_wait_ms, dt);
        }
        if (!s_auth.ac_reported && sm_auth_lear_has_ac(in)) {
            FRAMES_LOCK();
            obc380_set_ac_raw(frame_380_data, obc380_ac_raw_from_volts(SM_EVSE_AC_NOMINAL_V));
            frame_5bf_data[1] = OBC5BF_D1_AC_PRESENT;
            FRAMES_UNLOCK();
            ESP_LOGI(TAG, "SM: AC at the Lear %u ms after State C", (unsigned)s_auth.ac_wait_ms);
            s_auth.ac_reported = true;
            s_auth.ac_ms       = 0;
            if (!s_auth.pos_closed) {
                sm_auth_lear_hold_off();
            }
        } else if (s_auth.ac_reported && !s_auth.ac_type_set) {
            sat_add(&s_auth.ac_ms, dt);
            if (s_auth.ac_ms >= 200u) {
                frame_380_data[0] |= (uint8_t)SM_AC_TYPE_MASK;
                s_auth.ac_type_set = true;
            }
        }
    } else if (s_auth.ac_reported) {
        sat_add(&s_auth.s2_open_ms, dt);
        if (s_auth.s2_open_ms >= 400u) {
            FRAMES_LOCK();
            obc380_set_ac_raw(frame_380_data, 0);
            frame_380_data[0] &= (uint8_t)~(OBC380_D0_AC_100V | OBC380_D0_AC_200V);
            frame_5bf_data[1]  = 0x00u;
            FRAMES_UNLOCK();
            s_auth.ac_reported = false;
            s_auth.ac_type_set = false;
        }
    }
}

/* Output power, DC volts and amps from the Lear (0x617) and the HV bus (0x1DA) */
static void sm_auth_telemetry(const sm_in_t *in, bool charging)
{
    uint16_t out_raw = 0, amps = 0, volts = 0;
    if (charging && in->dc_fresh) {
        out_raw = (uint16_t)(((uint32_t)in->dc_dv * in->dc_da) / 10000u);   /* 0.1 V x 0.1 A → 0.1 kW */
        amps    = (uint16_t)(in->dc_da / 10u);
    }
    if (s_auth.contactors) {
        if (in->bus_v_fresh) {
            volts = (uint16_t)(in->bus_dv / 10u);
        } else if (in->dc_fresh) {
            volts = (uint16_t)(in->dc_dv / 10u);
        }
    }
    FRAMES_LOCK();
    obc380_set_output_power(frame_380_data, out_raw);
    obc5bf_set_dc_volts(frame_5bf_data, volts);
    obc5bf_set_dc_amps(frame_5bf_data, amps);
    FRAMES_UNLOCK();
}

/* Release State C and clear the EVSE announcement.  AC is about to go, so stop 0x050 too;
 * never called while the Lear is enabled. */
static void sm_auth_withdraw(void)
{
    s_auth.ready = false;
    vTaskSuspend(twai1_tx_task_hdl);
    gpio_set_level(GPIO_CP_ENABLE, 0);
    if (s_auth.s2_closed) {
        s_auth.s2_closed  = false;
        s_auth.s2_open_ms = 0;
    }
    FRAMES_LOCK();
    frame_380_data[2] &= (uint8_t)~OBC380_D2_EVSE_READY;
    frame_380_data[4] &= (uint8_t)~OBC380_D4_S2;
    FRAMES_UNLOCK();
}

static void sm_auth_ack_stop(void)
{
    FRAMES_LOCK();
    frame_380_data[0] |= OBC380_D0_BASE | OBC380_D0_STOP_ACK;
    obc380_set_mode(frame_380_data, OBC380_D2_MODE_STANDBY);
    frame_380_data[7] &= (uint8_t)~OBC380_D7_HV_OUTPUT;
    frame_5bf_data[4]  = OBC5BF_ST_STOP_ACK;
    FRAMES_UNLOCK();
    s_auth.acked = true;
}

/* Everything the auth path may drive, back to idle */
static void sm_auth_release_outputs(void)
{
    sm_eva_stop();
    sm_auth_lear_enable(false);
    vTaskSuspend(twai1_tx_task_hdl);
    gpio_set_level(GPIO_CP_ENABLE, 0);
    gpio_set_level(GPIO_CONT_POS,  0);
    gpio_set_level(GPIO_CONT_NEG,  0);
    s_auth.s2_closed  = false;
    s_auth.contactors = false;
    s_auth.pos_closed = false;
}

static sm_state_t sm_auth_to_sleep(void)
{
    sm_auth_release_outputs();
    gpio_set_level(GPIO_STATUS_LED, 0);
    twai0_frames_reset();
    return SM_STATE_SLEEP;
}

/* Power switch ON before the VCM authorised: continue on the bypass path */
static sm_state_t sm_auth_to_bypass(plug_zone_t zone)
{
    sm_auth_release_outputs();
    twai0_frames_reset();
    if (zone != PLUG_NONE) {
        idle_plug_ticks      = 0;
        idle_plug_status_adv = false;
        i_max_A              = 0.0f;
        sm_enter_idle_plug();
        return SM_STATE_IDLE_PLUG;
    }
    sm_enter_idle_no_plug();
    return SM_STATE_IDLE_NO_PLUG;
}

/* ── Auth-path state entry ───────────────────────────────────────────────── */

/* Appear on EV-CAN as the OBC: 0x380 normal mode, 0x5BF 0x10 (0x18 after 100 ms) */
static void sm_enter_vcm_auth(bool cold)
{
    memset(&s_auth, 0, sizeof s_auth);
    gpio_set_level(GPIO_CP_ENABLE,  0);
    gpio_set_level(GPIO_STATUS_LED, 1);

    FRAMES_LOCK();
    frame_380_data[0] = cold ? 0x00u : OBC380_D0_BASE;   /* stock OBC: first frame after power-up has d[0] = 0x00 */
    frame_380_data[1] = OBC380_D1_FIXED;
    frame_380_data[2] = OBC380_D2_MODE_NORMAL;
    obc380_set_chargeable(frame_380_data, sm_auth_chargeable_raw());
    frame_380_data[4] = 0x00u;
    frame_380_data[5] = 0x00u;
    frame_380_data[6] = 0x00u;
    frame_380_data[7] = 0x00u;
    for (int i = 0; i < 8; i++) {
        frame_5bf_data[i] = 0x00u;
    }
    frame_5bf_data[4] = OBC5BF_ST_START1;
    FRAMES_UNLOCK();
}

static void sm_enter_vcm_standby(bool stop_requested)
{
    sm_auth_withdraw();
    s_auth.acked    = false;
    s_auth.saw_stop = stop_requested;
    if (stop_requested) {
        sm_auth_ack_stop();
    }
}

static void sm_enter_vcm_shutdown(void)
{
    sm_auth_withdraw();
    FRAMES_LOCK();
    frame_380_data[0] &= (uint8_t)~OBC380_D0_STOP_ACK;
    frame_380_data[7] &= (uint8_t)~OBC380_D7_HV_OUTPUT;
    frame_5bf_data[4]  = OBC5BF_ST_ASLEEP;
    FRAMES_UNLOCK();
}

static void sm_enter_auth_start(void)
{
    s_auth.cmd_seen     = false;
    s_auth.session_seen = false;
    FRAMES_LOCK();
    frame_5bf_data[4] = OBC5BF_ST_CHG_REQ;
    obc380_set_chargeable(frame_380_data, 0);
    FRAMES_UNLOCK();
}

static void sm_enter_auth_charging(const sm_in_t *in)
{
    uint16_t da = sm_auth_lear_limit_da(in);

    portENTER_CRITICAL(&g_frame050_lock);
    frame_050_data[0] = 0x00u;
    frame_050_data[1] = 0xDCu;
    frame_050_data[4] = 0x00u;
    lear050_set_ac_limit(frame_050_data, da);
    lear050_set_enable(frame_050_data, true);
    portEXIT_CRITICAL(&g_frame050_lock);
    sm_encode_frame_050_voltage();
    s_auth.lear_limit_da = da;

    frame_5bf_data[4] = OBC5BF_ST_CHARGING;
    s_wake_attempts   = 0;

    /* Resume TWAI1 TX — immediately preempts this task (higher priority) */
    vTaskResume(twai1_tx_task_hdl);
    gpio_set_level(GPIO_STATUS_LED, 1);
    ESP_LOGI(TAG, "SM: Lear enabled, AC limit %u.%u A", (unsigned)(da / 10u), (unsigned)(da % 10u));
}

/* `done` stops further bridge-initiated wakes until the plug is removed */
static sm_state_t sm_auth_begin_stop(sm_state_t from, stop_kind_t kind, bool done, const char *why)
{
    s_auth.release_emul   = (kind == STOP_SOC);   /* no physical button: emulate a short press */
    s_auth.fast           = false;
    s_auth.ready          = false;
    s_auth.cnt617_at_stop = g_sm_rx.cnt_617;
    sm_auth_lear_enable(false);

    FRAMES_LOCK();
    obc380_set_chargeable(frame_380_data, 0);
    obc380_set_output_power(frame_380_data, 0);
    frame_380_data[2] &= (uint8_t)~OBC380_D2_EVSE_READY;
    obc5bf_set_dc_amps(frame_5bf_data, 0);
    frame_5bf_data[4] = (kind == STOP_VCM) ? OBC5BF_ST_END : OBC5BF_ST_OBC_STOP;
    FRAMES_UNLOCK();

    if (done) {
        s_session_done = true;
    }
    if (kind == STOP_SOC) {
        s_soc_stopped = true;
    }
    if (kind == STOP_FAULT) {
        ESP_LOGW(TAG, "SM: charge stopped: %s", why);
        if (++s_fault_stops >= SM_VCM_WAKE_ATTEMPTS) {
            s_session_done = true;
        }
    }
    return sm_go(from, SM_STATE_AUTH_STOP, why);
}

/* Below the SOC target; after a SOC-target stop, below target minus the restart hysteresis */
static bool sm_soc_allows_charge(const sm_in_t *in)
{
    if (!in->soc_valid) {
        return true;
    }
    float limit = in->target_soc - (s_soc_stopped ? SM_SOC_RESTART_HYST : 0.0f);
    return in->soc_pct < limit;
}

/* Runs every cycle in the auth build */
static void sm_plug_session_tick(const sm_in_t *in)
{
    if (in->zone == PLUG_NONE) {
        s_wake_attempts  = 0;
        s_fault_stops    = 0;
        s_session_done   = false;
        s_soc_stopped    = false;
        s_gave_up_logged = false;
    }
}

/* ── Auth-path states ────────────────────────────────────────────────────── */

/* SLEEP, auth build: answer a VCM self-wake, or wake the VCM for a plug-in.
 * Self-wakes (charge timer, immediate-charge switch) are always answered. */
static sm_state_t sm_sleep_auth(const sm_in_t *in)
{
    if (in->wake_edge && in->zone != PLUG_NONE) {
        if (in->beacon_recent) {
            ESP_LOGI(TAG, "SM: power-switch wake with plug in — waiting for 0x50B power ON");
            s_btn_wake    = true;
            s_btn_wake_ms = 0;
            return SM_STATE_SLEEP;
        }
        sm_enter_vcm_auth(true);
        return sm_go(SM_STATE_SLEEP, SM_STATE_VCM_AUTH, "VCM woke with plug in: timer start or charge switch");
    }
    if (s_btn_wake) {
        if (!in->bus_alive || in->zone == PLUG_NONE) {
            s_btn_wake = false;
        } else if (s_btn_wake_ms >= SM_POWER_ON_WAIT_MS) {
            ESP_LOGW(TAG, "SM: no power ON %u ms after a 0x603/0x68C wake", (unsigned)SM_POWER_ON_WAIT_MS);
            sm_enter_vcm_auth(true);
            return sm_go(SM_STATE_SLEEP, SM_STATE_VCM_AUTH, "treating it as a charge wake");
        }
        return SM_STATE_SLEEP;
    }

    if (in->bus_alive || in->zone != PLUG_IN || s_pilot_ok_ms < SM_PILOT_STABLE_MS ||
        s_quiet_ms < SM_REWAKE_DELAY_MS) {
        return SM_STATE_SLEEP;
    }
    bool soc_ok = sm_soc_allows_charge(in);
    if (s_session_done || !soc_ok || s_wake_attempts >= SM_VCM_WAKE_ATTEMPTS) {
        if (!s_gave_up_logged) {
            ESP_LOGW(TAG, "SM: not waking the VCM until the plug is removed (%s)",
                     s_session_done ? "session finished" : !soc_ok ? "SOC at target" : "wake attempts used");
            s_gave_up_logged = true;
        }
        return SM_STATE_SLEEP;
    }

    s_wake_attempts++;
    sm_eva_start();
    ESP_LOGI(TAG, "SM: SLEEP -> VCM_WAKE (activation pulse, attempt %u/%u, pilot %.1f A)",
             (unsigned)s_wake_attempts, (unsigned)SM_VCM_WAKE_ATTEMPTS, in->pilot_a);
    return SM_STATE_VCM_WAKE;
}

static sm_state_t sm_vcm_wake(const sm_in_t *in)
{
    const sm_state_t me = SM_STATE_VCM_WAKE;
    if (in->power_on) {
        return sm_go(me, sm_auth_to_bypass(in->zone), "power switch ON");
    }
    if (in->awake && in->beacon_recent) {
        sm_state_t next = sm_auth_to_sleep();
        s_btn_wake    = true;
        s_btn_wake_ms = 0;
        return sm_go(me, next, "power-switch wake (0x603/0x68C) — waiting for 0x50B power ON");
    }
    if (in->awake) {
        sm_enter_vcm_auth(true);
        return sm_go(me, SM_STATE_VCM_AUTH, "VCM awake");
    }
    if (in->zone != PLUG_IN) {
        return sm_go(me, sm_auth_to_sleep(), "plug removed or button pressed");
    }
    if (s_auth.st_ms >= SM_VCM_WAKE_TIMEOUT_MS) {
        ESP_LOGW(TAG, "SM: VCM did not wake (attempt %u/%u)",
                 (unsigned)s_wake_attempts, (unsigned)SM_VCM_WAKE_ATTEMPTS);
        s_quiet_ms = 0;   /* re-arm SM_REWAKE_DELAY_MS before the next attempt */
        return sm_go(me, sm_auth_to_sleep(), "no 0x50B WakeUp");
    }
    return me;
}

/* EVSE ready, then State C + S2 with the pilot latched just before, then the pilot amps.
 * Nothing is announced while the SOC is at the target: the VCM then times out and stops. */
static void sm_auth_announce(const sm_in_t *in, uint32_t dt)
{
    if (!s_auth.s2_closed) {
        bool soc_ok = sm_soc_allows_charge(in);
        if (!soc_ok && !s_auth.soc_logged) {
            ESP_LOGI(TAG, "SM: SOC %.1f%% at target — EVSE not announced", in->soc_pct);
            s_auth.soc_logged = true;
        }
        if (in->zone == PLUG_IN && in->pilot_a > 0.0f && soc_ok) {
            sat_add(&s_auth.ann_ms, dt);
        } else {
            s_auth.ann_ms = 0;
            s_auth.ready  = false;
        }
        if (s_auth.ann_ms >= SM_ANNOUNCE_READY_MS) {
            s_auth.ready = true;
        }
        if (s_auth.ann_ms >= SM_ANNOUNCE_S2_MS) {
            /* Last pilot reading: the CP circuit is unreliable once State C is asserted */
            s_auth.amps = in->pilot_a;
            gpio_set_level(GPIO_CP_ENABLE, 1);
            FRAMES_LOCK();
            frame_380_data[4] |= OBC380_D4_S2;
            obc380_set_chargeable(frame_380_data, sm_auth_chargeable_raw());
            FRAMES_UNLOCK();
            s_auth.s2_closed  = true;
            s_auth.s2_ms      = 0;
            s_auth.lear_stale = in->lear_alive;
            s_auth.ac_wait_ms = 0;
            ESP_LOGI(TAG, "SM: EVSE announced, State C asserted (pilot %.1f A)", s_auth.amps);
            if (s_auth.lear_stale) {
                ESP_LOGI(TAG, "SM: Lear still broadcasting from the last session — AC counts from +%u ms",
                         (unsigned)SM_LEAR_AC_SETTLE_MS);
            }
        }
    } else if (!s_auth.pilot_set) {
        sat_add(&s_auth.s2_ms, dt);
        if (s_auth.s2_ms >= (SM_ANNOUNCE_PILOT_MS - SM_ANNOUNCE_S2_MS)) {
            frame_5bf_data[2] = (uint8_t)(s_auth.amps * 5.0f);
            s_auth.pilot_set  = true;
        }
    }
}

static sm_state_t sm_vcm_auth(const sm_in_t *in, uint32_t dt)
{
    const sm_state_t me = SM_STATE_VCM_AUTH;
    if (!in->bus_alive) {
        return sm_go(me, sm_auth_to_sleep(), "EV-CAN silent");
    }
    if (in->gotosleep) {
        sm_enter_vcm_shutdown();
        return sm_go(me, SM_STATE_VCM_SHUTDOWN, "GoToSleep");
    }
    if (in->power_on) {
        return sm_go(me, sm_auth_to_bypass(in->zone), "power switch ON before authorisation");
    }
    if (in->req == VCM1F2_REQ_STOP) {
        sm_enter_vcm_standby(true);
        return sm_go(me, SM_STATE_VCM_STANDBY, "stop request: charge timer pending or not accepted");
    }
    bool soc_ok = sm_soc_allows_charge(in);
    if (in->req == VCM1F2_REQ_NORMAL && s_auth.pilot_set && in->zone == PLUG_IN && soc_ok) {
        sm_enter_auth_start();
        return sm_go(me, SM_STATE_AUTH_START, "normal charge request");
    }
    if (in->zone == PLUG_NONE) {
        sm_enter_vcm_standby(false);
        return sm_go(me, SM_STATE_VCM_STANDBY, "plug removed");
    }
    if (s_auth.st_ms >= SM_CHARGE_REQ_TIMEOUT_MS) {
        ESP_LOGW(TAG, "SM: no charge request from the VCM in %u ms", (unsigned)SM_CHARGE_REQ_TIMEOUT_MS);
        sm_enter_vcm_standby(false);
        return sm_go(me, SM_STATE_VCM_STANDBY, "request timeout");
    }

    /* 0x5BF 0x10 for one frame period, then 0x18 */
    if (!s_auth.appeared && s_auth.st_ms >= 100u) {
        FRAMES_LOCK();
        frame_380_data[0] |= OBC380_D0_BASE;
        frame_5bf_data[4]  = OBC5BF_ST_START2;
        FRAMES_UNLOCK();
        s_auth.appeared = true;
    }
    /* The SOC arrives a few hundred ms after a wake, possibly after State C was asserted */
    if (s_auth.s2_closed && !soc_ok) {
        ESP_LOGI(TAG, "SM: SOC %.1f%% at target — EVSE announcement withdrawn", in->soc_pct);
        s_auth.soc_logged = true;
        sm_auth_withdraw();
    }
    sm_auth_announce(in, dt);
    sm_auth_sync_switch_bits(in);
    sm_auth_ac_monitor(in, dt);
    return me;
}

static sm_state_t sm_vcm_standby(const sm_in_t *in, uint32_t dt)
{
    const sm_state_t me = SM_STATE_VCM_STANDBY;
    if (!in->bus_alive) {
        return sm_go(me, sm_auth_to_sleep(), "EV-CAN silent");
    }
    if (in->gotosleep) {
        sm_enter_vcm_shutdown();
        return sm_go(me, SM_STATE_VCM_SHUTDOWN, "GoToSleep");
    }
    if (in->power_on) {
        return sm_go(me, sm_auth_to_bypass(in->zone), "power switch ON");
    }

    if (in->req == VCM1F2_REQ_STOP) {
        if (!s_auth.acked) {
            sm_auth_ack_stop();
        }
        s_auth.saw_stop = true;
    } else if (s_auth.saw_stop && in->f1f2_fresh && in->zone == PLUG_IN) {
        /* The VCM holds its stop request until GoToSleep, so a withdrawal is a restart
         * (immediate-charge switch or timer start) */
        sm_enter_vcm_auth(false);
        return sm_go(me, SM_STATE_VCM_AUTH, "stop request withdrawn: VCM restarted");
    } else if (!s_auth.acked && s_auth.st_ms >= SM_STOP_ACK_TIMEOUT_MS) {
        ESP_LOGW(TAG, "SM: no stop request from the VCM — acknowledging anyway");
        sm_auth_ack_stop();
    }
    sm_auth_sync_switch_bits(in);
    sm_auth_ac_monitor(in, dt);
    return me;
}

static sm_state_t sm_auth_start(const sm_in_t *in, uint32_t dt)
{
    const sm_state_t me = SM_STATE_AUTH_START;
    if (in->power_cmd_w > 0u) s_auth.cmd_seen = true;
    if (in->session)          s_auth.session_seen = true;

    if (!in->bus_alive || in->gotosleep) {
        return sm_auth_begin_stop(me, STOP_FAULT, false, "VCM went to sleep");
    }
    if (in->req == VCM1F2_REQ_STOP) {
        return sm_auth_begin_stop(me, STOP_VCM, false, "stop request");
    }
    if (in->zone != PLUG_IN) {
        return sm_auth_begin_stop(me, STOP_RELEASE, true, "connector button or unplug");
    }
    if (in->lbc_fail) {
        return sm_auth_begin_stop(me, STOP_FAULT, false, "LBC failsafe");
    }
    if (s_auth.sub == 0u) {
        if (s_auth.st_ms >= SM_HV_LIVE_TIMEOUT_MS && !(in->hv_live && in->relay_plus)) {
            return sm_auth_begin_stop(me, STOP_FAULT, false, "HV bus not live (0x1D4)");
        }
        if (s_auth.st_ms >= SM_LEAR_AC_TIMEOUT_MS && !sm_auth_ac_present(in)) {
            return sm_auth_begin_stop(me, STOP_FAULT, false, "no AC at the Lear");
        }
    } else if (s_auth.contactors && in->d4_fresh && !in->hv_live) {
        return sm_auth_begin_stop(me, STOP_FAULT, false, "HV bus lost");
    }

    switch (s_auth.sub) {
    case 0:     /* VCM main relays closed and HV bus live; AC at the Lear and the Lear held off */
        if (in->hv_live && in->relay_plus && sm_auth_ac_present(in)) {
            sm_auth_sub(1);
        }
        break;
    case 1:     /* let the bus settle, then close the return side first */
        if (s_auth.sub_ms >= SM_HV_SETTLE_MS) {
            gpio_set_level(GPIO_CONT_NEG, 1);
            s_auth.contactors = true;   /* any contactor closed: a stop must run the open sequence */
            sm_auth_sub(2);
        }
        break;
    case 2:
        if (s_auth.sub_ms >= SM_CONTACTOR_STEP_MS) {
            gpio_set_level(GPIO_CONT_POS, 1);
            s_auth.pos_closed = true;
            FRAMES_LOCK();
            frame_380_data[4] |= OBC380_D4_NORMAL_RELAY;
            frame_5bf_data[4]  = OBC5BF_ST_RELAY_CLOSED;
            FRAMES_UNLOCK();
            ESP_LOGI(TAG, "SM: charge contactors closed (HV bus %u V)", (unsigned)(in->bus_dv / 10u));
            sm_trace_window_start();
            sm_auth_sub(3);
        }
        break;
    case 3:
        if (s_auth.sub_ms >= SM_AUTH_STEP_MS) {
            FRAMES_LOCK();
            frame_5bf_data[4]  = OBC5BF_ST_PREP1;
            frame_380_data[7] |= OBC380_D7_HV_OUTPUT;
            FRAMES_UNLOCK();
            sm_auth_sub(4);
        }
        break;
    case 4:
        if (s_auth.sub_ms >= SM_AUTH_STEP_MS) {
            frame_5bf_data[4] = OBC5BF_ST_PREP2;
            sm_auth_sub(5);
        }
        break;
    case 5:     /* re-advertising the chargeable power prompts the VCM power command */
        if (s_auth.sub_ms >= SM_AUTH_STEP_MS) {
            FRAMES_LOCK();
            obc380_set_chargeable(frame_380_data, sm_auth_chargeable_raw());
            FRAMES_UNLOCK();
            sm_auth_sub(6);
        }
        break;
    default:    /* 6: wait for the VCM maximum-charge-power command */
        if (s_auth.cmd_seen || !SM_FOLLOW_VCM_POWER_CMD) {
            sm_enter_auth_charging(in);
            return sm_go(me, SM_STATE_AUTH_CHARGING, s_auth.cmd_seen ? "VCM power command" : "power command ignored");
        }
        if (s_auth.sub_ms >= SM_POWER_CMD_TIMEOUT_MS) {
            ESP_LOGW(TAG, "SM: no VCM power command (0x1F2 d[1]) — charging on the pilot limit");
            sm_enter_auth_charging(in);
            return sm_go(me, SM_STATE_AUTH_CHARGING, "power command timeout");
        }
        break;
    }
    sm_auth_sync_switch_bits(in);
    sm_auth_ac_monitor(in, dt);
    sm_auth_telemetry(in, false);
    return me;
}

static sm_state_t sm_auth_charging(const sm_in_t *in, uint32_t dt)
{
    const sm_state_t me = SM_STATE_AUTH_CHARGING;
    if (in->power_cmd_w > 0u) s_auth.cmd_seen = true;

    if (!in->bus_alive || in->gotosleep) {
        return sm_auth_begin_stop(me, STOP_FAULT, false, "VCM went to sleep");
    }
    if (!in->f1f2_fresh) {
        return sm_auth_begin_stop(me, STOP_FAULT, false, "0x1F2 lost");
    }
    if (in->req == VCM1F2_REQ_STOP) {
        return sm_auth_begin_stop(me, STOP_VCM, true, "stop request");
    }
    if (SM_FOLLOW_VCM_POWER_CMD && s_auth.cmd_seen && in->power_cmd_w == 0u) {
        return sm_auth_begin_stop(me, STOP_VCM, true, "VCM power command 0");
    }
    if (s_auth.session_seen && in->d4_fresh && !in->session) {
        return sm_auth_begin_stop(me, STOP_VCM, true, "VCM charge session ended (0x1D4)");
    }
    if (in->zone != PLUG_IN) {
        return sm_auth_begin_stop(me, STOP_RELEASE, true, "connector button or unplug");
    }
    if (in->soc_valid && in->soc_pct >= in->target_soc) {
        return sm_auth_begin_stop(me, STOP_SOC, true, "SOC target");
    }
    if (in->lbc_fail) {
        return sm_auth_begin_stop(me, STOP_FAULT, false, "LBC failsafe");
    }
    if (in->d4_fresh && !in->hv_live) {
        return sm_auth_begin_stop(me, STOP_FAULT, false, "HV bus lost");
    }
    if (!in->lear_alive) {
        return sm_auth_begin_stop(me, STOP_FAULT, false, "Lear silent (AC lost)");
    }

    if (in->session) s_auth.session_seen = true;
    sm_auth_set_lear_limit(sm_auth_lear_limit_da(in));
    if (g_sm_target_voltage_v != s_frame050_voltage_v) {
        sm_encode_frame_050_voltage();
    }
    sm_auth_sync_switch_bits(in);
    sm_auth_ac_monitor(in, dt);
    sm_auth_telemetry(in, true);
    return me;
}

/* Stock order: OBC off → State C released → VCM opens its main relays → charge relay
 * open (0x78) → stop acknowledged (0x30) → GoToSleep */
static sm_state_t sm_auth_stop(const sm_in_t *in, uint32_t dt)
{
    const sm_state_t me = SM_STATE_AUTH_STOP;
    if (!in->bus_alive || in->gotosleep) {
        s_auth.fast = true;
    }
    if (in->req == VCM1F2_REQ_STOP && frame_5bf_data[4] == OBC5BF_ST_OBC_STOP) {
        frame_5bf_data[4] = OBC5BF_ST_END;    /* the VCM followed the OBC stop */
    }

    switch (s_auth.sub) {
    case 0: {   /* Lear ramp-down: TWAI1 keeps sending ChargeEnable = 0 */
        bool low = (g_sm_rx.cnt_617 != s_auth.cnt617_at_stop) && in->dc_da < 10u;
        if (s_auth.sub_ms >= 200u && (low || s_auth.fast || s_auth.sub_ms >= SM_RAMP_DOWN_MS)) {
            vTaskSuspend(twai1_tx_task_hdl);
            sm_auth_withdraw();
            sm_auth_sub(1);
        }
        break;
    }
    case 1:     /* release switch let go; wait for the VCM to open its main relays */
        if (s_auth.release_emul && s_auth.sub_ms >= 50u) {
            s_auth.release_emul = false;
        }
        if (!s_auth.contactors) {
            sm_auth_sub(3);
        } else if (!s_auth.pos_closed) {
            /* stopped between NEG and POS: no current path, open at once */
            gpio_set_level(GPIO_CONT_NEG, 0);
            s_auth.contactors = false;
            ESP_LOGI(TAG, "SM: charge contactors open");
            sm_auth_sub(3);
        } else if (s_auth.fast || !in->d4_fresh || !in->relay_plus || !in->hv_live ||
                   s_auth.sub_ms >= SM_MAIN_RELAY_WAIT_MS) {
            gpio_set_level(GPIO_CONT_POS, 0);
            s_auth.pos_closed = false;
            sm_auth_sub(2);
        }
        break;
    case 2:
        if (s_auth.sub_ms >= SM_CONTACTOR_STEP_MS) {
            gpio_set_level(GPIO_CONT_NEG, 0);
            s_auth.contactors = false;
            FRAMES_LOCK();
            frame_380_data[4] &= (uint8_t)~OBC380_D4_NORMAL_RELAY;
            frame_5bf_data[4]  = OBC5BF_ST_RELAY_OPEN;
            FRAMES_UNLOCK();
            ESP_LOGI(TAG, "SM: charge contactors open");
            sm_auth_sub(3);
        }
        break;
    default:    /* 3: wait for the VCM stop request, GoToSleep or power ON */
        if (!in->bus_alive) {
            return sm_go(me, sm_auth_to_sleep(), "EV-CAN silent");
        }
        if (in->gotosleep) {
            sm_enter_vcm_shutdown();
            return sm_go(me, SM_STATE_VCM_SHUTDOWN, "GoToSleep");
        }
        if (in->power_on) {
            return sm_go(me, sm_auth_to_bypass(in->zone), "power switch ON");
        }
        if (in->req == VCM1F2_REQ_STOP) {
            sm_enter_vcm_standby(true);
            return sm_go(me, SM_STATE_VCM_STANDBY, "stop request acknowledged");
        }
        if (s_auth.sub_ms >= SM_STOP_ACK_TIMEOUT_MS) {
            ESP_LOGW(TAG, "SM: VCM sent no stop request after the charge stop");
            sm_enter_vcm_standby(false);
            sm_auth_ack_stop();
            return sm_go(me, SM_STATE_VCM_STANDBY, "stop acknowledged without request");
        }
        break;
    }
    sm_auth_sync_switch_bits(in);
    sm_auth_ac_monitor(in, dt);
    sm_auth_telemetry(in, false);
    return me;
}

static sm_state_t sm_vcm_shutdown(const sm_in_t *in, uint32_t dt)
{
    const sm_state_t me = SM_STATE_VCM_SHUTDOWN;
    if (in->awake) {
        sm_enter_vcm_standby(true);
        return sm_go(me, SM_STATE_VCM_STANDBY, "VCM woke again");
    }
    if (!in->bus_alive || in->gotosleep_ms >= SM_SLEEP_TAIL_MS) {
        return sm_go(me, sm_auth_to_sleep(), "VCM asleep");
    }
    sm_auth_ac_monitor(in, dt);
    return me;
}

/* ── State Machine Task ─────────────────────────────────────────────────── */

void state_machine_task(void *pvParams)
{
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t prev_tick = last_wake;

    static sm_state_t state = SM_STATE_SLEEP;
    sm_state_t prev_state   = state;

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
        bool lbc_fail   = g_sm_lbc_failsafe;   g_sm_lbc_failsafe   = false;
        bool chg_int    = g_sm_charge_interrupted; g_sm_charge_interrupted = false;
        bool t1_rxd     = g_sm_twai1_rxd;  g_sm_twai1_rxd  = false;
        float soc_pct   = g_sm_soc_pct;
        float target_soc_pct = (float)g_sm_target_soc_pct;

        /* ── 3b. Decoded CAN levels, plug signal, activation pulse, trace ── */
        TickType_t now = xTaskGetTickCount();
        uint32_t   dt  = (uint32_t)(now - prev_tick) * portTICK_PERIOD_MS;
        prev_tick = now;

        float   pilot_a = pwm_valid ? derive_j1772_current(duty_us, period_us) : 0.0f;
        sm_in_t in;
        sm_read_inputs(&in, dt, adc_mv, pilot_a, lbc_fail, target_soc_pct);

        if (in.bus_alive) {
            s_quiet_ms = 0;
        } else {
            sat_add(&s_quiet_ms, dt);
        }
        if (in.zone == PLUG_IN && pilot_a > 0.0f) {
            sat_add(&s_pilot_ok_ms, dt);
        } else {
            s_pilot_ok_ms = 0;
        }
        sm_eva_tick(dt);
        sat_add(&s_auth.st_ms, dt);
        sat_add(&s_auth.sub_ms, dt);
        sat_add(&s_btn_wake_ms, dt);
        if (state != SM_STATE_SLEEP) {
            s_btn_wake = false;
        }
        if (!SM_SKIP_VCM_AUTH) {
            sm_plug_session_tick(&in);
        }

        if (SM_PLUG_SIG_ENABLE) {
            bool plug = adc_mv <= SM_ADC_NO_PLUG_MV;
            if (plug != s_plug_sig) {
                gpio_set_level(GPIO_PLUG_SIG, plug ? 1 : 0);
                s_plug_sig = plug;
            }
        }
        if (SM_LOG_VCM_TRACE) {
            sm_trace(&in, dt);
        }

        /* ── 4. State machine ───────────────────────────────────────────── */
        switch (state) {

        case SM_STATE_SLEEP:
            if (vcm_run) {
                sm_enter_idle_no_plug();
                ESP_LOGI(TAG, "SM: SLEEP -> IDLE_NO_PLUG (VCM_RUN)");
                state = SM_STATE_IDLE_NO_PLUG;
            } else if (!SM_SKIP_VCM_AUTH) {
                state = sm_sleep_auth(&in);
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
            } else if (idle_plug_status_adv && adc_mv < SM_ADC_BUTTON_MV) {
                /* Bypass path (power switch ON) — advance automatically after PLUG_BEEP (100 ms),
                 * no VCM signal needed.  Zone A only: button held (Zone B) keeps us here until released. */
                session_amps = i_max_A;
                gpio_set_level(GPIO_STATUS_LED, 0);   /* IDLE_PLUG exit action */
                sm_enter_evse_enable();
                ESP_LOGI(TAG, "SM: IDLE_PLUG -> EVSE_ENABLE (auth skipped, %.1f A)", session_amps);
                state = SM_STATE_EVSE_ENABLE;
            }
            break;

        /* ── VCM-authorised path (SM_SKIP_VCM_AUTH == 0) ─────────────────── */
        case SM_STATE_VCM_WAKE:       state = sm_vcm_wake(&in);            break;
        case SM_STATE_VCM_AUTH:       state = sm_vcm_auth(&in, dt);        break;
        case SM_STATE_VCM_STANDBY:    state = sm_vcm_standby(&in, dt);     break;
        case SM_STATE_AUTH_START:     state = sm_auth_start(&in, dt);      break;
        case SM_STATE_AUTH_CHARGING:  state = sm_auth_charging(&in, dt);   break;
        case SM_STATE_AUTH_STOP:      state = sm_auth_stop(&in, dt);       break;
        case SM_STATE_VCM_SHUTDOWN:   state = sm_vcm_shutdown(&in, dt);    break;

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

        if (state != prev_state) {
            s_auth.st_ms  = 0;
            s_auth.sub    = 0;
            s_auth.sub_ms = 0;
            prev_state    = state;
        }
        if (SM_LOG_VCM_TRACE) {
            sm_trace_window(&in, dt);
        }

        /* Auth build: the SM owns the TWAI0 TX gate.  As the OBC under VCM control it
         * transmits only while VCM frames keep arriving and for SM_SLEEP_TAIL_MS after
         * GoToSleep (TWAI0 retries an unacknowledged frame forever); elsewhere it follows
         * the power switch exactly as the bypass build's 0x50B handler does. */
        if (!SM_SKIP_VCM_AUTH) {
            g_twai0_tx_enabled = sm_state_is_auth_tx(state)
                               ? (in.vcm_fresh && in.gotosleep_ms < SM_SLEEP_TAIL_MS)
                               : g_sm_rx.power_on;
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
