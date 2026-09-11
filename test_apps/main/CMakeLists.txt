idf_component_register(
    SRCS "main.c" "${CMAKE_CURRENT_LIST_DIR}/../../test/test_cron.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../test" "${CMAKE_CURRENT_LIST_DIR}/../../include"
    REQUIRES esp_cron unity esp_driver_gpio vfs
    WHOLE_ARCHIVE
)

# Unity 默认输出走 esp_system_console_put_char → ROM UART0；
# LOLIN S2 Mini 无板上 UART，只有原生 USB-CDC，结果会丢。
# 用 --wrap 把 unity_putc / unity_flush 重定向到 printf（VFS→USB-CDC）。
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=unity_putc" "-Wl,--wrap=unity_flush")