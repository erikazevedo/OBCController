#pragma once
#include <stdint.h>
#include <stdbool.h>

/* PWM measurement results — written from MCPWM ISR, read from state machine task */
extern volatile uint32_t DRAM_ATTR g_pwm_duty_us;
extern volatile uint32_t DRAM_ATTR g_pwm_period_us;
extern volatile bool     DRAM_ATTR g_pwm_signal_valid;

/**
 * Initialise the MCPWM capture timer and channel on GPIO_PWM_CAPTURE.
 * Registers the on_cap ISR and starts the PWM watchdog esp_timer.
 */
void mcpwm_capture_init(void);
