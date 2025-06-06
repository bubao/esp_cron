# ESP-IDF 定时任务（Cron）组件

> English version: See [README.md](./README.md)

这是一个适用于 ESP-IDF 框架的 cron 类定时任务组件，支持 cron 语法和基于 newlib 的 time 库进行任务调度。

## 特性概览

- 支持标准 cron 表达式（秒级精度，6 字段）
- 任务调度基于 FreeRTOS，回调自动在独立任务中运行
- 使用 esp_timer 进行高精度定时调度，低功耗友好
- 支持任务防抖（同一秒只触发一次）
- 动态添加、删除、清空任务
- 任务队列和调度器自动管理
- 适合嵌入式定时场景

## 快速使用

### 1. 初始化时间

本组件依赖 time.h，**在创建任何任务前必须初始化时间**（如 SNTP 或手动 settimeofday）：

```c
struct timeval tv;
time_t begin = 1530000000;
tv.tv_sec = begin; // 2018 年 6 月
settimeofday(&tv, NULL);
```

### 2. 创建任务

```c
cron_job *cron_job_create(const char *schedule, cron_job_callback callback, void *data);
```

- `schedule`：cron 风格字符串，支持秒级精度。

            ┌────────────── second (0 - 59)  
            | ┌───────────── minute (0 - 59)
            | │ ┌───────────── hour (0 - 23)
            | │ │ ┌───────────── day of month (1 - 31)
            | │ │ │ ┌───────────── month (1 - 12)
            | │ │ │ │ ┌───────────── day of week (0 - 6) (Sunday to Saturday;
            | │ │ │ │ │                                       7 is also Sunday on some systems)
            | │ │ │ │ │
            * * * * * *  
            

- `callback`：任务回调函数指针，定义如下：

  ```c
  typedef void (*cron_job_callback)(cron_job *);
  ```

- `data`：用户自定义指针，可在回调中使用。

### 3. 启动调度模块

```c
int cron_start();
```

### 4. 停止调度模块

```c
int cron_stop();
```

### 5. 销毁任务

```c
int cron_job_destroy(cron_job *job);
```

### 6. 清除所有任务

```c
int cron_job_clear_all();
```

## 完整示例

```c
cron_job *jobs[2];
jobs[0] = cron_job_create("* * * * * *", test_cron_job_sample_callback, (void *)0);
jobs[1] = cron_job_create("*/5 * * * * *", test_cron_job_sample_callback, (void *)10000);
cron_start();
vTaskDelay((running_seconds * 1000) / portTICK_PERIOD_MS); // 运行一段时间
cron_stop();
cron_job_clear_all();

void test_cron_job_sample_callback(cron_job *job) {
    // 在这里编写你的任务逻辑
}
```

## 进阶说明

- 任务调度基于 esp_timer，精度高，适合低功耗和高实时性场景。
- 所有回调会在独立 FreeRTOS 任务中运行，避免阻塞主循环。
- 支持任务防抖（last_triggered_sec 字段），同一秒内不会重复触发。
- 支持动态添加、删除、清空任务。
- 任务调度精度为秒级，适合大多数定时场景。
- 任务队列和调度器均自动管理，无需手动干预。
- 支持通过 cron_job_unschedule(job) 移除单个任务。
- 支持 cron_job_seconds_until_next_execution() 查询下次调度剩余秒数。

## 主要 API

- `cron_job *cron_job_create(const char *schedule, cron_job_callback callback, void *data);`
- `int cron_job_destroy(cron_job *job);`
- `int cron_job_clear_all();`
- `int cron_start();`
- `int cron_stop();`
- `int cron_job_schedule(cron_job *job);`
- `int cron_job_unschedule(cron_job *job);`
- `time_t cron_job_seconds_until_next_execution();`
- `int cron_job_load_expression(cron_job *job, const char * schedule);`
- `int cron_job_has_loaded(cron_job *job);`
- `void cron_schedule_task(void *args); // 支持传入 "R1" 只运行一次`

## 代码结构

- `cron.c/cron.h`：核心调度逻辑与 API
- `library/ccronexpr/`：cron 表达式解析器
- `library/jobs/`：任务链表管理

---

特别感谢 [esp_cron](https://github.com/DavidMora/esp_cron) 项目及其作者 David Mora Rodriguez，以及 [ccronexpr](https://github.com/exander77/supertinycron) 相关代码的开源贡献。

感谢 staticlibs.net 的 alex 提供的 cron 解析器 [ccronexpr](https://github.com/staticlibs/ccronexpr)！

> 中文文档：本页为中文说明，English version please see [README.md](./README.md)
