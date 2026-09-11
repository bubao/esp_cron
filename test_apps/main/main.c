#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}