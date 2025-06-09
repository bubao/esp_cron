# ESP-IDF 定时任务（Cron）组件

> English version: [README.md](./README.md)

## 组件特性

本组件是 ESP-IDF 框架的类 Cron 任务调度工具，采用类 Cron 语法结合 ESP-IDF 框架的 `esp_timer.h` 实现高精度任务调度，更贴合嵌入式场景需求。

## 使用指南

我们精心设计了极简的接口体系，包含任务创建、销毁、调度器启停等核心功能。标准工作流程为：先定义至少一个任务，再启动调度器，后续可根据需求动态管理任务。特别设计了智能休眠机制：当无待调度任务时，调度器会自动进入低功耗模式，避免无谓的 CPU 资源消耗。

**重要提示**：组件基于 esp_timer 实现定时，无需手动管理系统时间，但需确保在创建任务前已完成必要的定时器初始化配置，以保证调度精度。

### 代码结构

```tree
esp-cron/
├── esp_cron.c            # 核心调度逻辑与 API 实现
├── include/
│   ├── esp_cron.h        # 公共头文件，暴露 API
│   └── cron_internal.h   # 内部头文件，调度器实现
├── library/
│   ├── ccronexpr/       # cron 表达式解析器（开源组件）
│   └── jobs/            # 任务链表管理模块
└── examples/            # 示例代码
```

## 快速使用指南

### 头文件引入

```c
#include "esp_cron.h"
```

### 1. 创建定时任务

```c
cron_job *cron_job_create(const char *schedule, cron_job_callback callback, void *data)
```

- `schedule` 支持秒级精度的类 Cron 表达式，格式说明：
  ```txt
    ┌────────────── 秒（0 - 59）
    | ┌───────────── 分（0 - 59）
    | │ ┌───────────── 时（0 - 23）
    | │ │ ┌───────────── 日（1 - 31）
    | │ │ │ ┌───────────── 月（1 - 12）
    | │ │ │ │ ┌───────────── 周（0 - 6，周日为 0；部分系统中 7 也表示周日）
    | │ │ │ │ │
    * * * * * *
  ```
- `callback` 为任务回调函数指针，参数为当前任务句柄，定义如下：
  ```c
  typedef void (*cron_job_callback)(cron_job *);
  ```
  回调函数设计为轻量级接口，无需自行处理任务调度逻辑，框架会自动管理执行周期。
- `data` 为用户自定义数据指针，可存储任意类型数据，随任务句柄传递至回调函数。

### 任务销毁

如需停止已创建的任务，直接调用销毁函数：

```c
int cron_job_destroy(cron_job *job);
```

### 启动调度器

当至少存在一个可调度任务时，调用启动函数：

```c
int cron_start();
```

### 停止调度器

调用以下函数关闭调度器：

```c
int cron_stop();
```

### 批量清空任务

```c
int cron_job_clear_all();
```

### 应用示例

以下是完整的使用示例，展示了从任务创建到调度的全流程：

```c
/* 初始化 ESP-Timer 环境（非组件必需步骤，依项目需求调整） */
esp_timer_init();

/* 创建两个定时任务 */
cron_job *jobs[2];
jobs[0] = cron_job_create("* * * * * *", on_timer_trigger, (void *)0);
jobs[1] = cron_job_create("*/5 * * * * *", on_timer_trigger, (void *)10000);

/* 启动调度器 */
cron_start();

/* 模拟主程序运行（实际项目中可替换为业务逻辑） */
vTaskDelay(pdMS_TO_TICKS(60000));  // 运行 60 秒后停止

/* 资源释放 */
cron_stop();
cron_job_clear_all();
```

回调函数示例：

```c
void on_timer_trigger(cron_job *job)
{
  /* 处理定时任务逻辑 */
  uint32_t task_id = (uint32_t)job->data;
  printf("Task %u triggered at %lu\n", task_id, esp_timer_get_time() / 1000);
}
```

## 核心 API 列表

### 任务管理

| 函数名                    | 描述                          |
|---------------------------|-------------------------------|
| `cron_job* cron_job_create(const char* schedule, cron_job_callback callback, void* data);`         | 创建并注册定时任务            |
| `int cron_job_destroy(cron_job* job);`        | 销毁指定任务                  |
| `int cron_job_clear_all();`      | 清空所有任务                  |

### 调度控制

| 函数名            | 描述                  |
|-------------------|-----------------------|
| `int cron_start();`      | 启动任务调度器        |
| `int cron_stop();`       | 停止任务调度器        |

### 任务调度操作

| 函数名                  | 描述                          |
|-------------------------|-------------------------------|
| `int cron_job_schedule(cron_job* job);`     | 手动调度指定任务              |
| `int cron_job_unschedule(cron_job* job);`   | 取消指定任务的调度            |
| `void cron_schedule_task(void* args);`    | 特殊调度（如仅运行一次）      |

### 状态查询

| 函数名                          | 描述                          |
|---------------------------------|-------------------------------|
| `int cron_job_load_expression(cron_job* job, const char* schedule);` | 动态加载 cron 表达式        |
| `int cron_job_has_loaded(cron_job* job);`      | 检查任务是否已加载表达式式          |
| `time_t cron_job_seconds_until_next_execution();`           | 获取下次执行剩余秒数      |

## 时间同步与精度优化

### 本地时区设置

若项目需求涉及基于系统时区的时间显示或跨区域协同，可通过以下方式配置本地时区：

1. **时区环境变量配置**
  ```c
  setenv("TZ", "CST-8", 1);  // 北京时间（东八区）
  tzset();  // 应用时区设置
  ```
  该配置会影响 `localtime` 等函数的输出，但 **不影响定时任务的调度精度**——任务调度始终基于硬件定时器的绝对时间，与系统时区无关。
2. **SNTP 时间同步（可选）**
  如需在任务中使用标准时间（如日志时间戳），可通过 SNTP 同步系统时间：
  ```c
  void init_system_time(void) {
      sntp_setoperatingmode(SNTP_OPMODE_POLL);
      sntp_setservername(0, "pool.ntp.org");  // 推荐使用国内服务器如 "cn.ntp.org"
      sntp_init();

      // 等待同步（最多 10 次重试）
      int retry = 0;
      const int max_retry = 10;
      while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < max_retry) {
          vTaskDelay(pdMS_TO_TICKS(1000));
          retry++;
      }
      time_t now = 0;
      time(&now);  // 获取同步后的系统时间
      printf("System time synced: %s", ctime(&now));
  }
  ```
  **注意**：SNTP 同步仅用于系统时间显示，本组件的任务调度不依赖 `time.h` 的系统时间，而是直接基于 `esp_timer` 的硬件计时，确保调度精度不受网络同步延迟影响。

### 定时精度优化指南

1. **硬件定时器配置**
  ```c
  /* 启用高精度定时器模式（ESP32 专用） */
  esp_timer_create_args_t timer_args = {
      .callback = NULL,
      .arg = NULL,
      .flags = ESP_TIMER_FLAG_HIGH_PRECISION,  // 关键配置，启用 APB 时钟源
  };
  esp_timer_init(&timer_args);
  ```
2. **调度器参数调整**
  ```c
  /* 配置最小调度间隔（默认 1ms，可根据功耗需求调整） */
  #define MIN_SCHEDULE_INTERVAL_US 100000  // 100ms 间隔，降低轻休眠功耗

  cron_start();  // 启动后自动应用配置
  ```
  3. **低功耗场景适配**
  ```c
  /* 结合定时器唤醒实现低功耗调度 */
  esp_sleep_enable_timer_wakeup(5000000);  // 5 秒唤醒一次
  esp_light_sleep_start();
  ```
  此时调度器会在唤醒周期内检查任务触发条件，兼顾精度与功耗。

### 为何任务调度无需依赖时区？

本组件采用 **硬件定时器直接计时** 的调度机制，具有以下优势：

1. **精度隔离**：定时触发基于 ESP32 内部定时器，不受系统时间、时区配置的影响，误差可控制在微秒级
2. **无网络依赖**：无需 SNTP 同步即可实现稳定调度，适合无网络连接的嵌入式场景
3. **功耗优化**：定时器可在 CPU 休眠时独立工作，避免传统系统时间方案的持续唤醒需求

**时区配置的实际作用**：仅影响通过 `localtime` 等函数获取的时间显示格式，与任务调度逻辑完全解耦。

## 特别鸣谢

- [esp_cron](https://github.com/DavidMora/esp_cron) 项目及作者 David Mora Rodriguez
- [ccronexpr](https://github.com/staticlibs/ccronexpr) 表达式解析器

## 许可证

本项目遵循 [Apache License 2.0](http://www.apache.org/licenses/LICENSE-2.0)。
