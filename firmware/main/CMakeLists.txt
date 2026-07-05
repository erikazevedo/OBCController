idf_component_register(
    SRCS
        "main.c"
        "twai_handler.c"
        "mcpwm_capture.c"
        "adc_reader.c"
        "state_machine.c"
        "rx_dispatcher.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_twai
        esp_driver_gpio
        esp_driver_mcpwm
        esp_driver_gptimer
        esp_adc
        esp_timer
        freertos
)
