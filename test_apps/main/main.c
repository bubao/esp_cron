#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

#define LED_GPIO GPIO_NUM_15

// --wrap=unity_putc / --wrap=unity_flush：把 Unity 输出从 ROM UART0
// 重定向到 printf（VFS → USB-CDC）。LOLIN S2 Mini 无板上 UART。
void __wrap_unity_putc(int c);
void __wrap_unity_flush(void);

void __wrap_unity_putc(int c) {
  if (c == '\n')
    putchar('\r');
  putchar(c);
}

void __wrap_unity_flush(void) { fflush(stdout); }

static void unity_test_task(void *arg) {
  while (1) {
    printf("\n==============================\n");
    printf("Running Unity tests...\n");
    printf("==============================\n");

    UNITY_BEGIN();

    unity_run_all_tests();

    UNITY_END();

    printf("\n==============================\n");
    printf("Unity tests finished.\n");
    printf("Next test in 10 seconds...\n");
    printf("==============================\n");

    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

static void led_flash_task(void *arg) {
  gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
  int level = 0;
  while (1) {
    gpio_set_level(LED_GPIO, level);
    level = !level;
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void app_main(void) {
  xTaskCreate(unity_test_task, "unity_test", 8192, NULL, 5, NULL);
  xTaskCreate(led_flash_task, "led_flash", 2048, NULL, 5, NULL);
}
