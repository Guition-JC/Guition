idf_component_register(
    SRCS
        "serial_terminal.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_uart
)
