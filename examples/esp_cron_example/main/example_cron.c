/*
 * ESP-Cron 完整示例
 *
 * 演示内容：
 *   1. SNTP 时间同步 + 时区设置
 *   2. 创建多种周期的定时任务（每秒、每天、工作日）
 *   3. 动态修改任务表达式
 *   4. callback 内安全 destroy 自己（一次性任务）
 *   5. 可选：打印运行统计
 *
 * 运行前确保：
 *   - sdkconfig 中已配置 Wi-Fi SSID/密码
 *   - CONFIG_ESP_CRON_ENABLE=y（默认）
 */

#include "esp_cron.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "cron_example";

/* ------------------------------------------------------------------ */
/*  1. SNTP 等待                                                       */
/* ------------------------------------------------------------------ */

static void wait_for_time_sync(void)
{
    ESP_LOGI(TAG, "Starting SNTP sync...");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    int retry = 0;
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < 30) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }

    if (retry >= 30)
        ESP_LOGW(TAG, "SNTP sync timed out — scheduling will be based on incorrect time");
    else
        ESP_LOGI(TAG, "Time synced");
}

/* SNTP 回调：时间跳变后重建所有 job 的 next_execution */
static void sntp_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP time adjusted, rescheduling all jobs");
    cron_job_reschedule_all();
}

/* ------------------------------------------------------------------ */
/*  2. Callback 示例                                                   */
/* ------------------------------------------------------------------ */

/* 普通周期任务：每秒打印 */
static void every_second_cb(cron_job *job)
{
    int count = (int)(intptr_t)job->data;
    ESP_LOGI(TAG, "[every-second] count=%d", count);
    job->data = (void *)(intptr_t)(count + 1);
}

/* 每天 08:00：泵启动 */
static void morning_pump_cb(cron_job *job)
{
    ESP_LOGI(TAG, "[morning-pump] Turning pump ON");
    /* ... 控制 GPIO / 泵 ... */
}

/* 每天 18:00：泵关闭 */
static void evening_pump_cb(cron_job *job)
{
    ESP_LOGI(TAG, "[evening-pump] Turning pump OFF");
    /* ... 控制 GPIO / 泵 ... */
}

/* 一次性任务：30 秒后自毁 */
static void one_shot_cb(cron_job *job)
{
    ESP_LOGI(TAG, "[one-shot] Fired! Destroying self...");
    cron_job_destroy(job);  /* callback 内 destroy 自己是安全的 */
}

/* 工作日 07:30：打印提醒 */
static void weekday_alert_cb(cron_job *job)
{
    ESP_LOGI(TAG, "[weekday-alert] Good morning!");
}

/* ------------------------------------------------------------------ */
/*  3. main                                                            */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    /* ---- 时区（必须在 cron_job_create 之前） ---- */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* ---- SNTP ---- */
    wait_for_time_sync();
    sntp_set_time_sync_notification_cb(sntp_sync_cb);

    /* ---- 注册任务 ---- */

    /* 每秒（测试用，生产中一般不需要这么频繁） */
    cron_job *sec_job = cron_job_create("* * * * * *", every_second_cb, (void *)(intptr_t)1);

    /* 每天 08:00 */
    cron_job *morning = cron_job_create("0 0 8 * * *", morning_pump_cb, NULL);

    /* 每天 18:00 */
    cron_job *evening = cron_job_create("0 0 18 * * *", evening_pump_cb, NULL);

    /* 工作日 07:30 */
    cron_job *weekday = cron_job_create("0 30 7 * * 1-5", weekday_alert_cb, NULL);

    /* 一次性：30 秒后触发并自毁 */
    cron_job *oneshot = cron_job_create("*/30 * * * * *", one_shot_cb, NULL);

    /* ---- 启动调度器 ---- */
    cron_start();

    ESP_LOGI(TAG, "Scheduler started with %d jobs",
             (sec_job != NULL) + (morning != NULL) + (evening != NULL) +
             (weekday != NULL) + (oneshot != NULL));

    /* ---- 主线程保持运行（可改为做你的业务） ---- */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

#ifdef CONFIG_ESP_CRON_ENABLE_STATS
        /* 每 5 秒打印统计（需开启 CONFIG_ESP_CRON_ENABLE_STATS） */
        time_t next = cron_job_seconds_until_next_execution();
        ESP_LOGI(TAG, "[stats] next in %llds", (long long)next);
#endif
    }
}
