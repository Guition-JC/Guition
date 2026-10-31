idf_component_register(
    SRCS
        "src/rx8025.c"
        "src/rx8025_registers.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
)
