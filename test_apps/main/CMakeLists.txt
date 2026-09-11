idf_component_register(
    SRCS "main.c" "${CMAKE_CURRENT_LIST_DIR}/../../test/test_cron.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../test" "${CMAKE_CURRENT_LIST_DIR}/../../include"
    REQUIRES esp_cron unity
)