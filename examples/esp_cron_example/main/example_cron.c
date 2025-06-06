#include "cron.h"
#include "esp_log.h"
#include <stdio.h>

void cron_callback(cron_job* arg)
{
    char* msg = (char*)arg->data;
    ESP_LOGI("callback", "Cron job triggered! arg=%s", msg);
}

void app_main(void)
{
    static const char* msg = "Hello ESP-CRON";
    // 注册一个每分钟触发的 cron 任务
    cron_job_create("* * * * * *", (cron_job_callback)cron_callback, (void*)msg);
    cron_start();

    printf("esp_cron example started. Waiting for cron job...\n");
    // 让主线程保持运行状态
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // 每秒延时，保持任务运行
    }
}
